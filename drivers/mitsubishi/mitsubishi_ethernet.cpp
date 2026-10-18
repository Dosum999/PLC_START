// Mitsubishi Ethernet driver: MC protocol / SLMP 3E frame, binary or ASCII code, TCP.
// Models: Q, L, iQ-R, iQ-F (FX5) with the 3E frame; FX3U/FX3G with
// FX3U-ENET(-ADP) with the 1E frame (binary). The PLC needs an "SLMP/MC protocol" port
// opened in GX Works (TCP; binary or ASCII code to match DEVICE_COMMUNICATION_CODE), with writes during RUN enabled for writes.
//
// Device ID: IP address or host name.

#include <algorithm>
#include <format>
#include <map>
#include <set>

#include "logict/sdk/block_planner.h"
#include "logict/sdk/driver.h"
#include "logict/sdk/tcp.h"
#include "mc_protocol.h"

namespace {

using namespace logict;
using namespace logict::sdk;
using mc::McAddress;

constexpr const char* kPort = "mitsubishi_ethernet.DEVICE_PORT";
constexpr const char* kNetwork = "mitsubishi_ethernet.DEVICE_NETWORK_NUMBER";
constexpr const char* kPc = "mitsubishi_ethernet.DEVICE_PC_NUMBER";
constexpr const char* kXyOctal = "mitsubishi_ethernet.DEVICE_XY_OCTAL";
constexpr const char* kWordGap = "mitsubishi_ethernet.DEVICE_MAX_WORD_GAP";
constexpr const char* kBitGap = "mitsubishi_ethernet.DEVICE_MAX_BIT_GAP";
constexpr const char* kCode = "mitsubishi_ethernet.DEVICE_COMMUNICATION_CODE";
constexpr const char* kFrame = "mitsubishi_ethernet.DEVICE_FRAME";

// FX3U/FX3G with FX3U-ENET(-ADP): 1E frame, octal X/Y.
bool is_fx3(const std::string& model) { return model.find("FX3") != std::string::npos; }

bool is_fx5(const std::string& model) {
  return model.find("FX5") != std::string::npos || model.find("IQ-F") != std::string::npos ||
         model.find("iQ-F") != std::string::npos;
}

class McDevice final : public DeviceRuntime {
 public:
  McDevice(const DeviceConfig& cfg, HostContext& host) : host_(host) {
    const auto& p = cfg.properties;
    host_name_ = cfg.id;
    model_ = cfg.model.empty() ? "Q Series" : cfg.model;
    port_ = property<std::uint16_t>(p, kPort, 5000);
    route_.network = static_cast<std::uint8_t>(property(p, kNetwork, 0));
    route_.pc = static_cast<std::uint8_t>(property(p, kPc, 0xFF));
    opt_.xy_octal = property(p, kXyOctal, is_fx5(model_) || is_fx3(model_));
    std::string frame = property<std::string>(p, kFrame, is_fx3(model_) ? "1E" : "3E");
    frame_1e_ = frame == "1E" || frame == "1e";
    if (frame_1e_) pc_1e_ = static_cast<std::uint8_t>(property(p, kPc, 0xFF));
    word_gap_ = property<std::uint32_t>(p, kWordGap, 16);
    bit_gap_ = property<std::uint32_t>(p, kBitGap, 64);
    request_timeout_ = cfg.request_timeout;
    timer_ = static_cast<std::uint16_t>(std::max<long long>(1, cfg.request_timeout.count() / 250));
    attempts_ = std::max(1, cfg.attempts);
    std::string code = property<std::string>(p, kCode, "Binary");
    for (auto& c : code) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    ascii_ = code == "ascii";
    tcp_.set_observer(host.frames);
  }

