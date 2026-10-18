#pragma once

// Modbus over a serial line, shared by the RTU and ASCII drivers. The channel
// is the serial line (port, baud, parity, ...); each device is one station
// (device ID 1-247, 0 = broadcast, write only). Devices of a channel share the
// port; the channel thread serialises their requests as Modbus requires.

#include <charconv>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "logict/sdk/serial.h"
#include "modbus_device.h"

namespace logict::modbus {

enum class Framing { Rtu, Ascii };

// One serial port shared by all devices of a channel.
struct SerialLine {
  sdk::SerialSettings settings;
  sdk::SerialPort port;
};

class ModbusSerialDevice final : public ModbusDevice {
 public:
  ModbusSerialDevice(std::shared_ptr<SerialLine> line, std::uint8_t station, Framing framing, const char* prefix,
                     const sdk::DeviceConfig& cfg, sdk::HostContext& host)
      : ModbusDevice(cfg, host, prefix), line_(std::move(line)), station_(station), framing_(framing) {
    broadcast_delay_ =
        std::chrono::milliseconds(sdk::property(cfg.properties, std::string(prefix) + ".DEVICE_BROADCAST_DELAY_MS", 100));
    line_->port.set_observer(host.frames);
  }

  sdk::StatusCode connect(sdk::Deadline) override {
    if (!line_->port.is_open()) {
      sdk::StatusCode st = line_->port.open(line_->settings);
      if (!sdk::is_good(st)) {
        host_.log.warn("cannot open serial port: {}", line_->port.last_error());
        return st;
      }
      host_.log.info("opened {} ({} {}{}{})", line_->settings.port, line_->settings.baud, line_->settings.data_bits,
                     line_->settings.parity, line_->settings.stop_bits);
    }
    online_ = true;
    return sdk::status::Good;
  }
  // A silent station must not close the port its neighbours use.
  void disconnect() override { online_ = false; }
  bool connected() const override { return online_ && line_->port.is_open(); }

 protected:
  sdk::StatusCode transact(const std::vector<std::uint8_t>& pdu, std::vector<std::uint8_t>& resp) override {
    namespace status = sdk::status;
    auto& port = line_->port;
    if (!port.is_open()) return status::BadNotConnected;
    std::vector<std::uint8_t> adu{station_};
    adu.insert(adu.end(), pdu.begin(), pdu.end());
    if (framing_ == Framing::Rtu) {
      const std::uint16_t crc = crc16(adu);
      adu.push_back(static_cast<std::uint8_t>(crc));
      adu.push_back(static_cast<std::uint8_t>(crc >> 8));
    } else {
      adu = ascii_frame(adu);
    }
    // RTU inter-frame silence: 3.5 characters, at least 1.75 ms above 19200 baud.
    const auto silence = std::max(port.char_time() * 7 / 2, std::chrono::microseconds(1750));

    for (int attempt = 0; attempt < attempts_; ++attempt) {
      if (framing_ == Framing::Rtu) port.wait_silence(silence, sdk::SteadyClock::now() + request_timeout_);
      port.flush_input();
      sdk::StatusCode st = port.write_all(adu, sdk::SteadyClock::now() + request_timeout_);
      if (!sdk::is_good(st)) return st;
      if (station_ == 0) {  // broadcast: no answer, give stations time to process
        std::this_thread::sleep_for(broadcast_delay_);
        resp = {pdu[0]};
        resp.insert(resp.end(), pdu.begin() + 1, pdu.begin() + std::min<std::ptrdiff_t>(5, static_cast<std::ptrdiff_t>(pdu.size())));
        return status::Good;
      }
      const sdk::Deadline deadline = sdk::SteadyClock::now() + request_timeout_;
      st = framing_ == Framing::Rtu ? receive_rtu(pdu[0], resp, deadline) : receive_ascii(pdu[0], resp, deadline);
      if (sdk::is_good(st)) return status::Good;
      if (st != status::BadTimeout && st != status::BadCommunicationError) return st;
      host_.log.debug("station {}: {} ({} of {})", station_, sdk::status_name(st), attempt + 1, attempts_);
    }
    return status::BadTimeout;
  }

 private:
  sdk::StatusCode receive_rtu(std::uint8_t function, std::vector<std::uint8_t>& resp, sdk::Deadline deadline) {
    namespace status = sdk::status;
    auto& port = line_->port;
    std::uint8_t head[3];
    if (sdk::StatusCode st = port.read_exact(std::span(head, 3), deadline); !sdk::is_good(st)) return st;
    if (head[0] != station_ || (head[1] & 0x7F) != function) return status::BadCommunicationError;
    const std::size_t rest = rtu_remaining(head[1], head[2]);
    if (rest < 3 || rest > 260) return status::BadCommunicationError;
    std::vector<std::uint8_t> frame(head, head + 3);
    frame.resize(3 + rest - 1);
    if (sdk::StatusCode st = port.read_exact(std::span(frame).subspan(3), deadline); !sdk::is_good(st)) return st;
    const std::uint16_t crc = static_cast<std::uint16_t>(frame[frame.size() - 2] | (frame[frame.size() - 1] << 8));
    if (crc16(std::span(frame).first(frame.size() - 2)) != crc) return status::BadCommunicationError;
    resp.assign(frame.begin() + 1, frame.end() - 2);
    return status::Good;
  }

