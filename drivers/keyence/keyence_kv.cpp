// Keyence KV Ethernet driver: host link (上位リンク) commands over TCP.
// Models: KV-8000, KV-7000, KV-5000/5500, KV NANO (KV-NC1EP / built-in port).
// The PLC needs host link enabled on its Ethernet port (KV STUDIO unit
// settings, default port 8501).
//
// Device ID: IP address or host name.

#include <algorithm>
#include <format>
#include <set>

#include "kv_protocol.h"
#include "logict/sdk/block_planner.h"
#include "logict/sdk/driver.h"
#include "logict/sdk/tcp.h"

namespace {

using namespace logict;
using namespace logict::sdk;
using kv::KvAddress;
using kv::Mode;

constexpr const char* kPort = "keyence_kv.DEVICE_PORT";
constexpr const char* kWordGap = "keyence_kv.DEVICE_MAX_WORD_GAP";
constexpr const char* kBitGap = "keyence_kv.DEVICE_MAX_BIT_GAP";

class KvDevice final : public DeviceRuntime {
 public:
  KvDevice(const DeviceConfig& cfg, HostContext& host) : host_(host) {
    const auto& p = cfg.properties;
    host_name_ = cfg.id;
    port_ = property<std::uint16_t>(p, kPort, 8501);
    word_gap_ = property<std::uint32_t>(p, kWordGap, 16);
    bit_gap_ = property<std::uint32_t>(p, kBitGap, 64);
    request_timeout_ = cfg.request_timeout;
    tcp_.set_observer(host.frames);
  }

  Result<std::shared_ptr<const Address>> parse_address(std::string_view text, TagType requested) override {
    auto a = kv::parse_address(text, requested);
    if (!a) return std::unexpected(a.error());
    return std::shared_ptr<const Address>(std::move(*a));
  }

  StatusCode connect(Deadline deadline) override {
    rx_.clear();
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
      const std::uint32_t area = area_of(a);
      if (a.mode == Mode::Contact || solo_.contains(&a)) jobs.push_back(Job{area, a.start, a.points, {i}});
      else plan.push_back({area, a.start, a.points, i});
    }
    for (auto& b : plan_blocks(plan, [this](std::uint32_t area) {
           const Mode mode = static_cast<Mode>(area & 3);
           return PlanLimits{kv::max_points(mode), mode == Mode::Bits ? bit_gap_ : mode == Mode::Words ? word_gap_ : 0};
         }))
      jobs.push_back(Job{b.area, static_cast<std::uint32_t>(b.start), b.length, std::move(b.items)});
    return run_jobs(items, std::move(jobs), now);
  }

  StatusCode write(const Address& address, const Value& value) override {
    const auto& a = static_cast<const KvAddress&>(address);
    if (a.bit >= 0) {
      // Bit of a word: read-modify-write the word.
      auto b = convert(value, DataType::Boolean);
      if (!b) return b.error().code;
      kv::Answer ans;
      if (StatusCode st = read_raw(*a.device, Mode::Words, a.start, 1, ans); !is_good(st)) return st;
      if (!ans.error.empty()) return kv::error_status(ans.error);
      std::uint32_t w = ans.values[0];
      w = *b->get_if<bool>() ? (w | (1u << a.bit)) : (w & ~(1u << a.bit));
      const std::uint32_t raw[1] = {w};
      return write_raw(a, a.start, raw);
    }
    auto raw = kv::encode(a, value);
    if (!raw) return raw.error().code;
    const std::uint32_t limit = kv::max_points(a.mode);
    for (std::uint32_t off = 0; off < raw->size(); off += limit) {
      const std::uint32_t n = std::min<std::uint32_t>(limit, static_cast<std::uint32_t>(raw->size()) - off);
      if (StatusCode st = write_raw(a, a.start + off, std::span(raw->data() + off, n)); !is_good(st)) return st;
    }
    return status::Good;
  }

 private:
  struct Job {
    std::uint32_t area;  // device index << 2 | mode
    std::uint32_t start, length;
    std::vector<std::size_t> items;
  };

  static const KvAddress& addr(const ReadItem& item) { return static_cast<const KvAddress&>(*item.address); }
  static std::uint32_t area_of(const KvAddress& a) {
    return kv::device_index(*a.device) << 2 | static_cast<std::uint32_t>(a.mode);
  }

  StatusCode run_jobs(std::span<ReadItem*> items, std::vector<Job> jobs, Timestamp now) {
    std::vector<Job> retry;
    for (auto& job : jobs) {
      const auto& dev = kv::device_at(job.area >> 2);
      const Mode mode = static_cast<Mode>(job.area & 3);
      kv::Answer ans;
      if (StatusCode st = read_raw(dev, mode, job.start, job.length, ans); !is_good(st)) return st;
      if (ans.error.empty()) {
        for (std::size_t idx : job.items) {
          const auto& a = addr(*items[idx]);
          auto v = kv::decode(a, std::span(ans.values).subspan(a.start - job.start));
          items[idx]->quality = v ? status::Good : v.error().code;
          if (v) items[idx]->value = std::move(*v);
          items[idx]->source_time = now;
        }
      } else if (job.items.size() == 1) {
        auto& item = *items[job.items[0]];
        item.quality = kv::error_status(ans.error);
        if (item.quality == status::BadOutOfRange || item.quality == status::BadNotSupported) {
          invalid_.insert(&addr(item));
          host_.log.warn("'{}' rejected by PLC ({}); tag deactivated", item.address->text, ans.error);
        } else {
          host_.log.warn("'{}': PLC error {}", item.address->text, ans.error);
        }
      } else {
        for (std::size_t idx : job.items) {
          const auto& a = addr(*items[idx]);
          solo_.insert(&a);
          retry.push_back(Job{area_of(a), a.start, a.points, {idx}});
        }
      }
    }
    return retry.empty() ? status::Good : run_jobs(items, std::move(retry), now);
  }

