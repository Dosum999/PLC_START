// CIMON Ethernet driver: the exclusive (HMI) service of CIMON Ethernet
// modules and CPUs with built-in Ethernet, "KDT_PLC_M" frames over TCP 10260.
//
// Device ID: IP address or host name.

#include "cimon_device.h"
#include "cimon_drivers.h"
#include "logict/sdk/tcp.h"

namespace {

using namespace logict;
using namespace logict::sdk;
using cimon::prop;

constexpr const char* kPort = "cimon_ethernet.DEVICE_PORT";
constexpr const char* kWordGap = "cimon_ethernet.DEVICE_MAX_WORD_GAP";
constexpr const char* kBitGap = "cimon_ethernet.DEVICE_MAX_BIT_GAP";

class CimonEnetDevice final : public cimon::CimonDeviceBase {
 public:
  CimonEnetDevice(const DeviceConfig& cfg, HostContext& host)
      : CimonDeviceBase(host,
                        cimon::Limits{cimon::kEnetWords, cimon::kEnetWords, cimon::kEnetBits, cimon::kEnetWriteBits},
                        property<std::uint32_t>(cfg.properties, kWordGap, 16),
                        property<std::uint32_t>(cfg.properties, kBitGap, 64)) {
    host_name_ = cfg.id;
    port_ = property<std::uint16_t>(cfg.properties, kPort, 10260);
    request_timeout_ = cfg.request_timeout;
    tcp_.set_observer(host.frames);
  }

  StatusCode connect(Deadline deadline) override {
    StatusCode st = tcp_.connect(host_name_, port_, deadline);
    if (st != status::Good) host_.log.debug("connect to {}:{} failed: {}", host_name_, port_, tcp_.last_error());
    return st;
  }
  void disconnect() override { tcp_.close(); }
  bool connected() const override { return tcp_.is_open(); }

 protected:
  StatusCode read_raw(const cimon::Device& d, bool bit_units, std::uint32_t start, std::uint32_t count,
                      cimon::Answer& out) override {
    const std::uint8_t no = next_frame();
    return transact(cimon::enet_read(no, d, bit_units, start, count), no, bit_units, count, false, out);
  }

  StatusCode write_raw(const cimon::Device& d, bool bit_units, std::uint32_t start, std::span<const std::uint8_t> raw,
                       int& error) override {
    const std::uint8_t no = next_frame();
    cimon::Answer ans;
    StatusCode st = transact(cimon::enet_write(no, d, bit_units, start, raw), no, bit_units, 0, true, ans);
    error = ans.error;
    return st;
  }

  StatusCode error_status(int code) const override { return cimon::enet_error_status(code); }

 private:
  // Frame numbers run 0-127; the PLC ignores a repeated number while busy.
  std::uint8_t next_frame() {
    frame_no_ = static_cast<std::uint8_t>((frame_no_ + 1) & 0x7F);
    return frame_no_;
  }

  StatusCode transact(const std::vector<std::uint8_t>& req, std::uint8_t no, bool bit_units, std::uint32_t count, bool is_write,
                      cimon::Answer& out) {
    if (!tcp_.is_open()) return status::BadNotConnected;
    const Deadline deadline = SteadyClock::now() + request_timeout_;
    StatusCode st = tcp_.send_all(req, deadline);
    std::vector<std::uint8_t> frame(cimon::kEnetHeader);
    if (is_good(st)) st = tcp_.recv_exact(frame, deadline);
    if (is_good(st)) {
      auto len = cimon::enet_length(frame);
      if (!len) {
        tcp_.close();
        return status::BadCommunicationError;
      }
      frame.resize(cimon::kEnetHeader + *len + 2);
      st = tcp_.recv_exact(std::span(frame).subspan(cimon::kEnetHeader), deadline);
      if (is_good(st)) {
        auto r = cimon::parse_enet(frame, no, bit_units, count, is_write);
        if (!r) {
          host_.log.warn("{}", r.error().message);
          tcp_.close();  // stream position unknown
          return r.error().code;
        }
        out = std::move(*r);
        return status::Good;
      }
    }
    if (st == status::BadTimeout) tcp_.close();
    return st;
  }

  std::string host_name_;
  std::uint16_t port_ = 10260;
  std::chrono::milliseconds request_timeout_{1000};
  std::uint8_t frame_no_ = 0;
  TcpClient tcp_;
};

class CimonEthernetDriver final : public Driver {
 public:
  DriverInfo info() const override {
    return {"cimon_ethernet", "CIMON Ethernet", "0.1.0", Medium::Ethernet, {"CP", "XP", "PLC-S"}};
  }

  nlohmann::json schema() const override {
    using nlohmann::json;
    return {
        {"driver", "cimon_ethernet"},
        {"display_name", "CIMON Ethernet"},
        {"channel", json::array()},
        {"device",
         json::array({
             prop("servermain.DEVICE_ID_STRING", "string", "192.168.0.10", "PLC IP address or host name"),
             prop(kPort, "integer", 10260, "Exclusive service TCP port (fixed 10260 on CIMON modules)"),
             prop(kWordGap, "integer", 16, "Unused words allowed between merged tags"),
             prop(kBitGap, "integer", 64, "Unused bits allowed between merged tags"),
         })},
        {"address_help", cimon::kAddressHelp},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig&, const DeviceConfig& device,
                                                       HostContext& host) override {
    if (device.id.empty()) return make_error(status::BadConfigurationError, "device ID (IP address) is empty");
    return std::make_unique<CimonEnetDevice>(device, host);
  }
};

}  // namespace

LOGICT_DECLARE_DRIVER(CimonEthernetDriver)