  Result<std::shared_ptr<const Address>> parse_address(std::string_view text, TagType requested) override {
    auto a = mc::parse_address(text, requested, opt_);
    if (!a) return std::unexpected(a.error());
    if (frame_1e_) {
      if (mc::device_code_1e(*(*a)->device) == 0)
        return make_error(status::BadInvalidArgument, std::format("device {} is not available with the 1E frame", (*a)->device->name));
      if ((*a)->bit_units ? (*a)->points > mc::kMaxBits1E : (*a)->points > mc::kMaxWords1E)
        return make_error(status::BadInvalidArgument, "tag larger than one 1E request (64 words / 256 bits)");
    }
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
    std::vector<Job> jobs;
    for (std::size_t i = 0; i < items.size(); ++i) {
      const auto& a = addr(*items[i]);
      if (invalid_.contains(&a)) {
        items[i]->quality = status::BadOutOfRange;
        continue;
      }
      const auto [area, start] = plan_key(a);
      if (solo_.contains(&a)) jobs.push_back(Job{area, start, a.points, {i}});
      else plan.push_back({area, start, a.points, i});
    }
    for (auto& b : plan_blocks(plan, [this](std::uint32_t area) {
           if (frame_1e_) return (area & 1) ? PlanLimits{mc::kMaxBits1E, bit_gap_} : PlanLimits{mc::kMaxWords1E, word_gap_};
           return (area & 1) ? PlanLimits{mc::kMaxBits, bit_gap_} : PlanLimits{mc::kMaxWords, word_gap_};
         }))
      jobs.push_back(Job{b.area, static_cast<std::uint32_t>(b.start), b.length, std::move(b.items)});
    return run_jobs(items, std::move(jobs), now);
  }

  StatusCode write(const Address& address, const Value& value) override {
    const auto& a = static_cast<const McAddress&>(address);
    if (a.bit >= 0) {
      // Bit of a word device: read-modify-write the word.
      auto b = convert(value, DataType::Boolean);
      if (!b) return b.error().code;
      std::vector<std::uint8_t> word;
      if (StatusCode st = read_raw(*a.device, a.number, 1, false, word); !is_good(st)) return st;
      if (word.size() != 2) return status::BadCommunicationError;
      const std::uint16_t mask = static_cast<std::uint16_t>(1u << a.bit);
      std::uint16_t w = static_cast<std::uint16_t>(word[0] | (word[1] << 8));
      w = *b->get_if<bool>() ? static_cast<std::uint16_t>(w | mask) : static_cast<std::uint16_t>(w & ~mask);
      const std::uint8_t out[2] = {static_cast<std::uint8_t>(w), static_cast<std::uint8_t>(w >> 8)};
      return write_raw(*a.device, a.number, 1, false, out);
    }
    auto data = mc::encode(a, value);
    if (!data) return data.error().code;
    const std::uint32_t max_bits = frame_1e_ ? mc::kMaxBits1E : mc::kMaxBits;
    const std::uint32_t max_words = frame_1e_ ? mc::kMaxWords1E : mc::kMaxWords;
    if (a.bit_units) {
      for (std::size_t off = 0; off < data->size(); off += max_bits) {
        const std::size_t n = std::min<std::size_t>(max_bits, data->size() - off);
        auto packed = mc::pack_bits(std::span<const std::uint8_t>(data->data() + off, n));
        StatusCode st = write_raw(*a.device, a.number + static_cast<std::uint32_t>(off), static_cast<std::uint16_t>(n), true, packed);
        if (!is_good(st)) return st;
      }
      return status::Good;
    }
    // Word units (also bit devices accessed as words: 16 points per word).
    const std::uint32_t words = a.points;
    for (std::uint32_t off = 0; off < words; off += max_words) {
      const std::uint32_t n = std::min(max_words, words - off);
      const std::uint32_t number = a.number + (a.device->bit ? off * 16 : off);
      StatusCode st = write_raw(*a.device, number, static_cast<std::uint16_t>(n), false,
                                std::span<const std::uint8_t>(data->data() + off * 2, n * 2));
      if (!is_good(st)) return st;
    }
    return status::Good;
  }

 private:
  struct Job {
    std::uint32_t area;  // (device code << 1) | bit_units
    std::uint32_t start, length;
    std::vector<std::size_t> items;
  };

  static const McAddress& addr(const ReadItem& item) { return static_cast<const McAddress&>(*item.address); }

