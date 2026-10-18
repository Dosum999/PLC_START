// LS ELECTRIC XGT FEnet driver (XGT dedicated protocol over TCP, port 2004).
//
// Models: XGK, XGB (XBM/XBC) use XG5000 device notation (D100, D100.A,
// M0010A, T5, ZR100) and IEC direct variables (%DW100); XGI, XGR and XEC use
// IEC direct variables (%MW100, %MD10, %MX5, %IX0.0.3).
//
// Reads are coalesced into continuous (byte block) reads of up to 1400 bytes;
// areas without block support are read with individual requests of up to 16
// variables. Device ID: IP address or host name.

#include <algorithm>
#include <format>
#include <map>
#include <set>

#include "logict/sdk/block_planner.h"
#include "logict/sdk/driver.h"
#include "logict/sdk/tcp.h"
#include "xgt_protocol.h"

namespace {

using namespace logict;
using namespace logict::sdk;
using xgt::XgtAddress;

constexpr const char* kPort = "ls_xgt.DEVICE_PORT";
constexpr const char* kSlot = "ls_xgt.DEVICE_FENET_SLOT";
constexpr const char* kBase = "ls_xgt.DEVICE_FENET_BASE";
constexpr const char* kMaxBlock = "ls_xgt.DEVICE_MAX_BLOCK_BYTES";
constexpr const char* kMaxGap = "ls_xgt.DEVICE_MAX_GAP_BYTES";

class XgtDevice final : public DeviceRuntime {
 public:
  XgtDevice(const DeviceConfig& cfg, HostContext& host) : host_(host) {
    const auto& p = cfg.properties;
    host_name_ = cfg.id;
    port_ = property<std::uint16_t>(p, kPort, 2004);
    model_ = cfg.model.empty() ? "XGK" : cfg.model;
    series_ = xgt::series_of(model_);
    header_.cpu_info = xgt::cpu_info_byte(model_);
    header_.fenet_position = static_cast<std::uint8_t>(((property(p, kBase, 0) & 0xF) << 4) | (property(p, kSlot, 0) & 0xF));
    max_block_ = std::clamp<std::uint32_t>(property<std::uint32_t>(p, kMaxBlock, 1400), 2, 1400);
    max_gap_ = property<std::uint32_t>(p, kMaxGap, 32);
    request_timeout_ = cfg.request_timeout;
    attempts_ = std::max(1, cfg.attempts);
    tcp_.set_observer(host.frames);
  }

  Result<std::shared_ptr<const Address>> parse_address(std::string_view text, TagType requested) override {
    auto a = xgt::parse_address(text, requested, series_);
    if (!a) return std::unexpected(a.error());
    if ((*a)->block && (*a)->byte_len > max_block_) return make_error(status::BadOutOfRange, "tag larger than the block size");
    return std::shared_ptr<const Address>(std::move(*a));
  }

  StatusCode connect(Deadline deadline) override {
    StatusCode st = tcp_.connect(host_name_, port_, deadline);
    if (st != status::Good) host_.log.debug("connect to {}:{} failed: {}", host_name_, port_, tcp_.last_error());
    return st;
  }
  void disconnect() override { tcp_.close(); }
  bool connected() const override { return tcp_.is_open(); }