  StatusCode read_raw(const kv::Device& d, Mode mode, std::uint32_t start, std::uint32_t count, kv::Answer& out) {
    std::string line;
    if (StatusCode st = transact(kv::read_command(d, mode, start, count), line); !is_good(st)) return st;
    auto ans = kv::parse_read_answer(line, mode, count);
    if (!ans) {
      host_.log.warn("{}", ans.error().message);
      return ans.error().code;
    }
    out = std::move(*ans);
    return status::Good;
  }

  StatusCode write_raw(const KvAddress& a, std::uint32_t start, std::span<const std::uint32_t> raw) {
    std::vector<std::string> tokens;
    for (auto r : raw) tokens.push_back(kv::format_token(a.mode, r));
    std::string line;
    if (StatusCode st = transact(kv::write_command(*a.device, a.mode, start, tokens), line); !is_good(st)) return st;
    if (line == "OK") return status::Good;
    if (line.size() == 2 && line[0] == 'E') {
      host_.log.warn("write to '{}' rejected ({})", a.text, line);
      return kv::error_status(line);
    }
    host_.log.warn("unexpected answer to a write: '{}'", line);
    return status::BadCommunicationError;
  }

  // Host link has no sequence numbers: after a timeout the connection is reset
  // so that a late answer cannot be taken for the next command's.
  StatusCode transact(const std::string& cmd, std::string& line) {
    if (!tcp_.is_open()) return status::BadNotConnected;
    const Deadline deadline = SteadyClock::now() + request_timeout_;
    rx_.clear();
    StatusCode st = tcp_.send_all(std::span(reinterpret_cast<const std::uint8_t*>(cmd.data()), cmd.size()), deadline);
    while (is_good(st)) {
      if (auto end = rx_.find_first_of("\r\n"); end != std::string::npos) {
        line = rx_.substr(0, end);
        rx_.clear();
        return status::Good;
      }
      if (rx_.size() > 64 * 1024) {
        tcp_.close();
        return status::BadCommunicationError;
      }
      std::uint8_t buf[4096];
      std::size_t n = 0;
      st = tcp_.recv_some(buf, n, deadline);
      if (is_good(st)) rx_.append(reinterpret_cast<const char*>(buf), n);
    }
    if (st == status::BadTimeout) tcp_.close();
    return st;
  }

  HostContext& host_;
  std::string host_name_;
  std::uint16_t port_ = 8501;
  std::uint32_t word_gap_ = 16, bit_gap_ = 64;
  std::chrono::milliseconds request_timeout_{1000};
  TcpClient tcp_;
  std::string rx_;
  std::set<const KvAddress*> invalid_, solo_;
};

nlohmann::json prop(const char* key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

class KeyenceDriver final : public Driver {
 public:
  DriverInfo info() const override {
    return {"keyence_kv", "Keyence KV Ethernet", "0.1.0", Medium::Ethernet, {"KV-8000", "KV-7000", "KV-5000", "KV NANO"}};
  }

  nlohmann::json schema() const override {
    using nlohmann::json;
    return {
        {"driver", "keyence_kv"},
        {"display_name", "Keyence KV Ethernet"},
        {"channel", json::array()},
        {"device",
         json::array({
             prop("servermain.DEVICE_MODEL", "enum", "KV-8000", "KV-8000, KV-7000, KV-5000, KV NANO"),
             prop("servermain.DEVICE_ID_STRING", "string", "192.168.0.10", "PLC IP address or host name"),
             prop(kPort, "integer", 8501, "Host link TCP port"),
             prop(kWordGap, "integer", 16, "Unused words allowed between merged tags"),
             prop(kBitGap, "integer", 64, "Unused bits allowed between merged tags"),
         })},
        {"address_help",
         "Words: DM100, EM0, FM0, ZF0, W1F (hex), TM5, CM0, VM0, D100/E0/F0 (XYM); bit of word: DM100.5; "
         "bits: R1015 (channel 10 bit 15), MR0, LR0, CR0, B1F, VB0, X1F, Y20, M100, L5; "
         "timers/counters: TC5 / TS5 / CC5 / CS5 (current / preset), T5 / C5 (contact); strings DM100.20; "
         "arrays DM100 [10], MR0 [16]. Bit devices with a word type (R000 as Word) read 16 bits per word."},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig&, const DeviceConfig& device,
                                                       HostContext& host) override {
    if (device.id.empty()) return make_error(status::BadConfigurationError, "device ID (IP address) is empty");
    return std::make_unique<KvDevice>(device, host);
  }
};

}  // namespace

LOGICT_DECLARE_DRIVER(KeyenceDriver)
