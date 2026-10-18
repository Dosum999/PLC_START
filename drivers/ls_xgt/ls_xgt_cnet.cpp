// LS XGT Cnet driver: XGT dedicated protocol over RS-232C/RS-422/485 (Cnet
// modules and XGB built-in Cnet). The channel is the serial line; each device
// is one station (device ID 0-31). Addresses are the same as the FEnet driver:
// XGK/XGB D100, M0010A, D100.A, T5 ...; XEC/XGI %MW100, %MX5 ...
//
// Reads are coalesced into continuous word reads (max 60 words per frame);
// areas without block support use individual reads (max 16 variables).

#include <algorithm>
#include <charconv>
#include <format>
#include <map>
#include <mutex>
#include <set>

#include "cnet_protocol.h"
#include "logict/sdk/block_planner.h"
#include "logict/sdk/driver.h"
#include "logict/sdk/serial.h"
#include "xgt_protocol.h"

namespace {

using namespace logict;
using namespace logict::sdk;
using xgt::XgtAddress;
namespace cnet = xgt::cnet;

constexpr const char* kBcc = "ls_xgt_cnet.DEVICE_USE_BCC";
constexpr const char* kMaxWords = "ls_xgt_cnet.DEVICE_MAX_WORDS_PER_READ";
constexpr const char* kGap = "ls_xgt_cnet.DEVICE_MAX_GAP_WORDS";

struct Line {
  SerialSettings settings;
  SerialPort port;
};

// Device bytes are little endian per word; frames carry each unit most
// significant digit first.
std::vector<std::uint8_t> unit_to_le(std::span<const std::uint8_t> text_order, std::size_t unit) {
  std::vector<std::uint8_t> out(text_order.begin(), text_order.end());
  for (std::size_t i = 0; i + unit <= out.size(); i += unit) std::reverse(out.begin() + static_cast<std::ptrdiff_t>(i), out.begin() + static_cast<std::ptrdiff_t>(i + unit));
  return out;
}

class CnetDevice final : public DeviceRuntime {
 public:
  CnetDevice(std::shared_ptr<Line> line, std::uint8_t station, const DeviceConfig& cfg, HostContext& host)
      : line_(std::move(line)), station_(station), host_(host) {
    model_ = cfg.model.empty() ? "XGB" : cfg.model;
    series_ = xgt::series_of(model_);
    bcc_ = property(cfg.properties, kBcc, true);
    max_words_ = std::clamp<std::uint32_t>(property<std::uint32_t>(cfg.properties, kMaxWords, 60), 1, cnet::kMaxContinuousWords);
    gap_ = property<std::uint32_t>(cfg.properties, kGap, 8);
    request_timeout_ = cfg.request_timeout;
    attempts_ = std::max(1, cfg.attempts);
    line_->port.set_observer(host.frames);
  }

  Result<std::shared_ptr<const Address>> parse_address(std::string_view text, TagType requested) override {
    auto a = xgt::parse_address(text, requested, series_);
    if (!a) return std::unexpected(a.error());
    if ((*a)->block && ((*a)->byte_len + 3) / 2 > max_words_)
      return make_error(status::BadOutOfRange, std::format("tag larger than {} words", max_words_));
    return std::shared_ptr<const Address>(std::move(*a));
  }

  StatusCode connect(Deadline) override {
    if (!line_->port.is_open()) {
      if (StatusCode st = line_->port.open(line_->settings); !is_good(st)) {
        host_.log.warn("cannot open serial port: {}", line_->port.last_error());
        return st;
      }
      host_.log.info("opened {} ({} {}{}{})", line_->settings.port, line_->settings.baud, line_->settings.data_bits,
                     line_->settings.parity, line_->settings.stop_bits);
    }
    online_ = true;
    return status::Good;
  }
  void disconnect() override { online_ = false; }  // keep the shared line open for other stations
  bool connected() const override { return online_ && line_->port.is_open(); }