  StatusCode read(std::span<ReadItem*> items) override {
    const auto now = Clock::now();
    std::vector<PlanItem> plan;
    std::map<std::uint32_t, std::string> area_names;
    std::vector<std::size_t> solo, individual;
    for (std::size_t i = 0; i < items.size(); ++i) {
      const auto& a = addr(*items[i]);
      if (invalid_.contains(&a)) {
        items[i]->quality = status::BadOutOfRange;
      } else if (!a.block) {
        individual.push_back(i);
      } else if (solo_.contains(&a)) {
        solo.push_back(i);
      } else {
        const std::uint32_t area = area_id(a.area);
        area_names[area] = a.area;
        plan.push_back({area, a.byte_offset, a.byte_len, i});
      }
    }

    // Continuous reads of merged blocks.
    for (const auto& b : plan_blocks(plan, [this](std::uint32_t) { return PlanLimits{max_block_, max_gap_}; })) {
      std::vector<std::uint8_t> data;
      std::uint8_t nak = 0;
      StatusCode st = read_block(area_names[b.area], b.start, b.length, data, nak);
      if (!is_good(st)) return st;
      if (nak == 0) {
        for (std::size_t idx : b.items) decode_into(*items[idx], data, b.start, now);
      } else if (b.items.size() == 1) {
        fail(*items[b.items[0]], nak);
      } else {
        // Something in the merged range is not accessible: read these tags one
        // by one from now on so the good ones keep working.
        for (std::size_t idx : b.items) {
          solo_.insert(&addr(*items[idx]));
          solo.push_back(idx);
        }
      }
    }
    for (std::size_t idx : solo) {
      const auto& a = addr(*items[idx]);
      std::vector<std::uint8_t> data;
      std::uint8_t nak = 0;
      StatusCode st = read_block(a.area, a.byte_offset, a.byte_len, data, nak);
      if (!is_good(st)) return st;
      if (nak) fail(*items[idx], nak);
      else decode_into(*items[idx], data, a.byte_offset, now);
    }

    // Individual reads, grouped by data type, at most 16 variables per request.
    std::map<char, std::vector<std::size_t>> by_size;
    for (std::size_t idx : individual) by_size[addr(*items[idx]).size].push_back(idx);
    for (auto& [size, group] : by_size) {
      for (std::size_t start = 0; start < group.size();) {
        std::vector<std::size_t> batch;
        std::size_t vars = 0;
        while (start < group.size()) {
          const std::size_t n = xgt::unit_vars(addr(*items[group[start]])).size();
          if (vars + n > xgt::kMaxBlocks) break;
          vars += n;
          batch.push_back(group[start++]);
        }
        if (StatusCode st = read_individual(items, batch, size, now); !is_good(st)) return st;
      }
    }
    return status::Good;
  }

  StatusCode write(const Address& address, const Value& value) override {
    const auto& a = static_cast<const XgtAddress&>(address);
    if (a.type.scalar == DataType::Boolean) return write_bits(a, value);
    auto data = xgt::encode(a, value);
    if (!data) return data.error().code;
    if (a.block) return write_block(a.area, a.byte_offset, *data);
    // Individual write: one variable per device unit.
    const auto vars = xgt::unit_vars(a);
    const std::size_t unit = data->size() / vars.size();
    std::vector<std::vector<std::uint8_t>> chunks;
    for (std::size_t i = 0; i < vars.size(); ++i)
      chunks.emplace_back(data->begin() + static_cast<std::ptrdiff_t>(i * unit), data->begin() + static_cast<std::ptrdiff_t>((i + 1) * unit));
    return expect_ack(xgt::write_individual(xgt::type_code(a.size), vars, chunks), xgt::kWriteResponse);
  }

 private:
  static const XgtAddress& addr(const ReadItem& item) { return static_cast<const XgtAddress&>(*item.address); }

  std::uint32_t area_id(const std::string& area) {
    auto [it, inserted] = area_ids_.try_emplace(area, static_cast<std::uint32_t>(area_ids_.size()));
    return it->second;
  }

  void fail(ReadItem& item, std::uint8_t nak) {
    item.quality = status::BadOutOfRange;
    invalid_.insert(&addr(item));
    host_.log.warn("address '{}' rejected by PLC (error code 0x{:02X}); tag deactivated", item.address->text, nak);
  }

  void decode_into(ReadItem& item, const std::vector<std::uint8_t>& data, std::uint64_t base, Timestamp now) {
    const auto& a = addr(item);
    auto v = xgt::decode(a, std::span(data).subspan(static_cast<std::size_t>(a.byte_offset - base)));
    if (v) {
      item.value = std::move(*v);
      item.quality = status::Good;
    } else {
      item.quality = v.error().code;
    }
    item.source_time = now;
  }

  StatusCode read_block(const std::string& area, std::uint64_t offset, std::uint32_t length, std::vector<std::uint8_t>& out,
                        std::uint8_t& nak) {
    xgt::Response r;
    StatusCode st = transact(xgt::read_continuous(std::format("%{}B{}", area, offset), static_cast<std::uint16_t>(length)),
                             xgt::kReadResponse, true, r);
    if (!is_good(st)) return st;
    if (r.nak()) {
      nak = r.error_code ? r.error_code : 0xFF;
      return status::Good;
    }
    if (r.data.size() != 1 || r.data[0].size() != length) return status::BadCommunicationError;
    out = std::move(r.data[0]);
    return status::Good;
  }

