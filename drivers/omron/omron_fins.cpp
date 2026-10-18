// Omron FINS Ethernet driver: FINS/UDP (default) or FINS/TCP.
// Models: CS1, CJ1, CJ2, CP1 (with Ethernet), NJ/NX in FINS mode.
//
// Device ID: IP address or host name. FINS node numbers default to the last
// octet of the IP addresses (UDP) or to the nodes negotiated by FINS/TCP.

#include <algorithm>
#include <format>
#include <set>

#include "fins_protocol.h"
#include "logict/sdk/block_planner.h"
#include "logict/sdk/driver.h"
#include "logict/sdk/tcp.h"
#include "logict/sdk/udp.h"

namespace {

using namespace logict;
using namespace logict::sdk;
using fins::FinsAddress;

constexpr const char* kProtocol = "omron_fins.DEVICE_PROTOCOL";
constexpr const char* kPort = "omron_fins.DEVICE_PORT";
constexpr const char* kDestNetwork = "omron_fins.DEVICE_DESTINATION_NETWORK";
constexpr const char* kDestNode = "omron_fins.DEVICE_DESTINATION_NODE";
constexpr const char* kDestUnit = "omron_fins.DEVICE_DESTINATION_UNIT";
constexpr const char* kSrcNetwork = "omron_fins.DEVICE_SOURCE_NETWORK";
constexpr const char* kSrcNode = "omron_fins.DEVICE_SOURCE_NODE";
constexpr const char* kLocalPort = "omron_fins.DEVICE_LOCAL_UDP_PORT";
constexpr const char* kFirstWordLow = "omron_fins.DEVICE_FIRST_WORD_LOW";
constexpr const char* kWordGap = "omron_fins.DEVICE_MAX_WORD_GAP";
constexpr const char* kBitGap = "omron_fins.DEVICE_MAX_BIT_GAP";

std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

const char* tcp_error_text(std::uint32_t e) {
  switch (e) {
    case 0x01: return "header is not FINS";
    case 0x02: return "data length too long";
    case 0x03: return "command not supported";
    case 0x20: return "all connections are in use";
    case 0x21: return "the node is already connected";
    case 0x22: return "attempt to access a protected node";
    case 0x23: return "client FINS node address out of range";
    case 0x24: return "same FINS node address as the server";
    case 0x25: return "all node addresses are in use";
    default: return "unknown error";
  }
}

class FinsDevice final : public DeviceRuntime {
 public:
  FinsDevice(const DeviceConfig& cfg, HostContext& host) : host_(host) {
    const auto& p = cfg.properties;
    host_name_ = cfg.id;
    tcp_mode_ = upper(property<std::string>(p, kProtocol, "UDP")) == "TCP";
    port_ = property<std::uint16_t>(p, kPort, 9600);
    local_port_ = property<std::uint16_t>(p, kLocalPort, 0);
    header_.dna = static_cast<std::uint8_t>(property(p, kDestNetwork, 0));
    header_.da2 = static_cast<std::uint8_t>(property(p, kDestUnit, 0));
    header_.sna = static_cast<std::uint8_t>(property(p, kSrcNetwork, 0));
    dest_node_ = property(p, kDestNode, -1);  // -1: automatic
    src_node_ = property(p, kSrcNode, -1);
    order_ = property(p, kFirstWordLow, true) ? codec::ByteOrder::CDAB : codec::ByteOrder::ABCD;
    word_gap_ = property<std::uint32_t>(p, kWordGap, 16);
    bit_gap_ = property<std::uint32_t>(p, kBitGap, 64);
    request_timeout_ = cfg.request_timeout;
    tcp_.set_observer(host.frames);
    udp_.set_observer(host.frames);
  }

  Result<std::shared_ptr<const Address>> parse_address(std::string_view text, TagType requested) override {
    auto a = fins::parse_address(text, requested);
    if (!a) return std::unexpected(a.error());
    return std::shared_ptr<const Address>(std::move(*a));
  }