  StatusCode read(std::span<ReadItem*> items) override {
    const auto now = Clock::now();
    std::vector<PlanItem> plan;
    std::map<std::uint32_t, std::string> areas;
    std::vector<std::size_t> individual, solo;
    for (std::size_t i = 0; i < items.size(); ++i) {
      const auto& a = addr(*items[i]);
      if (invalid_.contains(&a)) {
        items[i]->quality = status::BadOutOfRange;
      } else if (!a.block) {
        individual.push_back(i);
      } else if (solo_.contains(&a)) {
        solo.push_back(i);
      } else {
        const auto [w0, n] = words(a);
        const std::uint32_t area = area_id(a.area);
        areas[area] = a.area;
        plan.push_back({area, w0, n, i});
      }
    }
    for (const auto& b : plan_blocks(plan, [this](std::uint32_t) { return PlanLimits{max_words_, gap_}; })) {
      std::vector<std::uint8_t> data;
      std::uint16_t nak = 0;
      if (StatusCode st = read_words(areas[b.area], static_cast<std::uint32_t>(b.start), b.length, data, nak); !is_good(st)) return st;
      if (!nak) {
        for (std::size_t idx : b.items) decode_into(*items[idx], data, b.start * 2, now);
      } else if (b.items.size() == 1) {
        fail(*items[b.items[0]], nak);
      } else {
        for (std::size_t idx : b.items) {
          solo_.insert(&addr(*items[idx]));
          solo.push_back(idx);
        }
      }
    }
    for (std::size_t idx : solo) {
      const auto& a = addr(*items[idx]);
      const auto [w0, n] = words(a);
      std::vector<std::uint8_t> data;
      std::uint16_t nak = 0;
      if (StatusCode st = read_words(a.area, w0, n, data, nak); !is_good(st)) return st;
      if (nak) fail(*items[idx], nak);
      else decode_into(*items[idx], data, std::uint64_t{w0} * 2, now);
    }
    std::map<char, std::vector<std::size_t>> by_size;
    for (std::size_t idx : individual) by_size[addr(*items[idx]).size].push_back(idx);
    for (auto& [size, group] : by_size)
      for (std::size_t start = 0; start < group.size();) {
        std::vector<std::size_t> batch;
        std::size_t vars = 0;
        while (start < group.size()) {
          const std::size_t n = xgt::unit_vars(addr(*items[group[start]])).size();
          if (vars + n > cnet::kMaxBlocks) break;
          vars += n;
          batch.push_back(group[start++]);
        }
        if (StatusCode st = read_individual(items, batch, size, now); !is_good(st)) return st;
      }
    return status::Good;
  }

  StatusCode write(const Address& address, const Value& value) override {
    const auto& a = static_cast<const XgtAddress&>(address);
    if (a.type.scalar == DataType::Boolean && (a.bit_write_direct || !a.block)) return write_bits_direct(a, value);
    if (!a.block) {
      auto data = xgt::encode(a, value);
      if (!data) return data.error().code;
      const auto vars = xgt::unit_vars(a);
      const std::size_t unit = data->size() / vars.size();
      std::vector<std::string> hex;
      for (std::size_t i = 0; i < vars.size(); ++i) {
        auto le = std::span<const std::uint8_t>(data->data() + i * unit, unit);
        std::vector<std::uint8_t> msb(le.rbegin(), le.rend());
        hex.push_back(cnet::to_hex(msb));
      }
      return expect_ack(cnet::write_individual(station_, bcc_, vars, hex));
    }
    // Block areas: read the covering words when the value does not fill them
    // (bits, odd byte offsets), patch, then write the words back.
    const auto [w0, n] = words(a);
    std::vector<std::uint8_t> bytes(n * 2, 0);
    const std::size_t rel = static_cast<std::size_t>(a.byte_offset - std::uint64_t{w0} * 2);
    if (a.type.scalar == DataType::Boolean || rel != 0 || a.byte_len % 2) {
      std::uint16_t nak = 0;
      if (StatusCode st = read_words(a.area, w0, n, bytes, nak); !is_good(st)) return st;
      if (nak) return status::BadOutOfRange;
    }
    if (a.type.scalar == DataType::Boolean) {
      auto b = convert(value, DataType::Boolean);
      if (!b) return b.error().code;
      const std::size_t bit = rel * 8 + static_cast<std::size_t>(a.bit);
      if (*b->get_if<bool>()) bytes[bit / 8] = static_cast<std::uint8_t>(bytes[bit / 8] | (1u << (bit % 8)));
      else bytes[bit / 8] = static_cast<std::uint8_t>(bytes[bit / 8] & ~(1u << (bit % 8)));
    } else {
      auto data = xgt::encode(a, value);
      if (!data) return data.error().code;
      std::copy(data->begin(), data->end(), bytes.begin() + static_cast<std::ptrdiff_t>(rel));
    }
    return expect_ack(cnet::write_continuous(station_, bcc_, std::format("%{}W{}", a.area, w0), n,
                                             cnet::to_hex(unit_to_le(bytes, 2))));  // swap back: LE -> MSB first
  }

 private:
  static const XgtAddress& addr(const ReadItem& item) { return static_cast<const XgtAddress&>(*item.address); }
  static std::pair<std::uint32_t, std::uint32_t> words(const XgtAddress& a) {
    const auto first = static_cast<std::uint32_t>(a.byte_offset / 2);
    const auto last = static_cast<std::uint32_t>((a.byte_offset + std::max<std::uint32_t>(a.byte_len, 1) + 1) / 2);
    return {first, last - first};
  }
  std::uint32_t area_id(const std::string& area) {
    return area_ids_.try_emplace(area, static_cast<std::uint32_t>(area_ids_.size())).first->second;
  }

