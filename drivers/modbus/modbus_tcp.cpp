// Modbus TCP/IP Ethernet driver (Kepware "Modbus TCP/IP Ethernet" compatible
// addressing and device properties).
//
// Device ID: "<ip>", "<ip>.<unit>" (Kepware style, e.g. 192.168.0.10.1) or
// "<host>.<unit>" with the host in angle brackets, e.g. "<plc1.local>.1".

#include <algorithm>
#include <charconv>
#include <format>
#include <set>

#include "logict/sdk/block_planner.h"
#include "logict/sdk/driver.h"
#include "logict/sdk/tcp.h"
#include "protocol.h"

namespace {

using namespace logict;
using namespace logict::sdk;
using modbus::Area;
using modbus::ModbusAddress;

constexpr const char* kPort = "modbus_tcp.DEVICE_PORT";
constexpr const char* kUnit = "modbus_tcp.DEVICE_UNIT_ID";
constexpr const char* kZeroBased = "modbus_tcp.DEVICE_ZERO_BASED_ADDRESSING";
constexpr const char* kZeroBasedBits = "modbus_tcp.DEVICE_ZERO_BASED_BIT_ADDRESSING";
constexpr const char* kByteOrder = "modbus_tcp.DEVICE_MODBUS_BYTE_ORDER";
constexpr const char* kFirstWordLow = "modbus_tcp.DEVICE_FIRST_WORD_LOW";
constexpr const char* kFirstDWordLow = "modbus_tcp.DEVICE_FIRST_DWORD_LOW";
constexpr const char* kUseFc05 = "modbus_tcp.DEVICE_USE_FC05";
constexpr const char* kUseFc06 = "modbus_tcp.DEVICE_USE_FC06";
constexpr const char* kMaskWrite = "modbus_tcp.DEVICE_HOLDING_REGISTER_MASK_WRITES";
constexpr const char* kMaxCoils = "modbus_tcp.DEVICE_MAX_COILS_PER_REQUEST";
constexpr const char* kMaxRegisters = "modbus_tcp.DEVICE_MAX_REGISTERS_PER_REQUEST";
constexpr const char* kCoilGap = "modbus_tcp.DEVICE_MAX_COIL_GAP";
constexpr const char* kRegisterGap = "modbus_tcp.DEVICE_MAX_REGISTER_GAP";

struct Endpoint {
  std::string host;
  std::uint8_t unit = 1;
};

Result<Endpoint> parse_device_id(std::string_view id, int default_unit) {
  Endpoint ep;
  ep.unit = static_cast<std::uint8_t>(default_unit);
  auto parse_unit = [](std::string_view s) -> std::optional<std::uint8_t> {
    unsigned v;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || v > 255) return std::nullopt;
    return static_cast<std::uint8_t>(v);
  };
  if (!id.empty() && id.front() == '<') {
    auto close = id.find('>');
    if (close == std::string_view::npos) return make_error(status::BadConfigurationError, "missing '>' in device ID");
    ep.host = std::string(id.substr(1, close - 1));
    auto rest = id.substr(close + 1);
    if (!rest.empty()) {
      if (rest.front() != '.') return make_error(status::BadConfigurationError, "expected '.<unit>' after host");
      auto u = parse_unit(rest.substr(1));
      if (!u) return make_error(status::BadConfigurationError, "unit ID must be 0..255");
      ep.unit = *u;
    }
  } else {
    std::vector<std::string_view> parts;
    for (std::size_t start = 0;;) {
      auto dot = id.find('.', start);
      parts.push_back(id.substr(start, dot - start));
      if (dot == std::string_view::npos) break;
      start = dot + 1;
    }
    if (parts.size() == 5) {
      auto u = parse_unit(parts[4]);
      if (!u) return make_error(status::BadConfigurationError, "unit ID must be 0..255");
      ep.unit = *u;
      ep.host = std::string(id.substr(0, id.rfind('.')));
    } else {
      ep.host = std::string(id);
    }
  }
  if (ep.host.empty()) return make_error(status::BadConfigurationError, "device ID has no host");
  return ep;
}

