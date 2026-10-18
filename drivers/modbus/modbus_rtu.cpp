// Modbus RTU Serial driver. The channel is the serial line (port, baud,
// parity, ...); each device is one station (device ID 1-247, 0 = broadcast,
// write only). Devices of a channel share the port; the channel thread
// serialises their requests as Modbus RTU requires.

#include <charconv>
#include <format>
#include <map>
#include <mutex>
#include <thread>

#include "logict/sdk/serial.h"
#include "modbus_device.h"

namespace {

using namespace logict;
using namespace logict::sdk;

constexpr const char* kBroadcastDelay = "modbus_rtu.DEVICE_BROADCAST_DELAY_MS";

// One serial port shared by all devices of a channel.
struct Line {
  SerialSettings settings;
  SerialPort port;
};

class ModbusRtuDevice final : public modbus::ModbusDevice {
 public:
  ModbusRtuDevice(std::shared_ptr<Line> line, std::uint8_t station, const DeviceConfig& cfg, HostContext& host)
      : ModbusDevice(cfg, host, "modbus_rtu"), line_(std::move(line)), station_(station) {
    broadcast_delay_ = std::chrono::milliseconds(property(cfg.properties, kBroadcastDelay, 100));
    line_->port.set_observer(host.frames);
  }

  StatusCode connect(Deadline) override {
    if (!line_->port.is_open()) {
      StatusCode st = line_->port.open(line_->settings);
      if (!is_good(st)) {
        host_.log.warn("cannot open serial port: {}", line_->port.last_error());
        return st;
      }
      host_.log.info("opened {} ({} {}{}{})", line_->settings.port, line_->settings.baud, line_->settings.data_bits,
                     line_->settings.parity, line_->settings.stop_bits);
    }
    online_ = true;
    return status::Good;
  }
  // A silent station must not close the port its neighbours use.
  void disconnect() override { online_ = false; }
  bool connected() const override { return online_ && line_->port.is_open(); }

 protected:
  StatusCode transact(const std::vector<std::uint8_t>& pdu, std::vector<std::uint8_t>& resp) override {
    auto& port = line_->port;
    if (!port.is_open()) return status::BadNotConnected;
    std::vector<std::uint8_t> adu{station_};
    adu.insert(adu.end(), pdu.begin(), pdu.end());
    const std::uint16_t crc = modbus::crc16(adu);
    adu.push_back(static_cast<std::uint8_t>(crc));
    adu.push_back(static_cast<std::uint8_t>(crc >> 8));
    // Inter-frame silence: 3.5 characters, at least 1.75 ms above 19200 baud.
    const auto silence = std::max(port.char_time() * 7 / 2, std::chrono::microseconds(1750));

    for (int attempt = 0; attempt < attempts_; ++attempt) {
      port.wait_silence(silence, SteadyClock::now() + request_timeout_);
      port.flush_input();
      StatusCode st = port.write_all(adu, SteadyClock::now() + request_timeout_);
      if (!is_good(st)) return st;
      if (station_ == 0) {  // broadcast: no answer, give stations time to process
        std::this_thread::sleep_for(broadcast_delay_);
        resp = {pdu[0]};
        resp.insert(resp.end(), pdu.begin() + 1, pdu.begin() + std::min<std::ptrdiff_t>(5, static_cast<std::ptrdiff_t>(pdu.size())));
        return status::Good;
      }
      const Deadline deadline = SteadyClock::now() + request_timeout_;
      st = receive(pdu[0], resp, deadline);
      if (is_good(st)) return status::Good;
      if (st != status::BadTimeout && st != status::BadCommunicationError) return st;
      host_.log.debug("station {}: {} ({} of {})", station_, status_name(st), attempt + 1, attempts_);
    }
    return status::BadTimeout;
  }

 private:
  StatusCode receive(std::uint8_t function, std::vector<std::uint8_t>& resp, Deadline deadline) {
    auto& port = line_->port;
    std::uint8_t head[3];
    if (StatusCode st = port.read_exact(std::span(head, 3), deadline); !is_good(st)) return st;
    if (head[0] != station_ || (head[1] & 0x7F) != function) return status::BadCommunicationError;
    const std::size_t rest = modbus::rtu_remaining(head[1], head[2]);
    if (rest < 3 || rest > 260) return status::BadCommunicationError;
    std::vector<std::uint8_t> frame(head, head + 3);
    frame.resize(3 + rest - 1);
    if (StatusCode st = port.read_exact(std::span(frame).subspan(3), deadline); !is_good(st)) return st;
    const std::uint16_t crc = static_cast<std::uint16_t>(frame[frame.size() - 2] | (frame[frame.size() - 1] << 8));
    if (modbus::crc16(std::span(frame).first(frame.size() - 2)) != crc) return status::BadCommunicationError;
    resp.assign(frame.begin() + 1, frame.end() - 2);
    return status::Good;
  }

  std::shared_ptr<Line> line_;
  std::uint8_t station_;
  bool online_ = false;
  std::chrono::milliseconds broadcast_delay_{100};
};

nlohmann::json prop(const char* key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

class ModbusRtuDriver final : public Driver {
 public:
  DriverInfo info() const override { return {"modbus_rtu", "Modbus RTU Serial", "0.1.0", Medium::Serial, {"Modbus"}}; }

  nlohmann::json schema() const override {
    using nlohmann::json;
    json device = json::array({
        prop("servermain.DEVICE_ID_STRING", "string", "1", "Station address 1-247 (0 = broadcast, writes only)"),
        prop(kBroadcastDelay, "integer", 100, "Delay after a broadcast write (ms)"),
    });
    for (auto& p : modbus::common_device_schema("modbus_rtu")) device.push_back(std::move(p));
    return {
        {"driver", "modbus_rtu"},
        {"display_name", "Modbus RTU Serial"},
        {"channel",
         json::array({
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_PORT", "string", "/dev/ttyUSB0", "Serial device"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_BAUD_RATE", "integer", 9600, "Baud rate"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_DATA_BITS", "integer", 8, "Data bits (7 or 8)"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_PARITY", "enum", "None", "None, Even, Odd"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_STOP_BITS", "integer", 1, "Stop bits (1 or 2)"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_RS485", "boolean", false, "Kernel RS-485 mode (RTS on send)"),
         })},
        {"device", device},
        {"address_help", "Same as Modbus TCP: 0xxxxx, 1xxxxx, 3xxxxx, 4xxxxx, .bit, .nH/.nL strings, arrays."},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig& channel, const DeviceConfig& device,
                                                       HostContext& host) override {
    unsigned station = 0;
    auto r = std::from_chars(device.id.data(), device.id.data() + device.id.size(), station);
    if (device.id.empty() || r.ec != std::errc{} || r.ptr != device.id.data() + device.id.size() || station > 247)
      return make_error(status::BadConfigurationError, "device ID must be a station address 0-247");
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
    return std::make_unique<ModbusRtuDevice>(line, static_cast<std::uint8_t>(station), device, host);
  }

 private:
  std::mutex mu_;
  std::map<std::string, std::weak_ptr<Line>> lines_;
};

}  // namespace

LOGICT_DECLARE_DRIVER(ModbusRtuDriver)
