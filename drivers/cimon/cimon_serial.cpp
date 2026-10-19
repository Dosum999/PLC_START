// CIMON Serial driver: computer link protocol (ASCII, ENQ/STX framing with
// BCC) through the CPU loader/communication port or a CIMON serial module.
// The channel is the serial line; each device is one station (device ID =
// station number 0-31). Devices of a channel share the port.

#include <charconv>
#include <map>
#include <mutex>

#include "cimon_device.h"
#include "cimon_drivers.h"
#include "logict/sdk/serial.h"

namespace {

using namespace logict;
using namespace logict::sdk;
using cimon::prop;

constexpr const char* kWordGap = "cimon_serial.DEVICE_MAX_WORD_GAP";
constexpr const char* kBitGap = "cimon_serial.DEVICE_MAX_BIT_GAP";

struct Line {
  SerialSettings settings;
  SerialPort port;
};

class CimonSerialDevice final : public cimon::CimonDeviceBase {
 public:
  CimonSerialDevice(std::shared_ptr<Line> line, std::uint8_t station, const DeviceConfig& cfg, HostContext& host)
      : CimonDeviceBase(host,
                        cimon::Limits{cimon::kSerialReadWords, cimon::kSerialWriteWords, cimon::kSerialReadBits,
                                      cimon::kSerialWriteBits},
                        property<std::uint32_t>(cfg.properties, kWordGap, 8),
                        property<std::uint32_t>(cfg.properties, kBitGap, 32)),
        line_(std::move(line)),
        station_(station) {
    request_timeout_ = cfg.request_timeout;
    attempts_ = std::max(1, cfg.attempts);
    line_->port.set_observer(host.frames);
  }

  StatusCode connect(Deadline) override {
    if (!line_->port.is_open()) {
      StatusCode st = line_->port.open(line_->settings);
      if (!is_good(st)) {
        host_.log.warn("cannot open serial port: {}", line_->port.last_error());
        return st;
      }
    }
    online_ = true;
    return status::Good;
  }
  // A silent station must not close the port its neighbours use.
  void disconnect() override { online_ = false; }
  bool connected() const override { return online_ && line_->port.is_open(); }

 protected:
  StatusCode read_raw(const cimon::Device& d, bool bit_units, std::uint32_t start, std::uint32_t count,
                      cimon::Answer& out) override {
    return transact(cimon::serial_read(station_, d, bit_units, start, count), bit_units ? 'r' : 'R', bit_units, count, out);
  }

  StatusCode write_raw(const cimon::Device& d, bool bit_units, std::uint32_t start, std::span<const std::uint8_t> raw,
                       int& error) override {
    cimon::Answer ans;
    StatusCode st = transact(cimon::serial_write(station_, d, bit_units, start, raw), bit_units ? 'w' : 'W', bit_units, 0, ans);
    error = ans.error;
    return st;
  }

  StatusCode error_status(int code) const override { return cimon::serial_error_status(code); }

 private:
  StatusCode transact(const std::vector<std::uint8_t>& req, char cmd, bool bit_units, std::uint32_t count,
                      cimon::Answer& out) {
    auto& port = line_->port;
    if (!port.is_open()) return status::BadNotConnected;
    for (int attempt = 0; attempt < attempts_; ++attempt) {
      port.flush_input();
      StatusCode st = port.write_all(req, SteadyClock::now() + request_timeout_);
      if (!is_good(st)) return st;
      std::vector<std::uint8_t> frame;
      st = receive(frame, SteadyClock::now() + request_timeout_);
      if (is_good(st)) {
        auto r = cimon::parse_serial(frame, station_, cmd, bit_units, count);
        if (r) {
          out = std::move(*r);
          return status::Good;
        }
        st = r.error().code;
        host_.log.debug("station {}: {}", station_, r.error().message);
      }
      if (st != status::BadTimeout && st != status::BadCommunicationError) return st;
    }
    return status::BadTimeout;
  }

  // STX, station (2), command, length (2 hex), data, BCC (2), ETX.
  StatusCode receive(std::vector<std::uint8_t>& frame, Deadline deadline) {
    auto& port = line_->port;
    std::uint8_t c = 0;
    do {
      if (StatusCode st = port.read_exact(std::span(&c, 1), deadline); !is_good(st)) return st;
    } while (c != 0x02);
    frame.assign(6, 0);
    frame[0] = c;
    if (StatusCode st = port.read_exact(std::span(frame).subspan(1), deadline); !is_good(st)) return st;
    unsigned len = 0;
    auto r = std::from_chars(reinterpret_cast<const char*>(frame.data()) + 4, reinterpret_cast<const char*>(frame.data()) + 6, len, 16);
    if (r.ec != std::errc{}) return status::BadCommunicationError;
    frame.resize(6 + len + 3);
    return port.read_exact(std::span(frame).subspan(6), deadline);
  }

  std::shared_ptr<Line> line_;
  std::uint8_t station_;
  std::chrono::milliseconds request_timeout_{1000};
  int attempts_ = 3;
  bool online_ = false;
};

class CimonSerialDriver final : public Driver {
 public:
  DriverInfo info() const override { return {"cimon_serial", "CIMON Serial", "0.1.0", Medium::Serial, {"CP", "XP", "PLC-S"}}; }

  nlohmann::json schema() const override {
    using nlohmann::json;
    return {
        {"driver", "cimon_serial"},
        {"display_name", "CIMON Serial"},
        {"channel",
         json::array({
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_PORT", "string", "/dev/ttyUSB0", "Serial device"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_BAUD_RATE", "integer", 9600, "Baud rate"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_DATA_BITS", "integer", 8, "Data bits"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_PARITY", "enum", "None", "None, Even, Odd"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_STOP_BITS", "integer", 1, "Stop bits"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_RS485", "boolean", false, "Kernel RS-485 mode (RTS on send)"),
         })},
        {"device",
         json::array({
             prop("servermain.DEVICE_ID_STRING", "string", "0", "PLC station number (0-31)"),
             prop(kWordGap, "integer", 8, "Unused words allowed between merged tags"),
             prop(kBitGap, "integer", 32, "Unused bits allowed between merged tags"),
         })},
        {"address_help", cimon::kAddressHelp},
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
    return std::make_unique<CimonSerialDevice>(line, static_cast<std::uint8_t>(station), device, host);
  }

 private:
  std::mutex mu_;
  std::map<std::string, std::weak_ptr<Line>> lines_;
};

}  // namespace

LOGICT_DECLARE_DRIVER(CimonSerialDriver)
