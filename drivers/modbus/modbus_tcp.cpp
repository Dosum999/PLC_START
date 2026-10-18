// Modbus TCP/IP Ethernet driver (Kepware "Modbus TCP/IP Ethernet" compatible
// addressing and device properties).
//
// Device ID: "<ip>", "<ip>.<unit>" (Kepware style, e.g. 192.168.0.10.1) or
// "<host>.<unit>" with the host in angle brackets, e.g. "<plc1.local>.1".

#include <algorithm>
#include <charconv>
#include <format>
#include <set>

#include "logict/sdk/driver.h"
#include "logict/sdk/tcp.h"
#include "modbus_device.h"

namespace {

using namespace logict;
using namespace logict::sdk;

constexpr const char* kPort = "modbus_tcp.DEVICE_PORT";
constexpr const char* kUnit = "modbus_tcp.DEVICE_UNIT_ID";

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

class ModbusTcpDevice final : public modbus::ModbusDevice {
 public:
  ModbusTcpDevice(Endpoint ep, const DeviceConfig& cfg, HostContext& host)
      : ModbusDevice(cfg, host, "modbus_tcp"), ep_(std::move(ep)) {
    port_ = property<std::uint16_t>(cfg.properties, kPort, 502);
    tcp_.set_observer(host.frames);
  }

  StatusCode connect(Deadline deadline) override {
    StatusCode st = tcp_.connect(ep_.host, port_, deadline);
    if (st != status::Good)
      host_.log.debug("connect to {}:{} failed: {}", ep_.host, port_, tcp_.last_error());
    return st;
  }
  void disconnect() override { tcp_.close(); }
  bool connected() const override { return tcp_.is_open(); }

 private:
  // Sends one request and returns the matching response PDU. Retries on
  // timeout up to the configured attempts; discards stale responses.
  StatusCode transact(const std::vector<std::uint8_t>& pdu, std::vector<std::uint8_t>& resp) override {
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
  std::uint16_t port_ = 502;
  TcpClient tcp_;
  std::uint16_t tid_ = 0;
};

nlohmann::json prop(const char* key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

nlohmann::json device_schema() {
  nlohmann::json props = nlohmann::json::array({
      prop("servermain.DEVICE_ID_STRING", "string", "127.0.0.1",
           "IP address, optionally with unit ID (192.168.0.10.1) or <hostname>.unit"),
      prop(kPort, "integer", 502, "TCP port"),
      prop(kUnit, "integer", 1, "Unit ID when not given in the device ID (0-255)"),
  });
  for (auto& p : modbus::common_device_schema("modbus_tcp")) props.push_back(std::move(p));
  return props;
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
        {"device", device_schema()},
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