  void fail(ReadItem& item, std::uint16_t nak) {
    item.quality = status::BadOutOfRange;
    invalid_.insert(&addr(item));
    host_.log.warn("'{}' rejected by PLC (NAK 0x{:04X}); tag deactivated", item.address->text, nak);
  }

  void decode_into(ReadItem& item, const std::vector<std::uint8_t>& data, std::uint64_t base, Timestamp now) {
    const auto& a = addr(item);
    auto v = xgt::decode(a, std::span(data).subspan(static_cast<std::size_t>(a.byte_offset - base)));
    item.quality = v ? status::Good : v.error().code;
    if (v) item.value = std::move(*v);
    item.source_time = now;
  }

  StatusCode read_words(const std::string& area, std::uint32_t first, std::uint32_t count, std::vector<std::uint8_t>& le,
                        std::uint16_t& nak) {
    cnet::Response r;
    StatusCode st = transact(cnet::read_continuous(station_, bcc_, std::format("%{}W{}", area, first), count), true, r);
    if (!is_good(st)) return st;
    if (!r.ack) {
      nak = r.error ? r.error : 0xFFFF;
      return status::Good;
    }
    if (r.blocks.size() != 1 || r.blocks[0].size() != count * 2u) return status::BadCommunicationError;
    le = unit_to_le(r.blocks[0], 2);
    return status::Good;
  }

  StatusCode read_individual(std::span<ReadItem*> items, const std::vector<std::size_t>& batch, char size, Timestamp now) {
    std::vector<std::string> vars;
    for (std::size_t idx : batch)
      for (auto& v : xgt::unit_vars(addr(*items[idx]))) vars.push_back(std::move(v));
    cnet::Response r;
    if (StatusCode st = transact(cnet::read_individual(station_, bcc_, vars), true, r); !is_good(st)) return st;
    if (!r.ack) {
      if (batch.size() == 1) {
        fail(*items[batch[0]], r.error);
        return status::Good;
      }
      for (std::size_t idx : batch)
        if (StatusCode st = read_individual(items, {idx}, size, now); !is_good(st)) return st;
      return status::Good;
    }
    if (r.blocks.size() != vars.size()) return status::BadCommunicationError;
    std::size_t k = 0;
    for (std::size_t idx : batch) {
      const auto& a = addr(*items[idx]);
      std::vector<std::uint8_t> le;
      for (std::size_t j = 0; j < xgt::unit_vars(a).size(); ++j, ++k) {
        auto unit = unit_to_le(r.blocks[k], r.blocks[k].size());
        if (size == 'X') unit = {static_cast<std::uint8_t>(unit.empty() ? 0 : unit[0] & 1)};
        le.insert(le.end(), unit.begin(), unit.end());
      }
      XgtAddress at = a;
      at.byte_offset = 0;
      if (size == 'X') at.bit = 0;
      auto v = xgt::decode(at, le);
      items[idx]->quality = v ? status::Good : v.error().code;
      if (v) items[idx]->value = std::move(*v);
      items[idx]->source_time = now;
    }
    return status::Good;
  }

  StatusCode write_bits_direct(const XgtAddress& a, const Value& value) {
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
    if (bits.size() > cnet::kMaxBlocks) return status::BadOutOfRange;
    std::vector<std::string> vars, data;
    for (std::size_t i = 0; i < bits.size(); ++i) {
      vars.push_back(a.type.is_array() ? std::format("%{}X{}", a.area, a.bit_index + i) : a.var);
      data.push_back(bits[i] ? "01" : "00");
    }
    return expect_ack(cnet::write_individual(station_, bcc_, vars, data));
  }

  StatusCode expect_ack(const std::vector<std::uint8_t>& frame) {
    cnet::Response r;
    if (StatusCode st = transact(frame, false, r); !is_good(st)) return st;
    if (!r.ack) {
      host_.log.warn("write rejected by PLC (NAK 0x{:04X})", r.error);
      return status::BadOutOfRange;
    }
    return status::Good;
  }

