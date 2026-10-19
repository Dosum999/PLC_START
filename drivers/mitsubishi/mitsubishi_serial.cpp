// Mitsubishi Serial driver: MC protocol over a serial communication module
// (QJ71C24, LJ71C24, RJ71C24) with the 3C or 4C frame, ASCII format 1 or 4.
// The channel is the serial line; each device is one station (device ID =
// station number 0-31). Devices of a channel share the port.

#include <charconv>
#include <map>
#include <mutex>

#include "logict/sdk/serial.h"
#include "mc_device.h"

namespace {

using namespace logict;
using namespace logict::sdk;
using mc::McAddress;

constexpr const char* kFrame = "mitsubishi_serial.DEVICE_FRAME";
constexpr const char* kFormat = "mitsubishi_serial.DEVICE_FORMAT";
constexpr const char* kSum = "mitsubishi_serial.DEVICE_SUM_CHECK";
constexpr const char* kNetwork = "mitsubishi_serial.DEVICE_NETWORK_NUMBER";
constexpr const char* kPc = "mitsubishi_serial.DEVICE_PC_NUMBER";
constexpr const char* kXyOctal = "mitsubishi_serial.DEVICE_XY_OCTAL";
constexpr const char* kWordGap = "mitsubishi_serial.DEVICE_MAX_WORD_GAP";
constexpr const char* kBitGap = "mitsubishi_serial.DEVICE_MAX_BIT_GAP";

struct Line {
  SerialSettings settings;
  SerialPort port;
};

class McSerialDevice final : public mc::McDeviceBase {
 public:
  McSerialDevice(std::shared_ptr<Line> line, std::uint8_t station, const DeviceConfig& cfg, HostContext& host)
      : McDeviceBase(host, mc::AddressOptions{.xy_octal = property(cfg.properties, kXyOctal, false)},
                     property<std::uint32_t>(cfg.properties, kWordGap, 16),
                     property<std::uint32_t>(cfg.properties, kBitGap, 64)),
        line_(std::move(line)) {
    const auto& p = cfg.properties;
    frame_.four_c = property<std::string>(p, kFrame, "3C") == "4C";
    frame_.format4 = property(p, kFormat, 4) == 4;
    frame_.sum_check = property(p, kSum, true);
    frame_.station = station;
    frame_.network = static_cast<std::uint8_t>(property(p, kNetwork, 0));
    frame_.pc = static_cast<std::uint8_t>(property(p, kPc, 0xFF));
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
  std::uint32_t max_words() const override { return mc::kMaxWordsC; }
  std::uint32_t max_bits() const override { return mc::kMaxBitsC; }
  StatusCode end_status(std::uint16_t code) const override { return mc::end_code_status(code); }

  StatusCode read_raw(const mc::Device& d, std::uint32_t number, std::uint16_t points, bool bit_units,
                      std::vector<std::uint8_t>& out, std::uint16_t* end_code = nullptr) override {
    mc::Response r;
    StatusCode st = transact(mc::c_batch_read(frame_, d, base_of(d), number, points, bit_units), bit_units, points, r);
    if (!is_good(st)) return st;
    if (end_code) *end_code = r.end_code;
    if (r.end_code != 0) return end_code ? status::Good : mc::end_code_status(r.end_code);
    if (r.data.size() != (bit_units ? (points + 1u) / 2 : points * 2u)) return status::BadCommunicationError;
    out = std::move(r.data);
    return status::Good;
  }

  StatusCode write_raw(const mc::Device& d, std::uint32_t number, std::uint16_t points, bool bit_units,
                       std::span<const std::uint8_t> payload) override {
    mc::Response r;
    StatusCode st = transact(mc::c_batch_write(frame_, d, base_of(d), number, points, bit_units, payload), bit_units, 0, r);
    if (!is_good(st)) return st;
    if (r.end_code != 0) {
      host_.log.warn("write to {}{} rejected (error code 0x{:04X})", d.name, number, r.end_code);
      return mc::end_code_status(r.end_code);
    }
    return status::Good;
  }

 private:
  StatusCode transact(const std::vector<std::uint8_t>& req, bool bit_units, std::uint16_t points, mc::Response& out) {
    auto& port = line_->port;
    if (!port.is_open()) return status::BadNotConnected;
    for (int attempt = 0; attempt < attempts_; ++attempt) {
      port.flush_input();
      StatusCode st = port.write_all(req, SteadyClock::now() + request_timeout_);
      if (!is_good(st)) return st;
      std::vector<std::uint8_t> frame;
      st = receive(frame, SteadyClock::now() + request_timeout_);
      if (is_good(st)) {
        auto r = mc::parse_c_response(frame_, frame, bit_units, points);
        if (r) {
          out = std::move(*r);
          return status::Good;
        }
        st = r.error().code;
        host_.log.debug("station {}: {}", frame_.station, r.error().message);
      }
      if (st != status::BadTimeout && st != status::BadCommunicationError) return st;
      host_.log.debug("station {}: {} ({} of {})", frame_.station, status_name(st), attempt + 1, attempts_);
    }
    return status::BadTimeout;
  }

  // One response: control code, then up to ETX (+ sum) for STX, or the fixed
  // header (+ error code) for ACK / NAK; CR LF in format 4.
  StatusCode receive(std::vector<std::uint8_t>& frame, Deadline deadline) {
    auto& port = line_->port;
    auto get = [&](std::size_t n) -> StatusCode {
      const std::size_t at = frame.size();
      frame.resize(at + n);
      return port.read_exact(std::span(frame).subspan(at), deadline);
    };
    std::uint8_t c = 0;
    do {  // skip noise before a control code
      if (StatusCode st = port.read_exact(std::span(&c, 1), deadline); !is_good(st)) return st;
    } while (c != 0x02 && c != 0x06 && c != 0x15);
    frame.push_back(c);
    const std::size_t hs = mc::c_header_size(frame_);
    StatusCode st = status::Good;
    if (c == 0x06) {
      st = get(hs);
    } else if (c == 0x15) {
      st = get(hs + 4);
    } else {
      while (is_good(st)) {
        if (frame.size() > 1 + hs + 4 * mc::kMaxWordsC + mc::kMaxBitsC) return status::BadCommunicationError;
        st = get(1);
        if (is_good(st) && frame.back() == 0x03) break;
      }
      if (is_good(st) && frame_.sum_check) st = get(2);
    }
    if (is_good(st) && frame_.format4) st = get(2);
    return st;
  }

  std::shared_ptr<Line> line_;
  mc::CFrame frame_;
  std::chrono::milliseconds request_timeout_{1000};
  int attempts_ = 3;
  bool online_ = false;
};

nlohmann::json prop(const char* key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

class MitsubishiSerialDriver final : public Driver {
 public:
  DriverInfo info() const override {
    return {"mitsubishi_serial", "Mitsubishi Serial", "0.1.0", Medium::Serial, {"Q Series", "L Series", "iQ-R"}};
  }

  nlohmann::json schema() const override {
    using nlohmann::json;
    return {
        {"driver", "mitsubishi_serial"},
        {"display_name", "Mitsubishi Serial"},
        {"channel",
         json::array({
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_PORT", "string", "/dev/ttyUSB0", "Serial device"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_BAUD_RATE", "integer", 9600, "Baud rate"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_DATA_BITS", "integer", 8, "Data bits (7 or 8)"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_PARITY", "enum", "None", "None, Even, Odd"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_STOP_BITS", "integer", 1, "Stop bits (1 or 2)"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_RS485", "boolean", false, "Kernel RS-485 mode (RTS on send)"),
         })},
        {"device",
         json::array({
             prop("servermain.DEVICE_ID_STRING", "string", "0", "Station number of the C24 module (0-31)"),
             prop(kFrame, "enum", "3C", "3C or 4C frame"),
             prop(kFormat, "integer", 4, "Format 1 or 4 (4 = CR LF after each message)"),
             prop(kSum, "boolean", true, "Sum check code"),
             prop(kNetwork, "integer", 0, "Network number (0 = own network)"),
             prop(kPc, "integer", 255, "PC number (255 = connected station)"),
             prop(kXyOctal, "boolean", false, "X/Y numbered in octal"),
             prop(kWordGap, "integer", 16, "Unused words allowed between merged tags"),
             prop(kBitGap, "integer", 64, "Unused bit points allowed between merged tags"),
         })},
        {"address_help", "Same as Mitsubishi Ethernet: D100, D100.A, M10, X1F, TN5, ZR100, D110.6 (String), M0 [16]."},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig& channel, const DeviceConfig& device,
                                                       HostContext& host) override {
    unsigned station = 0;
    auto r = std::from_chars(device.id.data(), device.id.data() + device.id.size(), station);
    if (device.id.empty() || r.ec != std::errc{} || r.ptr != device.id.data() + device.id.size() || station > 31)
      return make_error(status::BadConfigurationError, "device ID must be a station number 0-31");
    const auto format = property(device.properties, kFormat, 4);
    if (format != 1 && format != 4) return make_error(status::BadConfigurationError, "format must be 1 or 4");
    const auto frame = property<std::string>(device.properties, kFrame, "3C");
    if (frame != "3C" && frame != "4C") return make_error(status::BadConfigurationError, "frame must be 3C or 4C");
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
    return std::make_unique<McSerialDevice>(line, static_cast<std::uint8_t>(station), device, host);
  }

 private:
  std::mutex mu_;
  std::map<std::string, std::weak_ptr<Line>> lines_;
};

}  // namespace

LOGICT_DECLARE_DRIVER(MitsubishiSerialDriver)