  // Planning space: bit units are device points; word units are words, where a
  // bit device read as words is indexed by number / 16.
  static std::pair<std::uint32_t, std::uint32_t> plan_key(const McAddress& a) {
    const std::uint32_t area = (static_cast<std::uint32_t>(a.device->code) << 1) | (a.bit_units ? 1u : 0u);
    const std::uint32_t start = !a.bit_units && a.device->bit ? a.number / 16 : a.number;
    return {area, start};
  }

  static const mc::Device& device_of(std::uint32_t area) {
    static std::map<std::uint8_t, const mc::Device*> by_code;
    const auto code = static_cast<std::uint8_t>(area >> 1);
    if (by_code.empty())
      for (const char* n : {"SM", "SD", "SB", "SW", "DX", "DY", "TS", "TC", "TN", "SS", "SC", "SN", "CS", "CC", "CN", "ZR",
                            "X", "Y", "M", "L", "F", "V", "B", "D", "W", "R", "Z"})
        by_code[mc::find_device(n)->code] = mc::find_device(n);
    return *by_code.at(code);
  }

  StatusCode run_jobs(std::span<ReadItem*> items, std::vector<Job> jobs, Timestamp now) {
    std::vector<Job> retry;
    for (auto& job : jobs) {
      const bool bit_units = job.area & 1;
      const auto& dev = device_of(job.area);
      const std::uint32_t number = !bit_units && dev.bit ? job.start * 16 : job.start;
      std::vector<std::uint8_t> raw;
      std::uint16_t end_code = 0;
      StatusCode st = read_raw(dev, number, static_cast<std::uint16_t>(job.length), bit_units, raw, &end_code);
      if (!is_good(st)) return st;
      if (end_code == 0) {
        std::vector<std::uint8_t> data;
        if (bit_units) {
          data.resize(job.length);
          for (std::uint32_t i = 0; i < job.length; ++i) data[i] = mc::unpack_bit(raw, i) ? 1 : 0;
        } else {
          data = std::move(raw);
        }
        for (std::size_t idx : job.items) {
          const auto& a = addr(*items[idx]);
          const std::uint32_t rel = plan_key(a).second - job.start;
          auto v = mc::decode(a, std::span(data).subspan(bit_units ? rel : rel * 2));
          items[idx]->quality = v ? status::Good : v.error().code;
          if (v) items[idx]->value = std::move(*v);
          items[idx]->source_time = now;
        }
      } else if (job.items.size() == 1) {
        auto& item = *items[job.items[0]];
        item.quality = frame_1e_ ? status_1e(end_code) : mc::end_code_status(end_code);
        if (item.quality == status::BadOutOfRange) {
          invalid_.insert(&addr(item));
          host_.log.warn("'{}' rejected by PLC (end code 0x{:04X}); tag deactivated", item.address->text, end_code);
        } else {
          host_.log.warn("'{}': PLC end code 0x{:04X}", item.address->text, end_code);
        }
      } else {
        for (std::size_t idx : job.items) {
          const auto& a = addr(*items[idx]);
          solo_.insert(&a);
          const auto [area, start] = plan_key(a);
          retry.push_back(Job{area, start, a.points, {idx}});
        }
      }
    }
    return retry.empty() ? status::Good : run_jobs(items, std::move(retry), now);
  }

  StatusCode read_raw(const mc::Device& d, std::uint32_t number, std::uint16_t points, bool bit_units,
                      std::vector<std::uint8_t>& out, std::uint16_t* end_code = nullptr) {
    mc::Response r;
    if (frame_1e_) {
      StatusCode st = transact_1e(mc::batch_read_1e(pc_1e_, timer_, mc::device_code_1e(d), number, points, bit_units),
                                  bit_units ? (points + 1u) / 2 : points * 2u, r);
      if (!is_good(st)) return st;
      if (end_code) *end_code = r.end_code;
      if (r.end_code != 0) return end_code ? status::Good : status_1e(r.end_code);
      out = std::move(r.data);
      return status::Good;
    }
    StatusCode st = transact(ascii_ ? mc::batch_read_ascii(route_, timer_, d, base_of(d), number, points, bit_units)
                                    : mc::batch_read(route_, timer_, d, number, points, bit_units),
                             r, bit_units, points);
    if (!is_good(st)) return st;
    if (end_code) *end_code = r.end_code;
    if (r.end_code != 0) return end_code ? status::Good : mc::end_code_status(r.end_code);
    const std::size_t expect = bit_units ? (points + 1u) / 2 : points * 2u;
    if (r.data.size() != expect) return status::BadCommunicationError;
    out = std::move(r.data);
    return status::Good;
  }

