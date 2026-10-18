// Siemens TCP/IP Ethernet driver: S7comm over ISO-on-TCP (port 102) for
// S7-300/400/1200/1500 (PUT/GET, absolute addressing).
//
// S7-1200/1500 prerequisites: "Permit access with PUT/GET communication from
// remote partner" enabled, and "Optimized block access" disabled for every DB
// that is read. Optimized DBs are reached through the PLC's OPC UA server instead.
//
// Device ID: IP address or host name. Rack/slot default per model.

#include <algorithm>
#include <format>
#include <map>
#include <set>

#include "logict/sdk/block_planner.h"
#include "logict/sdk/driver.h"
#include "logict/sdk/tcp.h"
#include "s7_protocol.h"

namespace {

using namespace logict;
using namespace logict::sdk;
using s7::S7Address;

constexpr const char* kPort = "siemens_tcpip.DEVICE_PORT";
constexpr const char* kRack = "siemens_tcpip.DEVICE_RACK";
constexpr const char* kSlot = "siemens_tcpip.DEVICE_SLOT";
constexpr const char* kConnType = "siemens_tcpip.DEVICE_CONNECTION_TYPE";  // 1 PG, 2 OP, 3 S7 basic
constexpr const char* kLocalTsap = "siemens_tcpip.DEVICE_LOCAL_TSAP";
constexpr const char* kRemoteTsap = "siemens_tcpip.DEVICE_REMOTE_TSAP";  // 0 = from rack/slot
constexpr const char* kPduSize = "siemens_tcpip.DEVICE_PDU_SIZE";
constexpr const char* kMaxGap = "siemens_tcpip.DEVICE_MAX_GAP_BYTES";

int default_slot(const std::string& model) {
  if (model.find("300") != std::string::npos) return 2;
  if (model.find("400") != std::string::npos) return 3;
  return 1;  // S7-1200 / S7-1500
}

class S7Device final : public DeviceRuntime {
 public:
  S7Device(const DeviceConfig& cfg, HostContext& host) : host_(host) {
    const auto& p = cfg.properties;
    host_name_ = cfg.id;
    model_ = cfg.model.empty() ? "S7-1500" : cfg.model;
    port_ = property<std::uint16_t>(p, kPort, 102);
    const int rack = property(p, kRack, 0), slot = property(p, kSlot, default_slot(model_));
    const int conn_type = property(p, kConnType, 1);
    local_tsap_ = property<std::uint16_t>(p, kLocalTsap, 0x0100);
    remote_tsap_ = property<std::uint16_t>(p, kRemoteTsap, 0);
    if (remote_tsap_ == 0) remote_tsap_ = static_cast<std::uint16_t>((conn_type << 8) | (rack * 0x20 + slot));
    requested_pdu_ = std::clamp<std::uint16_t>(property<std::uint16_t>(p, kPduSize, 960), 240, 960);
    max_gap_ = property<std::uint32_t>(p, kMaxGap, 32);
    request_timeout_ = cfg.request_timeout;
    attempts_ = std::max(1, cfg.attempts);
    tcp_.set_observer(host.frames);
  }

  Result<std::shared_ptr<const Address>> parse_address(std::string_view text, TagType requested) override {
    auto a = s7::parse_address(text, requested);
    if (!a) return std::unexpected(a.error());
    return std::shared_ptr<const Address>(std::move(*a));
  }