class ModbusTcpDevice final : public DeviceRuntime {
 public:
  ModbusTcpDevice(Endpoint ep, const DeviceConfig& cfg, HostContext& host) : ep_(std::move(ep)), host_(host) {
    const auto& p = cfg.properties;
    port_ = property<std::uint16_t>(p, kPort, 502);
    addr_opt_.zero_based = property(p, kZeroBased, true);
    addr_opt_.zero_based_bits = property(p, kZeroBasedBits, true);
    enc_.modbus_byte_order = property(p, kByteOrder, true);
    enc_.first_word_low = property(p, kFirstWordLow, true);
    enc_.first_dword_low = property(p, kFirstDWordLow, true);
    use_fc05_ = property(p, kUseFc05, true);
    use_fc06_ = property(p, kUseFc06, true);
    mask_write_ = property(p, kMaskWrite, false);
    max_coils_ = std::clamp<std::uint32_t>(property<std::uint32_t>(p, kMaxCoils, 2000), 8, 2000);
    max_regs_ = std::clamp<std::uint32_t>(property<std::uint32_t>(p, kMaxRegisters, 120), 1, 125);
    coil_gap_ = property<std::uint32_t>(p, kCoilGap, 64);
    reg_gap_ = property<std::uint32_t>(p, kRegisterGap, 10);
    request_timeout_ = cfg.request_timeout;
    attempts_ = std::max(1, cfg.attempts);
    tcp_.set_observer(host.frames);
  }

  Result<std::shared_ptr<const Address>> parse_address(std::string_view text, TagType requested) override {
    auto a = modbus::parse_address(text, requested, addr_opt_);
    if (!a) return std::unexpected(a.error());
    return std::shared_ptr<const Address>(std::move(*a));
  }

  StatusCode connect(Deadline deadline) override {
    StatusCode st = tcp_.connect(ep_.host, port_, deadline);
    if (st != status::Good)
      host_.log.debug("connect to {}:{} failed: {}", ep_.host, port_, tcp_.last_error());
    return st;
  }
  void disconnect() override { tcp_.close(); }
  bool connected() const override { return tcp_.is_open(); }

  StatusCode read(std::span<ReadItem*> items) override {
    const auto now = Clock::now();
    std::vector<PlanItem> plan;
    plan.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
      const auto& a = addr(*items[i]);
      if (invalid_.contains(&a)) {
        items[i]->quality = status::BadOutOfRange;
        continue;
      }
      plan.push_back({static_cast<std::uint32_t>(a.area), a.offset, a.units(), i});
    }
    auto blocks = split_at_bad_gaps(plan_blocks(plan, [this](std::uint32_t area) {
      return modbus::is_bit_area(static_cast<Area>(area)) ? PlanLimits{max_coils_, coil_gap_}
                                                          : PlanLimits{max_regs_, reg_gap_};
    }), items);