  StatusCode write_raw(const mc::Device& d, std::uint32_t number, std::uint16_t points, bool bit_units,
                       std::span<const std::uint8_t> payload) {
    mc::Response r;
    if (frame_1e_) {
      StatusCode st = transact_1e(mc::batch_write_1e(pc_1e_, timer_, mc::device_code_1e(d), number, points, bit_units, payload), 0, r);
      if (!is_good(st)) return st;
      if (r.end_code != 0) {
        host_.log.warn("write to {}{} rejected (1E end code 0x{:04X})", d.name, number, r.end_code);
        return status_1e(r.end_code);
      }
      return status::Good;
    }
    StatusCode st = transact(ascii_ ? mc::batch_write_ascii(route_, timer_, d, base_of(d), number, points, bit_units, payload)
                                    : mc::batch_write(route_, timer_, d, number, points, bit_units, payload),
                             r, bit_units, 0);
    if (!is_good(st)) return st;
    if (r.end_code != 0) {
      host_.log.warn("write to {}{} rejected (end code 0x{:04X}; is 'write during RUN' enabled?)", d.name, number, r.end_code);
      return mc::end_code_status(r.end_code);
    }
    return status::Good;
  }

  static StatusCode status_1e(std::uint16_t code) {
    return mc::end_code_status_1e(static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(code));
  }

  // 1E: the answer length follows from the request (`data_size` on success).
  // end_code = end << 8 | abnormal code.
  StatusCode transact_1e(const std::vector<std::uint8_t>& req, std::size_t data_size, mc::Response& out) {
    if (!tcp_.is_open()) return status::BadNotConnected;
    const Deadline deadline = SteadyClock::now() + request_timeout_;
    StatusCode st = tcp_.send_all(req, deadline);
    std::uint8_t hdr[2];
    if (is_good(st)) st = tcp_.recv_exact(hdr, deadline);
    if (is_good(st)) {
      if (hdr[0] != (req[0] | 0x80)) {
        tcp_.close();
        return status::BadCommunicationError;
      }
      out = {};
      if (hdr[1] == 0x5B) {
        std::uint8_t abnormal = 0;
        st = tcp_.recv_exact(std::span(&abnormal, 1), deadline);
        out.end_code = static_cast<std::uint16_t>(0x5B00 | abnormal);
      } else if (hdr[1] != 0) {
        out.end_code = static_cast<std::uint16_t>(hdr[1] << 8);
      } else {
        out.data.resize(data_size);
        st = tcp_.recv_exact(out.data, deadline);
      }
      if (is_good(st)) return status::Good;
    }
    if (st == status::BadTimeout) tcp_.close();
    return st;
  }

  // Numbering base of a device in ASCII frames (iQ-F X/Y are octal).
  int base_of(const mc::Device& d) const {
    if (opt_.xy_octal && (d.code == 0x9C || d.code == 0x9D)) return 8;
    return d.base;
  }