  StatusCode connect(Deadline deadline) override {
    if (StatusCode st = tcp_.connect(host_name_, port_, deadline); !is_good(st)) {
      host_.log.debug("connect to {}:{} failed: {}", host_name_, port_, tcp_.last_error());
      return st;
    }
    // COTP connection request -> connection confirm.
    if (StatusCode st = tcp_.send_all(s7::connection_request(local_tsap_, remote_tsap_), deadline); !is_good(st)) return st;
    std::vector<std::uint8_t> cc;
    if (StatusCode st = recv_tpkt(cc, deadline); !is_good(st)) return fail_connect(st, "no COTP connection confirm");
    if (cc.size() < 2 || cc[1] != 0xD0)
      return fail_connect(status::BadNotConnected,
                          std::format("COTP connection refused (check rack/slot; remote TSAP 0x{:04X})", remote_tsap_));
    // S7 setup communication: negotiate the PDU size.
    s7::AckData ack;
    if (StatusCode st = transact(s7::setup_communication(next_ref(), requested_pdu_), ack, deadline); !is_good(st))
      return fail_connect(st, "S7 setup communication failed");
    if (ack.error_class != 0 || ack.param.size() < 8)
      return fail_connect(status::BadNotConnected, std::format("S7 setup rejected (error 0x{:02X}{:02X})", ack.error_class, ack.error_code));
    pdu_ = static_cast<std::uint16_t>((ack.param[6] << 8) | ack.param[7]);
    if (pdu_ < 240) pdu_ = 240;
    host_.log.info("connected to {} ({}, TSAP 0x{:04X}, PDU {} bytes)", host_name_, model_, remote_tsap_, pdu_);
    return status::Good;
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
      } else if (solo_.contains(&a)) {
        jobs.push_back(Job{a.area, a.db, a.byte_offset, a.byte_len, {i}});
      } else {
        plan.push_back({key(a), a.byte_offset, a.byte_len, i});
      }
    }
    for (auto& b : plan_blocks(plan, [this](std::uint32_t) { return PlanLimits{max_item_bytes(), max_gap_}; }))
      jobs.push_back(Job{static_cast<s7::Area>(b.area >> 16), static_cast<std::uint16_t>(b.area & 0xFFFF),
                         static_cast<std::uint32_t>(b.start), b.length, std::move(b.items)});
    return run_jobs(items, std::move(jobs), now);
  }

  StatusCode write(const Address& address, const Value& value) override {
    const auto& a = static_cast<const S7Address&>(address);
    auto bytes = s7::encode(a, value);
    if (!bytes) return bytes.error().code;
    if (a.bit >= 0) return write_once(a, a.byte_offset, a.bit, *bytes);
    const std::size_t chunk = (static_cast<std::size_t>(pdu_) - 28) & ~std::size_t{1};
    for (std::size_t off = 0; off < bytes->size(); off += chunk) {
      const std::size_t n = std::min(chunk, bytes->size() - off);
      StatusCode st = write_once(a, a.byte_offset + static_cast<std::uint32_t>(off), -1,
                                 std::span<const std::uint8_t>(bytes->data() + off, n));
      if (!is_good(st)) return st;
    }
    return status::Good;
  }

 private:
  struct Job {
    s7::Area area;
    std::uint16_t db;
    std::uint32_t start, length;
    std::vector<std::size_t> items;
  };
  struct Segment {
    std::size_t job;
    std::uint32_t start, length;
  };

  static const S7Address& addr(const ReadItem& item) { return static_cast<const S7Address&>(*item.address); }
  static std::uint32_t key(const S7Address& a) { return (static_cast<std::uint32_t>(a.area) << 16) | a.db; }
  std::uint32_t max_item_bytes() const { return (static_cast<std::uint32_t>(pdu_) - 18) & ~1u; }

  // Reads all jobs, packing segments into as few Read Var requests as the PDU allows.
  StatusCode run_jobs(std::span<ReadItem*> items, std::vector<Job> jobs, Timestamp now) {
    std::vector<Segment> segs;
    for (std::size_t j = 0; j < jobs.size(); ++j)
      for (std::uint32_t off = 0; off < jobs[j].length; off += max_item_bytes())
        segs.push_back({j, jobs[j].start + off, std::min(max_item_bytes(), jobs[j].length - off)});

    std::vector<std::vector<std::uint8_t>> data(jobs.size());
    std::vector<std::uint8_t> code(jobs.size(), s7::kItemOk);
    for (std::size_t i = 0; i < segs.size();) {
      std::vector<s7::ReadItem> req;
      std::vector<std::size_t> which;
      std::size_t resp = s7::kHeaderAck + 2;
      while (i < segs.size() && req.size() < s7::kMaxItems && s7::request_size(req.size() + 1) <= pdu_ &&
             resp + s7::response_item_size(segs[i].length) <= pdu_) {
        const auto& job = jobs[segs[i].job];
        req.push_back({job.area, job.db, segs[i].start, static_cast<std::uint16_t>(segs[i].length)});
        resp += s7::response_item_size(segs[i].length);
        which.push_back(i++);
      }
      s7::AckData ack;
      StatusCode st = transact(s7::read_var(next_ref(), req), ack, SteadyClock::now() + request_timeout_ * attempts_);
      if (!is_good(st)) return st;
      if (ack.error_class != 0) return header_error(ack);
      auto results = s7::parse_read_data(ack.data, req.size());
      if (!results) return results.error().code;
      for (std::size_t k = 0; k < which.size(); ++k) {
        const auto& seg = segs[which[k]];
        auto& r = (*results)[k];
        if (r.code != s7::kItemOk || r.bytes.size() != seg.length) {
          code[seg.job] = r.code == s7::kItemOk ? 0x00 : r.code;
          continue;
        }
        data[seg.job].insert(data[seg.job].end(), r.bytes.begin(), r.bytes.end());
      }
    }

    std::vector<Job> retry;
    for (std::size_t j = 0; j < jobs.size(); ++j) {
      const auto& job = jobs[j];
      if (code[j] == s7::kItemOk) {
        for (std::size_t idx : job.items) decode_into(*items[idx], data[j], job.start, now);
      } else if (job.items.size() == 1) {
        auto& item = *items[job.items[0]];
        item.quality = s7::item_status(code[j]);
        if (code[j] == 0x05 || code[j] == 0x0A) {
          invalid_.insert(&addr(item));
          host_.log.warn("'{}': {}; tag deactivated", item.address->text, s7::item_error_text(code[j]));
        }
      } else {
        // A merged range failed: read its tags one by one from now on.
        for (std::size_t idx : job.items) {
          const auto& a = addr(*items[idx]);
          solo_.insert(&a);
          retry.push_back(Job{a.area, a.db, a.byte_offset, a.byte_len, {idx}});
        }
      }
    }
    return retry.empty() ? status::Good : run_jobs(items, std::move(retry), now);
  }

  void decode_into(ReadItem& item, const std::vector<std::uint8_t>& data, std::uint32_t base, Timestamp now) {
    const auto& a = addr(item);
    auto v = s7::decode(a, std::span(data).subspan(a.byte_offset - base));
    item.quality = v ? status::Good : v.error().code;
    if (v) item.value = std::move(*v);
    item.source_time = now;
  }

  StatusCode write_once(const S7Address& a, std::uint32_t start, int bit, std::span<const std::uint8_t> bytes) {
    s7::AckData ack;
    StatusCode st = transact(s7::write_var(next_ref(), a.area, a.db, start, bit, bytes), ack,
                             SteadyClock::now() + request_timeout_ * attempts_);
    if (!is_good(st)) return st;
    if (ack.error_class != 0) return header_error(ack);
    if (ack.data.empty()) return status::BadCommunicationError;
    if (ack.data[0] != s7::kItemOk) {
      host_.log.warn("write to '{}' rejected: {}", a.text, s7::item_error_text(ack.data[0]));
      return s7::item_status(ack.data[0]);
    }
    return status::Good;
  }

  StatusCode header_error(const s7::AckData& ack) {
    const int code = (ack.error_class << 8) | ack.error_code;
    if (code == 0x8104)
      host_.log.error("PLC refused the request (0x8104): enable 'Permit access with PUT/GET communication' in TIA Portal");
    else
      host_.log.error("PLC returned S7 error 0x{:04X}", code);
    return status::BadDeviceFailure;
  }

  StatusCode fail_connect(StatusCode st, const std::string& why) {
    host_.log.warn("{}: {}", host_name_, why);
    tcp_.close();
    return st;
  }

  std::uint16_t next_ref() { return ++pdu_ref_ == 0 ? ++pdu_ref_ : pdu_ref_; }

  // One TPKT; for COTP DT frames, returns the S7 payload (reassembling
  // multi-part data units), otherwise the raw COTP part.
  StatusCode recv_tpkt(std::vector<std::uint8_t>& out, Deadline deadline) {
    out.clear();
    for (;;) {
      std::uint8_t hdr[4];
      if (StatusCode st = tcp_.recv_exact(hdr, deadline); !is_good(st)) return st;
      const std::size_t len = static_cast<std::size_t>((hdr[2] << 8) | hdr[3]);
      if (hdr[0] != 0x03 || len < 7 || len > 4096) {
        tcp_.close();
        return status::BadCommunicationError;
      }
      std::vector<std::uint8_t> body(len - 4);
      if (StatusCode st = tcp_.recv_exact(body, deadline); !is_good(st)) return st;
      if (body.size() < 2 || body[1] != 0xF0) {  // not a data transfer unit (e.g. connection confirm)
        out = std::move(body);
        return status::Good;
      }
      const std::size_t cotp_len = body[0] + 1u;
      if (cotp_len > body.size()) return status::BadCommunicationError;
      out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(cotp_len), body.end());
      if (body[2] & 0x80) return status::Good;  // last data unit
    }
  }

  // Sends a job and returns the ack with the same PDU reference; late answers
  // to earlier (timed-out) requests are discarded. Retries on timeout.
  StatusCode transact(const std::vector<std::uint8_t>& pdu, s7::AckData& out, Deadline overall) {
    if (!tcp_.is_open()) return status::BadNotConnected;
    std::vector<std::uint8_t> req = pdu;
    for (int attempt = 0; attempt < attempts_; ++attempt) {
      if (attempt > 0) {
        const std::uint16_t ref = next_ref();
        req[4] = static_cast<std::uint8_t>(ref >> 8);
        req[5] = static_cast<std::uint8_t>(ref);
      }
      const std::uint16_t ref = static_cast<std::uint16_t>((req[4] << 8) | req[5]);
      const Deadline deadline = std::min(overall, SteadyClock::now() + request_timeout_);
      StatusCode st = tcp_.send_all(s7::data_frame(req), deadline);
      while (is_good(st)) {
        std::vector<std::uint8_t> payload;
        st = recv_tpkt(payload, deadline);
        if (!is_good(st)) break;
        auto ack = s7::parse_ack(payload);
        if (!ack) return ack.error().code;
        if (ack->pdu_ref != ref) continue;  // stale
        out = std::move(*ack);
        return status::Good;
      }
      if (st != status::BadTimeout) return st;
      host_.log.debug("request timeout ({} of {})", attempt + 1, attempts_);
    }
    return status::BadTimeout;
  }

  HostContext& host_;
  std::string host_name_, model_;
  std::uint16_t port_ = 102, local_tsap_ = 0x0100, remote_tsap_ = 0, requested_pdu_ = 960, pdu_ = 240;
  std::uint32_t max_gap_ = 32;
  std::chrono::milliseconds request_timeout_{1000};
  int attempts_ = 3;
  TcpClient tcp_;
  std::uint16_t pdu_ref_ = 0;
  std::set<const S7Address*> invalid_, solo_;
};