  StatusCode read_individual(std::span<ReadItem*> items, const std::vector<std::size_t>& batch, char size, Timestamp now) {
    std::vector<std::string> vars;
    for (std::size_t idx : batch)
      for (auto& v : xgt::unit_vars(addr(*items[idx]))) vars.push_back(std::move(v));
    xgt::Response r;
    StatusCode st = transact(xgt::read_individual(xgt::type_code(size), vars), xgt::kReadResponse, true, r);
    if (!is_good(st)) return st;
    if (r.nak()) {
      if (batch.size() == 1) {
        fail(*items[batch[0]], r.error_code ? r.error_code : 0xFF);
        return status::Good;
      }
      for (std::size_t idx : batch)  // isolate the bad one(s)
        if (st = read_individual(items, {idx}, size, now); !is_good(st)) return st;
      return status::Good;
    }
    if (r.data.size() != vars.size()) return status::BadCommunicationError;
    std::size_t k = 0;
    for (std::size_t idx : batch) {
      const auto& a = addr(*items[idx]);
      std::vector<std::uint8_t> bytes;
      const std::size_t n = xgt::unit_vars(a).size();
      for (std::size_t j = 0; j < n; ++j, ++k) {
        // Bit reads return one byte whose lowest bit is the value.
        if (size == 'X') bytes.push_back(r.data[k].empty() ? 0 : static_cast<std::uint8_t>(r.data[k][0] & 1));
        else bytes.insert(bytes.end(), r.data[k].begin(), r.data[k].end());
      }
      if (a.size == 'X') {
        // Individual bit reads return the value in bit 0 of their byte.
        XgtAddress at_bit0 = a;
        at_bit0.bit = 0;
        at_bit0.byte_offset = 0;
        auto v = xgt::decode(at_bit0, bytes);
        items[idx]->quality = v ? status::Good : v.error().code;
        if (v) items[idx]->value = std::move(*v);
        items[idx]->source_time = now;
      } else {
        decode_into(*items[idx], bytes, a.byte_offset, now);
      }
    }
    return status::Good;
  }

  StatusCode write_block(const std::string& area, std::uint64_t offset, std::span<const std::uint8_t> data) {
    return expect_ack(xgt::write_continuous(std::format("%{}B{}", area, offset), data), xgt::kWriteResponse);
  }

  StatusCode write_bits(const XgtAddress& a, const Value& value) {
    std::vector<bool> bits;
    if (const auto* arr = value.get_if<Array>()) {
      for (const auto& e : *arr) {
        auto b = convert(e, DataType::Boolean);
        if (!b) return b.error().code;
        bits.push_back(*b->get_if<bool>());
      }
    } else {
      auto b = convert(value, DataType::Boolean);
      if (!b) return b.error().code;
      bits.push_back(*b->get_if<bool>());
    }
    if (a.bit_write_direct && bits.size() <= xgt::kMaxBlocks) {
      std::vector<std::string> vars;
      std::vector<std::vector<std::uint8_t>> data;
      if (a.type.is_array()) {
        for (std::size_t i = 0; i < bits.size(); ++i) vars.push_back(std::format("%{}X{}", a.area, a.bit_index + i));
      } else {
        vars.push_back(a.var);
      }
      for (bool b : bits) data.push_back({static_cast<std::uint8_t>(b ? 1 : 0)});
      return expect_ack(xgt::write_individual(xgt::kTypeBit, vars, data), xgt::kWriteResponse);
    }
    if (!a.block) return status::BadNotSupported;
    // Read-modify-write of the bytes holding the bits (e.g. D100.A on XGK).
    std::vector<std::uint8_t> cur;
    std::uint8_t nak = 0;
    if (StatusCode st = read_block(a.area, a.byte_offset, a.byte_len, cur, nak); !is_good(st)) return st;
    if (nak) return status::BadOutOfRange;
    for (std::size_t i = 0; i < bits.size(); ++i) {
      const std::size_t b = static_cast<std::size_t>(a.bit) + i;
      if (bits[i]) cur[b / 8] = static_cast<std::uint8_t>(cur[b / 8] | (1u << (b % 8)));
      else cur[b / 8] = static_cast<std::uint8_t>(cur[b / 8] & ~(1u << (b % 8)));
    }
    return write_block(a.area, a.byte_offset, cur);
  }