  // 3E frames carry no sequence number: after a timeout the connection is
  // reset so a late answer cannot be taken for the next request's response.
  // `bit_units`/`points` describe the expected data (ASCII responses).
  StatusCode transact(const std::vector<std::uint8_t>& req, mc::Response& out, bool bit_units, std::uint16_t points) {
    if (!tcp_.is_open()) return status::BadNotConnected;
    const Deadline deadline = SteadyClock::now() + request_timeout_;
    StatusCode st = tcp_.send_all(req, deadline);
    std::uint8_t hdr[mc::kAsciiResponseHeader];
    const std::size_t hdr_size = ascii_ ? mc::kAsciiResponseHeader : mc::kResponseHeader;
    if (is_good(st)) st = tcp_.recv_exact(std::span(hdr, hdr_size), deadline);
    if (is_good(st)) {
      auto len = ascii_ ? mc::response_length_ascii(std::span(hdr, hdr_size)) : mc::response_length(std::span(hdr, hdr_size));
      if (!len) {
        tcp_.close();
        return status::BadCommunicationError;
      }
      std::vector<std::uint8_t> rest(*len);
      st = tcp_.recv_exact(rest, deadline);
      if (is_good(st)) {
        auto r = ascii_ ? mc::parse_response_ascii(rest, bit_units, points) : mc::parse_response(rest);
        if (!r) return r.error().code;
        out = std::move(*r);
        return status::Good;
      }
    }
    if (st == status::BadTimeout) tcp_.close();
    return st;
  }

  HostContext& host_;
  std::string host_name_, model_;
  std::uint16_t port_ = 5000, timer_ = 4;
  mc::Route route_;
  mc::AddressOptions opt_;
  std::uint32_t word_gap_ = 16, bit_gap_ = 64;
  std::chrono::milliseconds request_timeout_{1000};
  int attempts_ = 3;
  bool ascii_ = false;
  bool frame_1e_ = false;
  std::uint8_t pc_1e_ = 0xFF;
  TcpClient tcp_;
  std::set<const McAddress*> invalid_, solo_;
};

nlohmann::json prop(const char* key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

class MitsubishiDriver final : public Driver {
 public:
  DriverInfo info() const override {
    return {"mitsubishi_ethernet", "Mitsubishi Ethernet", "0.1.0", Medium::Ethernet, {"Q Series", "L Series", "iQ-R", "iQ-F (FX5)", "FX3U (1E)"}};
  }

  nlohmann::json schema() const override {
    using nlohmann::json;
    return {
        {"driver", "mitsubishi_ethernet"},
        {"display_name", "Mitsubishi Ethernet"},
        {"channel", json::array()},
        {"device",
         json::array({
             prop("servermain.DEVICE_MODEL", "enum", "Q Series", "Q Series, L Series, iQ-R, iQ-F (FX5), FX3U (1E)"),
             prop("servermain.DEVICE_ID_STRING", "string", "192.168.3.39", "PLC IP address or host name"),
             prop(kPort, "integer", 5000, "SLMP/MC protocol TCP port opened in GX Works"),
             prop(kNetwork, "integer", 0, "Network number (0 = own network)"),
             prop(kPc, "integer", 255, "PC number (255 = connected station)"),
             prop(kXyOctal, "boolean", nullptr, "X/Y numbered in octal (default: true for iQ-F)"),
             prop(kWordGap, "integer", 16, "Unused words allowed between merged tags"),
             prop(kBitGap, "integer", 64, "Unused bit points allowed between merged tags"),
             prop(kCode, "enum", "Binary", "Communication data code set in GX Works: Binary or ASCII (3E frame)"),
             prop(kFrame, "enum", nullptr, "3E or 1E (default: 1E for FX3 models with FX3U-ENET, else 3E)"),
         })},
        {"address_help",
         "Words: D100, W1F (hex), R10, ZR100, SD10, TN5 (timer value), CN5; bit of word: D100.A; bits: M100, X1F, Y20, "
         "B10, L5, SM400, TS5 (timer contact), CS5; strings D100.20; arrays D100 [10], M0 [16]. "
         "Bit devices with a word type (M0 as Word) read 16 points per word."},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig&, const DeviceConfig& device,
                                                       HostContext& host) override {
    if (device.id.empty()) return make_error(status::BadConfigurationError, "device ID (IP address) is empty");
    return std::make_unique<McDevice>(device, host);
  }
};

}  // namespace

LOGICT_DECLARE_DRIVER(MitsubishiDriver)