nlohmann::json prop(const char* key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

class SiemensDriver final : public Driver {
 public:
  DriverInfo info() const override {
    return {"siemens_tcpip", "Siemens TCP/IP Ethernet", "0.1.0", Medium::Ethernet, {"S7-300", "S7-400", "S7-1200", "S7-1500"}};
  }

  nlohmann::json schema() const override {
    using nlohmann::json;
    return {
        {"driver", "siemens_tcpip"},
        {"display_name", "Siemens TCP/IP Ethernet"},
        {"channel", json::array()},
        {"device",
         json::array({
             prop("servermain.DEVICE_MODEL", "enum", "S7-1500", "S7-300, S7-400, S7-1200, S7-1500"),
             prop("servermain.DEVICE_ID_STRING", "string", "192.168.0.1", "PLC IP address or host name"),
             prop(kPort, "integer", 102, "ISO-on-TCP port"),
             prop(kRack, "integer", 0, "CPU rack"),
             prop(kSlot, "integer", nullptr, "CPU slot (default: S7-300 2, S7-400 3, S7-1200/1500 1)"),
             prop(kConnType, "integer", 1, "Connection type: 1 PG, 2 OP, 3 S7 basic"),
             prop(kLocalTsap, "integer", 0x0100, "Local TSAP"),
             prop(kRemoteTsap, "integer", 0, "Remote TSAP (0 = from connection type, rack and slot)"),
             prop(kPduSize, "integer", 960, "Requested PDU size (240-960)"),
             prop(kMaxGap, "integer", 32, "Unused bytes allowed between merged tags"),
         })},
        {"address_help",
         "I0.0 IB0 IW0 ID0 (E alias), Q0.0 QB0 QW0 QD0 (A alias), M10.3 MB10 MW10 MD10, DB1.DBX0.0 DB1.DBB0 DB1.DBW0 "
         "DB1.DBD0, DB1.STRING10.20; arrays: DB1.DBW0 [10]. Data type sets the interpretation (e.g. DBD + Float = REAL)."},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig&, const DeviceConfig& device,
                                                       HostContext& host) override {
    if (device.id.empty()) return make_error(status::BadConfigurationError, "device ID (IP address) is empty");
    return std::make_unique<S7Device>(device, host);
  }
};

}  // namespace

LOGICT_DECLARE_DRIVER(SiemensDriver)