  StatusCode transact(const std::vector<std::uint8_t>& frame, bool is_read, cnet::Response& out) {
    auto& port = line_->port;
    if (!port.is_open()) return status::BadNotConnected;
    const auto silence = std::max(port.char_time() * 4, std::chrono::microseconds(2000));
    for (int attempt = 0; attempt < attempts_; ++attempt) {
      port.wait_silence(silence, SteadyClock::now() + request_timeout_);
      port.flush_input();
      const Deadline deadline = SteadyClock::now() + request_timeout_;
      if (StatusCode st = port.write_all(frame, deadline); !is_good(st)) return st;
      std::vector<std::uint8_t> resp;
      StatusCode st = receive(resp, deadline);
      if (is_good(st)) {
        auto r = cnet::parse(resp, station_, bcc_, is_read);
        if (r) {
          out = std::move(*r);
          return status::Good;
        }
        host_.log.debug("station {}: {}", station_, r.error().message);
        st = status::BadCommunicationError;
      }
      if (st != status::BadTimeout && st != status::BadCommunicationError) return st;
      host_.log.debug("station {}: {} ({} of {})", station_, status_name(st), attempt + 1, attempts_);
    }
    return status::BadTimeout;
  }

  // Reads one response: ACK/NAK ... ETX [BCC].
  StatusCode receive(std::vector<std::uint8_t>& out, Deadline deadline) {
    auto& port = line_->port;
    std::uint8_t c = 0;
    do {  // skip noise before the header
      if (StatusCode st = port.read_exact(std::span(&c, 1), deadline); !is_good(st)) return st;
    } while (c != cnet::ACK && c != cnet::NAK);
    out = {c};
    while (out.size() < 300) {
      if (StatusCode st = port.read_exact(std::span(&c, 1), deadline); !is_good(st)) return st;
      out.push_back(c);
      if (c == cnet::ETX) {
        if (bcc_) {
          std::uint8_t b[2];
          if (StatusCode st = port.read_exact(b, deadline); !is_good(st)) return st;
          out.insert(out.end(), b, b + 2);
        }
        return status::Good;
      }
    }
    return status::BadCommunicationError;
  }

  std::shared_ptr<Line> line_;
  std::uint8_t station_;
  HostContext& host_;
  std::string model_;
  xgt::Series series_ = xgt::Series::XGK;
  bool bcc_ = true, online_ = false;
  std::uint32_t max_words_ = 60, gap_ = 8;
  std::chrono::milliseconds request_timeout_{1000};
  int attempts_ = 3;
  std::map<std::string, std::uint32_t> area_ids_;
  std::set<const XgtAddress*> invalid_, solo_;
};

nlohmann::json prop(const char* key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

class CnetDriver final : public Driver {
 public:
  DriverInfo info() const override {
    return {"ls_xgt_cnet", "LS XGT Cnet", "0.1.0", Medium::Serial, {"XGB", "XGK", "XGI", "XGR", "XEC"}};
  }

  nlohmann::json schema() const override {
    using nlohmann::json;
    return {
        {"driver", "ls_xgt_cnet"},
        {"display_name", "LS XGT Cnet"},
        {"channel",
         json::array({
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_PORT", "string", "/dev/ttyUSB0", "Serial device"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_BAUD_RATE", "integer", 9600, "Baud rate"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_DATA_BITS", "integer", 8, "Data bits"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_PARITY", "enum", "None", "None, Even, Odd"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_STOP_BITS", "integer", 1, "Stop bits"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_RS485", "boolean", false, "Kernel RS-485 mode"),
         })},
        {"device",
         json::array({
             prop("servermain.DEVICE_MODEL", "enum", "XGB", "XGB, XGK, XGI, XGR, XEC"),
             prop("servermain.DEVICE_ID_STRING", "string", "1", "Station number 0-31"),
             prop(kBcc, "boolean", true, "Use lowercase commands with BCC check"),
             prop(kMaxWords, "integer", 60, "Words per continuous read (max 60)"),
             prop(kGap, "integer", 8, "Unused words allowed between merged tags"),
         })},
        {"address_help", "Same as LS XGT FEnet: D100, D100.A, M0010A, T5, %MW100, %MX5 ..."},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig& channel, const DeviceConfig& device,
                                                       HostContext& host) override {
    unsigned station = 0;
    auto r = std::from_chars(device.id.data(), device.id.data() + device.id.size(), station);
    if (device.id.empty() || r.ec != std::errc{} || r.ptr != device.id.data() + device.id.size() || station > 31)
      return make_error(status::BadConfigurationError, "device ID must be a station number 0-31");
    auto settings = serial_settings(channel.properties);
    if (!settings) return std::unexpected(settings.error());
    std::lock_guard lock(mu_);
    auto& weak = lines_[channel.name];
    auto line = weak.lock();
    if (!line) {
      line = std::make_shared<Line>();
      line->settings = *settings;
      weak = line;
    }
    return std::make_unique<CnetDevice>(line, static_cast<std::uint8_t>(station), device, host);
  }

 private:
  std::mutex mu_;
  std::map<std::string, std::weak_ptr<Line>> lines_;
};

}  // namespace

LOGICT_DECLARE_DRIVER(CnetDriver)