    for (const auto& b : blocks) {
      const auto area = static_cast<Area>(b.area);
      std::vector<std::uint8_t> data;
      std::uint8_t exception = 0;
      StatusCode st = read_range(area, static_cast<std::uint32_t>(b.start), b.length, data, exception);
      if (!is_good(st)) return st;  // communication failure: abort the whole transaction

      if (exception == 0) {
        for (std::size_t idx : b.items) decode(*items[idx], data, static_cast<std::uint32_t>(b.start), now);
        continue;
      }
      if (exception != 0x02 || b.items.size() == 1) {
        for (std::size_t idx : b.items) fail_item(*items[idx], exception);
        continue;
      }
      // Illegal address somewhere in a merged block: isolate the bad tags so
      // the good ones keep working (bad ones are skipped from now on), and
      // stop merging across the unused gaps of this block (the device may
      // not implement them).
      remember_gaps(b, items);
      for (std::size_t idx : b.items) {
        const auto& a = addr(*items[idx]);
        std::vector<std::uint8_t> one;
        std::uint8_t exc = 0;
        st = read_range(a.area, a.offset, a.units(), one, exc);
        if (!is_good(st)) return st;
        if (exc == 0) {
          decode(*items[idx], one, a.offset, now);
        } else {
          fail_item(*items[idx], exc);
        }
      }
    }
    return status::Good;
  }

  StatusCode write(const Address& address, const Value& value) override {
    const auto& a = static_cast<const ModbusAddress&>(address);
    switch (a.area) {
      case Area::DiscreteInput:
      case Area::InputRegister: return status::BadNotWritable;
      case Area::Coil: return write_coils(a, value);
      case Area::HoldingRegister: return a.bit >= 0 ? write_register_bit(a, value) : write_registers(a, value);
    }
    return status::BadNotSupported;
  }

 private:
  static const ModbusAddress& addr(const ReadItem& item) { return static_cast<const ModbusAddress&>(*item.address); }

  using Unit = std::pair<std::uint32_t, std::uint32_t>;  // (area, address)

  // Records the unused ranges between the items of a block as "do not span".
  void remember_gaps(const Block& b, std::span<ReadItem*> items) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    for (std::size_t idx : b.items) ranges.emplace_back(addr(*items[idx]).offset, addr(*items[idx]).offset + addr(*items[idx]).units());
    std::sort(ranges.begin(), ranges.end());
    std::uint32_t covered = ranges.front().second;
    for (std::size_t i = 1; i < ranges.size(); ++i) {
      for (std::uint32_t u = covered; u < ranges[i].first; ++u) no_span_.insert({b.area, u});
      covered = std::max(covered, ranges[i].second);
    }
  }

  // Splits planned blocks so none spans a remembered bad gap.
  std::vector<Block> split_at_bad_gaps(std::vector<Block> blocks, std::span<ReadItem*> items) const {
    if (no_span_.empty()) return blocks;
    std::vector<Block> out;
    for (auto& b : blocks) {
      std::vector<std::size_t> order = b.items;
      std::sort(order.begin(), order.end(),
                [&](std::size_t x, std::size_t y) { return addr(*items[x]).offset < addr(*items[y]).offset; });
      Block cur{};
      bool open = false;
      for (std::size_t idx : order) {
        const auto& a = addr(*items[idx]);
        const std::uint32_t end = a.offset + a.units();
        if (open) {
          const std::uint32_t cur_end = static_cast<std::uint32_t>(cur.start) + cur.length;
          auto it = no_span_.lower_bound({b.area, cur_end});
          const bool blocked = it != no_span_.end() && it->first == b.area && it->second < a.offset;
          if (!blocked) {
            cur.length = std::max(cur_end, end) - static_cast<std::uint32_t>(cur.start);
            cur.items.push_back(idx);
            continue;
          }
          out.push_back(std::move(cur));
        }
        cur = Block{b.area, a.offset, a.units(), {idx}, a.units() > (modbus::is_bit_area(static_cast<Area>(b.area)) ? max_coils_ : max_regs_)};
        open = true;
      }
      if (open) out.push_back(std::move(cur));
    }
    return out;
  }

  void fail_item(ReadItem& item, std::uint8_t exception) {
    item.quality = modbus::exception_status(exception);
    if (exception == 0x02) {
      const auto& a = addr(item);
      invalid_.insert(&a);
      for (std::uint32_t u = a.offset; u < a.offset + a.units(); ++u)
        no_span_.insert({static_cast<std::uint32_t>(a.area), u});
      host_.log.warn("address '{}' rejected by device (illegal data address); tag deactivated", item.address->text);
    }
  }

  void decode(ReadItem& item, const std::vector<std::uint8_t>& data, std::uint32_t base, Timestamp now) {
    const auto& a = addr(item);
    const std::uint32_t rel = a.offset - base;
    if (modbus::is_bit_area(a.area)) {
      item.value = modbus::decode_bits(a, data, rel);
      item.quality = status::Good;
    } else {
      auto v = modbus::decode_registers(a, std::span(data).subspan(rel * 2), enc_);
      if (v) {
        item.value = std::move(*v);
        item.quality = status::Good;
      } else {
        item.quality = v.error().code;
      }
    }
    item.source_time = now;
  }

  // Reads `count` units starting at `start`, splitting into protocol-sized
  // requests. Register data is returned as wire bytes, coils as packed bits.
  StatusCode read_range(Area area, std::uint32_t start, std::uint32_t count, std::vector<std::uint8_t>& out,
                        std::uint8_t& exception) {
    const bool bits = modbus::is_bit_area(area);
    const std::uint32_t max = bits ? max_coils_ : max_regs_;
    out.assign(bits ? (count + 7) / 8 : count * 2, 0);
    for (std::uint32_t done = 0; done < count;) {
      const std::uint32_t n = std::min(max, count - done);
      std::vector<std::uint8_t> resp;
      StatusCode st = transact(modbus::make_read(area, static_cast<std::uint16_t>(start + done), static_cast<std::uint16_t>(n)), resp);
      if (!is_good(st)) return st;
      if (resp[0] & 0x80) {
        exception = resp.size() > 1 ? resp[1] : 0xFF;
        return status::Good;
      }
      const std::size_t expect = bits ? (n + 7) / 8 : n * 2;
      if (resp.size() < 2 || resp[1] != expect || resp.size() != 2 + expect) return status::BadCommunicationError;
      if (bits) {
        for (std::uint32_t i = 0; i < n; ++i)
          if ((resp[2 + i / 8] >> (i % 8)) & 1) out[(done + i) / 8] |= static_cast<std::uint8_t>(1u << ((done + i) % 8));
      } else {
        std::copy(resp.begin() + 2, resp.end(), out.begin() + done * 2);
      }
      done += n;
    }
    return status::Good;
  }

  StatusCode write_coils(const ModbusAddress& a, const Value& v) {
    std::vector<bool> bits;
    if (const auto* arr = v.get_if<Array>()) {
      for (const auto& e : *arr) {
        auto b = convert(e, DataType::Boolean);
        if (!b) return b.error().code;
        bits.push_back(*b->get_if<bool>());
      }
    } else {
      auto b = convert(v, DataType::Boolean);
      if (!b) return b.error().code;
      bits.push_back(*b->get_if<bool>());
    }
    std::vector<std::uint8_t> pdu;
    if (bits.size() == 1 && use_fc05_) {
      pdu = {modbus::kWriteSingleCoil};
      modbus::put16(pdu, static_cast<std::uint16_t>(a.offset));
      modbus::put16(pdu, bits[0] ? 0xFF00 : 0x0000);
    } else {
      pdu = {modbus::kWriteMultipleCoils};
      modbus::put16(pdu, static_cast<std::uint16_t>(a.offset));
      modbus::put16(pdu, static_cast<std::uint16_t>(bits.size()));
      pdu.push_back(static_cast<std::uint8_t>((bits.size() + 7) / 8));
      std::vector<std::uint8_t> packed((bits.size() + 7) / 8, 0);
      for (std::size_t i = 0; i < bits.size(); ++i)
        if (bits[i]) packed[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
      pdu.insert(pdu.end(), packed.begin(), packed.end());
    }
    return write_pdu(pdu);
  }

  StatusCode write_registers(const ModbusAddress& a, const Value& v) {
    auto data = modbus::encode_registers(a, v, enc_);
    if (!data) return data.error().code;
    const std::size_t regs = data->size() / 2;
    if (regs > 123) return status::BadOutOfRange;  // FC16 limit
    std::vector<std::uint8_t> pdu;
    if (regs == 1 && use_fc06_) {
      pdu = {modbus::kWriteSingleRegister};
      modbus::put16(pdu, static_cast<std::uint16_t>(a.offset));
    } else {
      pdu = {modbus::kWriteMultipleRegisters};
      modbus::put16(pdu, static_cast<std::uint16_t>(a.offset));
      modbus::put16(pdu, static_cast<std::uint16_t>(regs));
      pdu.push_back(static_cast<std::uint8_t>(data->size()));
    }
    pdu.insert(pdu.end(), data->begin(), data->end());
    return write_pdu(pdu);
  }

  StatusCode write_register_bit(const ModbusAddress& a, const Value& v) {
    auto b = convert(v, DataType::Boolean);
    if (!b) return b.error().code;
    const bool on = *b->get_if<bool>();
    // Bit numbering refers to the register value, so translate through the
    // configured byte order to find the bit on the wire.
    const std::uint16_t value_mask = static_cast<std::uint16_t>(1u << a.bit);
    const std::uint16_t wire_mask = enc_.modbus_byte_order ? value_mask : static_cast<std::uint16_t>((value_mask >> 8) | (value_mask << 8));
    std::vector<std::uint8_t> pdu;
    if (mask_write_) {
      pdu = {modbus::kMaskWriteRegister};
      modbus::put16(pdu, static_cast<std::uint16_t>(a.offset));
      modbus::put16(pdu, static_cast<std::uint16_t>(~wire_mask));       // AND mask
      modbus::put16(pdu, on ? wire_mask : std::uint16_t{0});              // OR mask
      return write_pdu(pdu);
    }
    std::vector<std::uint8_t> cur;
    std::uint8_t exc = 0;
    if (auto st = read_range(Area::HoldingRegister, a.offset, 1, cur, exc); !is_good(st)) return st;
    if (exc) return modbus::exception_status(exc);
    std::uint16_t reg = modbus::get16(cur.data());
    reg = on ? (reg | wire_mask) : (reg & static_cast<std::uint16_t>(~wire_mask));
    pdu = {use_fc06_ ? modbus::kWriteSingleRegister : modbus::kWriteMultipleRegisters};
    modbus::put16(pdu, static_cast<std::uint16_t>(a.offset));
    if (!use_fc06_) {
      modbus::put16(pdu, 1);
      pdu.push_back(2);
    }
    modbus::put16(pdu, reg);
    return write_pdu(pdu);
  }

  StatusCode write_pdu(const std::vector<std::uint8_t>& pdu) {
    std::vector<std::uint8_t> resp;
    if (auto st = transact(pdu, resp); !is_good(st)) return st;
    if (resp[0] & 0x80) return modbus::exception_status(resp.size() > 1 ? resp[1] : 0xFF);
    if (resp[0] != pdu[0] || resp.size() < 5) return status::BadCommunicationError;
    return status::Good;
  }

  // Sends one request and returns the matching response PDU. Retries on
  // timeout up to the configured attempts; discards stale responses.
  StatusCode transact(const std::vector<std::uint8_t>& pdu, std::vector<std::uint8_t>& resp) {
    if (!tcp_.is_open()) return status::BadNotConnected;
    for (int attempt = 0; attempt < attempts_; ++attempt) {
      const std::uint16_t tid = ++tid_;
      std::vector<std::uint8_t> adu;
      adu.reserve(7 + pdu.size());
      modbus::put16(adu, tid);
      modbus::put16(adu, 0);
      modbus::put16(adu, static_cast<std::uint16_t>(pdu.size() + 1));
      adu.push_back(ep_.unit);
      adu.insert(adu.end(), pdu.begin(), pdu.end());

      const Deadline deadline = SteadyClock::now() + request_timeout_;
      StatusCode st = tcp_.send_all(adu, deadline);
      while (is_good(st)) {
        std::uint8_t hdr[7];
        st = tcp_.recv_exact(hdr, deadline);
        if (!is_good(st)) break;
        const std::uint16_t len = modbus::get16(hdr + 4);
        if (modbus::get16(hdr + 2) != 0 || len < 2 || len > 254) {
          tcp_.close();  // framing lost
          return status::BadCommunicationError;
        }
        resp.resize(len - 1u);
        st = tcp_.recv_exact(resp, deadline);
        if (!is_good(st)) break;
        if (modbus::get16(hdr) == tid) {
          if ((resp[0] & 0x7F) != pdu[0]) return status::BadCommunicationError;
          return status::Good;
        }
        // Late answer to an earlier (timed out) request: keep waiting for ours.
      }
      if (st != status::BadTimeout) return st;
      host_.log.debug("request timeout ({} of {})", attempt + 1, attempts_);
    }
    return status::BadTimeout;
  }

  Endpoint ep_;
  HostContext& host_;
  std::uint16_t port_ = 502;
  modbus::AddressOptions addr_opt_;
  modbus::Encoding enc_;
  bool use_fc05_ = true, use_fc06_ = true, mask_write_ = false;
  std::uint32_t max_coils_ = 2000, max_regs_ = 120, coil_gap_ = 64, reg_gap_ = 10;
  std::chrono::milliseconds request_timeout_{1000};
  int attempts_ = 3;
  TcpClient tcp_;
  std::uint16_t tid_ = 0;
  std::set<const ModbusAddress*> invalid_;
  std::set<Unit> no_span_;
};