  sdk::StatusCode receive_ascii(std::uint8_t function, std::vector<std::uint8_t>& resp, sdk::Deadline deadline) {
    namespace status = sdk::status;
    auto& port = line_->port;
    // Skip to ':' (a new start character also restarts the frame), then read to LF.
    std::string text;
    bool started = false;
    for (;;) {
      std::uint8_t c;
      if (sdk::StatusCode st = port.read_exact(std::span(&c, 1), deadline); !sdk::is_good(st)) return st;
      if (c == ':') {
        started = true;
        text.clear();
      } else if (!started) {
        continue;
      } else if (c == '\n') {
        break;
      } else if (c != '\r') {
        if (text.size() >= 2 * 256) return status::BadCommunicationError;
        text.push_back(static_cast<char>(c));
      }
    }
    auto frame = parse_ascii_frame(text);
    if (!frame) return frame.error().code;
    if (frame->size() < 2 || (*frame)[0] != station_ || ((*frame)[1] & 0x7F) != function) return status::BadCommunicationError;
    resp.assign(frame->begin() + 1, frame->end());
    return status::Good;
  }

  std::shared_ptr<SerialLine> line_;
  std::uint8_t station_;
  Framing framing_;
  bool online_ = false;
  std::chrono::milliseconds broadcast_delay_{100};
};

inline nlohmann::json serial_prop(const std::string& key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

// Driver for one framing; devices of a channel share its serial line.
class ModbusSerialDriver : public sdk::Driver {
 public:
  ModbusSerialDriver(Framing framing, const char* id, const char* display) : framing_(framing), id_(id), display_(display) {}

  sdk::DriverInfo info() const override { return {id_, display_, "0.1.0", sdk::Medium::Serial, {"Modbus"}}; }

  nlohmann::json schema() const override {
    using nlohmann::json;
    const std::string prefix = id_;
    json device = json::array({
        serial_prop("servermain.DEVICE_ID_STRING", "string", "1", "Station address 1-247 (0 = broadcast, writes only)"),
        serial_prop(prefix + ".DEVICE_BROADCAST_DELAY_MS", "integer", 100, "Delay after a broadcast write (ms)"),
    });
    for (auto& p : common_device_schema(id_)) device.push_back(std::move(p));
    return {
        {"driver", id_},
        {"display_name", display_},
        {"channel",
         json::array({
             serial_prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_PORT", "string", "/dev/ttyUSB0", "Serial device"),
             serial_prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_BAUD_RATE", "integer", 9600, "Baud rate"),
             serial_prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_DATA_BITS", "integer", 8, "Data bits (7 or 8; Modbus ASCII devices often use 7)"),
             serial_prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_PARITY", "enum", "None", "None, Even, Odd"),
             serial_prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_STOP_BITS", "integer", 1, "Stop bits (1 or 2)"),
             serial_prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_RS485", "boolean", false, "Kernel RS-485 mode (RTS on send)"),
         })},
        {"device", device},
        {"address_help", "Same as Modbus TCP: 0xxxxx, 1xxxxx, 3xxxxx, 4xxxxx, .bit, .nH/.nL strings, arrays."},
    };
  }

  sdk::Result<std::unique_ptr<sdk::DeviceRuntime>> create_device(const sdk::ChannelConfig& channel,
                                                                 const sdk::DeviceConfig& device,
                                                                 sdk::HostContext& host) override {
    unsigned station = 0;
    auto r = std::from_chars(device.id.data(), device.id.data() + device.id.size(), station);
    if (device.id.empty() || r.ec != std::errc{} || r.ptr != device.id.data() + device.id.size() || station > 247)
      return sdk::make_error(sdk::status::BadConfigurationError, "device ID must be a station address 0-247");
    auto settings = sdk::serial_settings(channel.properties);
    if (!settings) return std::unexpected(settings.error());

    std::lock_guard lock(mu_);
    auto& weak = lines_[channel.name];
    auto line = weak.lock();
    if (!line) {
      line = std::make_shared<SerialLine>();
      line->settings = *settings;
      weak = line;
    }
    return std::make_unique<ModbusSerialDevice>(line, static_cast<std::uint8_t>(station), framing_, id_, device, host);
  }

 private:
  Framing framing_;
  const char* id_;
  const char* display_;
  std::mutex mu_;
  std::map<std::string, std::weak_ptr<SerialLine>> lines_;
};

}  // namespace logict::modbus