  StatusCode expect_ack(const std::vector<std::uint8_t>& body, std::uint16_t response) {
    xgt::Response r;
    StatusCode st = transact(body, response, false, r);
    if (!is_good(st)) return st;
    if (r.nak()) {
      host_.log.warn("write rejected by PLC (error code 0x{:02X})", r.error_code);
      return status::BadOutOfRange;
    }
    return status::Good;
  }

  // Sends one request and waits for the response with the same invoke ID,
  // discarding late answers to earlier requests. Retries on timeout.
  StatusCode transact(const std::vector<std::uint8_t>& body, std::uint16_t expect, bool has_data, xgt::Response& out) {
    if (!tcp_.is_open()) return status::BadNotConnected;
    for (int attempt = 0; attempt < attempts_; ++attempt) {
      header_.invoke_id = ++invoke_;
      const auto req = xgt::frame(header_, body);
      const Deadline deadline = SteadyClock::now() + request_timeout_;
      StatusCode st = tcp_.send_all(req, deadline);
      while (is_good(st)) {
        std::uint8_t hdr[xgt::kHeaderSize];
        st = tcp_.recv_exact(hdr, deadline);
        if (!is_good(st)) break;
        auto h = xgt::parse_header(hdr);
        if (!h || h->length < 8 || h->length > 4096) {
          tcp_.close();  // framing lost
          return status::BadCommunicationError;
        }
        std::vector<std::uint8_t> resp(h->length);
        st = tcp_.recv_exact(resp, deadline);
        if (!is_good(st)) break;
        if (h->invoke_id != header_.invoke_id) continue;  // stale answer
        auto r = xgt::parse_body(resp, has_data);
        if (!r) return r.error().code;
        if (r->command != expect) return status::BadCommunicationError;
        out = std::move(*r);
        return status::Good;
      }
      if (st != status::BadTimeout) return st;
      host_.log.debug("request timeout ({} of {})", attempt + 1, attempts_);
    }
    return status::BadTimeout;
  }

  HostContext& host_;
  std::string host_name_, model_;
  std::uint16_t port_ = 2004;
  xgt::Series series_ = xgt::Series::XGK;
  xgt::Header header_;
  std::uint32_t max_block_ = 1400, max_gap_ = 32;
  std::chrono::milliseconds request_timeout_{1000};
  int attempts_ = 3;
  TcpClient tcp_;
  std::uint16_t invoke_ = 0;
  std::map<std::string, std::uint32_t> area_ids_;
  std::set<const XgtAddress*> invalid_, solo_;
};

nlohmann::json prop(const char* key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

class XgtDriver final : public Driver {
 public:
  DriverInfo info() const override {
    return {"ls_xgt", "LS XGT FEnet", "0.1.0", Medium::Ethernet, {"XGK", "XGB", "XGI", "XGR", "XEC"}};
  }

  nlohmann::json schema() const override {
    using nlohmann::json;
    return {
        {"driver", "ls_xgt"},
        {"display_name", "LS XGT FEnet"},
        {"channel", json::array()},
        {"device",
         json::array({
             prop("servermain.DEVICE_MODEL", "enum", "XGK", "XGK, XGB (XBM/XBC), XGI, XGR, XEC"),
             prop("servermain.DEVICE_ID_STRING", "string", "192.168.0.10", "FEnet module IP address or host name"),
             prop(kPort, "integer", 2004, "XGT dedicated TCP port"),
             prop(kBase, "integer", 0, "Base number of the FEnet module"),
             prop(kSlot, "integer", 0, "Slot number of the FEnet module"),
             prop(kMaxBlock, "integer", 1400, "Bytes per continuous read (2-1400)"),
             prop(kMaxGap, "integer", 32, "Unused bytes allowed between merged tags"),
         })},
        {"address_help",
         "XGK/XGB: D100, D100.A (bit), M0010A (bit), T5 (Word = current value, Boolean = contact), ZR100, D100.20 (String). "
         "XGI/XGR/XEC: %MW100, %MD10, %MX5, %MW100.3, %IX0.0.3, %QW0.1.0. Arrays: [n] or [r][c]."},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig&, const DeviceConfig& device,
                                                       HostContext& host) override {
    if (device.id.empty()) return make_error(status::BadConfigurationError, "device ID (IP address) is empty");
    return std::make_unique<XgtDevice>(device, host);
  }
};

}  // namespace

LOGICT_DECLARE_DRIVER(XgtDriver)