nlohmann::json prop(const char* key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

class ModbusTcpDriver final : public Driver {
 public:
  DriverInfo info() const override {
    return {"modbus_tcp", "Modbus TCP/IP Ethernet", "0.1.0", Medium::Ethernet, {"Modbus"}};
  }

  nlohmann::json schema() const override {
    using nlohmann::json;
    return {
        {"driver", "modbus_tcp"},
        {"display_name", "Modbus TCP/IP Ethernet"},
        {"channel", json::array()},
        {"device",
         json::array({
             prop("servermain.DEVICE_ID_STRING", "string", "127.0.0.1",
                  "IP address, optionally with unit ID (192.168.0.10.1) or <hostname>.unit"),
             prop(kPort, "integer", 502, "TCP port"),
             prop(kUnit, "integer", 1, "Unit ID when not given in the device ID (0-255)"),
             prop(kZeroBased, "boolean", true, "Address 40001 is protocol register 0"),
             prop(kZeroBasedBits, "boolean", true, "Register bits are numbered 0-15 (else 1-16)"),
             prop(kByteOrder, "boolean", true, "High byte first within a register"),
             prop(kFirstWordLow, "boolean", true, "Low word first in 32-bit values"),
             prop(kFirstDWordLow, "boolean", true, "Low double word first in 64-bit values"),
             prop(kUseFc05, "boolean", true, "Write single coils with function 05 (else 15)"),
             prop(kUseFc06, "boolean", true, "Write single registers with function 06 (else 16)"),
             prop(kMaskWrite, "boolean", false, "Write register bits with function 22 (else read-modify-write)"),
             prop(kMaxCoils, "integer", 2000, "Coils per read request (8-2000)"),
             prop(kMaxRegisters, "integer", 120, "Registers per read request (1-125)"),
             prop(kCoilGap, "integer", 64, "Unused coils allowed between merged tags"),
             prop(kRegisterGap, "integer", 10, "Unused registers allowed between merged tags"),
         })},
        {"address_help",
         "0xxxxx coil, 1xxxxx discrete input, 3xxxxx input register, 4xxxxx holding register "
         "(5 or 6 digits). Suffixes: .b register bit, .nH/.nL string, [r] / [r][c] arrays."},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig&, const DeviceConfig& device,
                                                       HostContext& host) override {
    auto ep = parse_device_id(device.id, property(device.properties, kUnit, 1));
    if (!ep) return std::unexpected(ep.error());
    return std::make_unique<ModbusTcpDevice>(std::move(*ep), device, host);
  }
};

}  // namespace

LOGICT_DECLARE_DRIVER(ModbusTcpDriver)