  StatusCode connect(Deadline deadline) override {
    return tcp_mode_ ? connect_tcp(deadline) : connect_udp();
  }
  void disconnect() override {
    tcp_.close();
    udp_.close();
  }
  bool connected() const override { return tcp_mode_ ? tcp_.is_open() : udp_.is_open(); }

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
      if (solo_.contains(&a)) jobs.push_back(Job{a.code, a.index, a.points, {i}});
      else plan.push_back({a.code, a.index, a.points, i});
    }
    for (auto& b : plan_blocks(plan, [this](std::uint32_t code) {
           return is_bit_code(static_cast<std::uint8_t>(code)) ? PlanLimits{fins::kMaxBits, bit_gap_}
                                                               : PlanLimits{fins::kMaxWords, word_gap_};
         }))
      jobs.push_back(Job{static_cast<std::uint8_t>(b.area), static_cast<std::uint32_t>(b.start), b.length, std::move(b.items)});
    return run_jobs(items, std::move(jobs), now);
  }

  StatusCode write(const Address& address, const Value& value) override {
    const auto& a = static_cast<const FinsAddress&>(address);
    auto data = fins::encode(a, value, order_);
    if (!data) return data.error().code;
    const std::uint32_t limit = a.bit_units ? fins::kMaxBits : fins::kMaxWords;
    const std::size_t unit = a.bit_units ? 1 : 2;
    for (std::uint32_t off = 0; off < a.points; off += limit) {
      const std::uint32_t n = std::min(limit, a.points - off);
      const std::uint32_t index = a.index + off;
      fins::Response r;
      StatusCode st = transact(fins::memory_write(header_, next_sid(), a.code, fins::word_of(a.code, index, a.bit_units),
                                                  fins::bit_of(a.code, index, a.bit_units), static_cast<std::uint16_t>(n),
                                                  std::span<const std::uint8_t>(data->data() + off * unit, n * unit)),
                               fins::kSrcWrite, r);
      if (!is_good(st)) return st;
      if (r.end_code != 0) {
        host_.log.warn("write to '{}' rejected (end code 0x{:04X})", a.text, r.end_code);
        return fins::end_code_status(r.end_code);
      }
    }
    return status::Good;
  }

 private:
  struct Job {
    std::uint8_t code;
    std::uint32_t start, length;
    std::vector<std::size_t> items;
  };

  static bool is_bit_code(std::uint8_t code) { return code < 0x80 || (code >= 0xE0 && code <= 0xE8); }
  static const FinsAddress& addr(const ReadItem& item) { return static_cast<const FinsAddress&>(*item.address); }

  std::uint8_t next_sid() { return ++sid_; }

  StatusCode run_jobs(std::span<ReadItem*> items, std::vector<Job> jobs, Timestamp now) {
    std::vector<Job> retry;
    for (auto& job : jobs) {
      const bool bits = is_bit_code(job.code);
      fins::Response r;
      StatusCode st = transact(fins::memory_read(header_, next_sid(), job.code, fins::word_of(job.code, job.start, bits),
                                                 fins::bit_of(job.code, job.start, bits), static_cast<std::uint16_t>(job.length)),
                               fins::kSrcRead, r);
      if (!is_good(st)) return st;
      if (r.end_code == 0) {
        const std::size_t unit = bits ? 1 : 2;
        if (r.data.size() != job.length * unit) return status::BadCommunicationError;
        for (std::size_t idx : job.items) {
          const auto& a = addr(*items[idx]);
          auto v = fins::decode(a, std::span(r.data).subspan((a.index - job.start) * unit), order_);
          items[idx]->quality = v ? status::Good : v.error().code;
          if (v) items[idx]->value = std::move(*v);
          items[idx]->source_time = now;
        }
      } else if (job.items.size() == 1) {
        auto& item = *items[job.items[0]];
        item.quality = fins::end_code_status(r.end_code);
        if (item.quality == status::BadOutOfRange || item.quality == status::BadNotSupported) {
          invalid_.insert(&addr(item));
          host_.log.warn("'{}' rejected by PLC (end code 0x{:04X}); tag deactivated", item.address->text, r.end_code);
        } else {
          host_.log.warn("'{}': PLC end code 0x{:04X}", item.address->text, r.end_code);
        }
      } else {
        for (std::size_t idx : job.items) {
          const auto& a = addr(*items[idx]);
          solo_.insert(&a);
          retry.push_back(Job{a.code, a.index, a.points, {idx}});
        }
      }
    }
    return retry.empty() ? status::Good : run_jobs(items, std::move(retry), now);
  }

  StatusCode connect_udp() {
    StatusCode st = udp_.open(host_name_, port_, local_port_);
    if (st != status::Good) {
      host_.log.debug("UDP open {}:{} failed: {}", host_name_, port_, udp_.last_error());
      return st;
    }
    header_.da1 = static_cast<std::uint8_t>(dest_node_ >= 0 ? dest_node_ : udp_.peer_ipv4_last_octet());
    header_.sa1 = static_cast<std::uint8_t>(src_node_ >= 0 ? src_node_ : udp_.local_ipv4_last_octet());
    return status::Good;
  }

  StatusCode connect_tcp(Deadline deadline) {
    StatusCode st = tcp_.connect(host_name_, port_, deadline);
    if (st != status::Good) {
      host_.log.debug("connect to {}:{} failed: {}", host_name_, port_, tcp_.last_error());
      return st;
    }
    // Node address exchange: the server assigns our node when we send 0.
    st = tcp_.send_all(fins::tcp_node_request(static_cast<std::uint8_t>(std::max(src_node_, 0))), deadline);
    std::uint8_t resp[fins::kTcpHeader + 8];
    if (is_good(st)) st = tcp_.recv_exact(std::span(resp, fins::kTcpHeader), deadline);
    if (!is_good(st)) {
      tcp_.close();
      return st;
    }
    auto h = fins::parse_tcp_header(std::span(resp, fins::kTcpHeader));
    if (!h || h->error != 0 || h->command != fins::kTcpNodeResponse || h->length != 16) {
      if (h && h->error != 0)
        host_.log.warn("FINS/TCP node address exchange refused: {} (0x{:X})", tcp_error_text(h->error), h->error);
      tcp_.close();
      return status::BadCommunicationError;
    }
    if (st = tcp_.recv_exact(std::span(resp + fins::kTcpHeader, 8), deadline); !is_good(st)) {
      tcp_.close();
      return st;
    }
    header_.sa1 = resp[fins::kTcpHeader + 3];
    header_.da1 = static_cast<std::uint8_t>(dest_node_ >= 0 ? dest_node_ : resp[fins::kTcpHeader + 7]);
    return status::Good;
  }

  StatusCode transact(const std::vector<std::uint8_t>& req, std::uint8_t src, fins::Response& out) {
    const std::uint8_t sid = req[9];
    const Deadline deadline = SteadyClock::now() + request_timeout_;
    if (!tcp_mode_) {
      if (!udp_.is_open()) return status::BadNotConnected;
      udp_.drain();
      if (StatusCode st = udp_.send(req); !is_good(st)) return st;
      std::vector<std::uint8_t> buf(4096);
      for (;;) {
        std::size_t n = 0;
        if (StatusCode st = udp_.recv(buf, n, deadline); !is_good(st)) return st;
        auto r = fins::parse_response(std::span(buf.data(), n), fins::kMrcMemory, src);
        if (r && r->sid == sid) {
          out = std::move(*r);
          return status::Good;
        }
        // Late answer to an earlier request (or junk): keep waiting.
      }
    }
    if (!tcp_.is_open()) return status::BadNotConnected;
    StatusCode st = tcp_.send_all(fins::tcp_frame(req), deadline);
    std::uint8_t hdr[fins::kTcpHeader];
    if (is_good(st)) st = tcp_.recv_exact(hdr, deadline);
    if (is_good(st)) {
      auto h = fins::parse_tcp_header(hdr);
      if (!h || h->error != 0 || h->command != fins::kTcpFrame) {
        if (h && h->error != 0) host_.log.warn("FINS/TCP error: {} (0x{:X})", tcp_error_text(h->error), h->error);
        tcp_.close();
        return status::BadCommunicationError;
      }
      std::vector<std::uint8_t> body(h->length - 8);
      st = tcp_.recv_exact(body, deadline);
      if (is_good(st)) {
        auto r = fins::parse_response(body, fins::kMrcMemory, src);
        if (!r || r->sid != sid) {
          tcp_.close();
          return status::BadCommunicationError;
        }
        out = std::move(*r);
        return status::Good;
      }
    }
    // FINS/TCP answers in order: after a timeout the stream position is
    // unknown, so start over with a fresh connection.
    if (st == status::BadTimeout) tcp_.close();
    return st;
  }

  HostContext& host_;
  std::string host_name_;
  bool tcp_mode_ = false;
  std::uint16_t port_ = 9600, local_port_ = 0;
  int dest_node_ = -1, src_node_ = -1;
  fins::Header header_;
  std::uint8_t sid_ = 0;
  codec::ByteOrder order_ = codec::ByteOrder::CDAB;
  std::uint32_t word_gap_ = 16, bit_gap_ = 64;
  std::chrono::milliseconds request_timeout_{1000};
  TcpClient tcp_;
  UdpClient udp_;
  std::set<const FinsAddress*> invalid_, solo_;
};

nlohmann::json prop(const char* key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

class OmronFinsDriver final : public Driver {
 public:
  DriverInfo info() const override {
    return {"omron_fins", "Omron FINS Ethernet", "0.1.0", Medium::Ethernet, {"CS1", "CJ1", "CJ2", "CP1", "NJ/NX"}};
  }

  nlohmann::json schema() const override {
    using nlohmann::json;
    return {
        {"driver", "omron_fins"},
        {"display_name", "Omron FINS Ethernet"},
        {"channel", json::array()},
        {"device",
         json::array({
             prop("servermain.DEVICE_MODEL", "enum", "CJ2", "CS1, CJ1, CJ2, CP1, NJ/NX"),
             prop("servermain.DEVICE_ID_STRING", "string", "192.168.250.1", "PLC IP address or host name"),
             prop(kProtocol, "enum", "UDP", "UDP or TCP"),
             prop(kPort, "integer", 9600, "FINS port"),
             prop(kDestNetwork, "integer", 0, "Destination FINS network (0 = local)"),
             prop(kDestNode, "integer", nullptr, "Destination FINS node (default: UDP last IP octet, TCP from the PLC)"),
             prop(kDestUnit, "integer", 0, "Destination unit (0 = CPU unit)"),
             prop(kSrcNetwork, "integer", 0, "Source FINS network"),
             prop(kSrcNode, "integer", nullptr, "Source FINS node (default: UDP last local IP octet, TCP assigned by the PLC)"),
             prop(kLocalPort, "integer", 0, "Local UDP port (0 = any; 9600 for PLCs using a static IP table)"),
             prop(kFirstWordLow, "boolean", true, "32/64-bit values store the low word first"),
             prop(kWordGap, "integer", 16, "Unused words allowed between merged tags"),
             prop(kBitGap, "integer", 64, "Unused bits allowed between merged tags"),
         })},
        {"address_help",
         "CIO100, A10, D100, H5, W3, E0:100 (EM bank hex); bits CIO100.05, D100.15; timers/counters T5, C5 "
         "(PV; Boolean type = completion flag); strings D100.20; arrays D100 [10], CIO0.00 [16]."},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig&, const DeviceConfig& device,
                                                       HostContext& host) override {
    if (device.id.empty()) return make_error(status::BadConfigurationError, "device ID (IP address) is empty");
    const auto proto = upper(property<std::string>(device.properties, kProtocol, "UDP"));
    if (proto != "UDP" && proto != "TCP") return make_error(status::BadConfigurationError, "protocol must be UDP or TCP");
    return std::make_unique<FinsDevice>(device, host);
  }
};

}  // namespace

LOGICT_DECLARE_DRIVER(OmronFinsDriver)
