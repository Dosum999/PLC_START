// Modbus Server Serial driver (Kepware "Modbus Server Serial", formerly
// Modbus Unsolicited Serial): logict is the slave. The channel is the serial
// line; each device is a station (1-247) with its own data table. A master on
// the line reads and writes the tables, and the device's tags read and write
// the same tables, so values flow both ways without polling a device.
//
// A thread per line answers the master (RTU or ASCII framing). The tags use
// the Modbus TCP address syntax and go through the same request processor
// locally; their writes may also set discrete inputs (1xxxxx) and input
// registers (3xxxxx), which a master can only read.

#include <atomic>
#include <charconv>
#include <map>
#include <mutex>
#include <thread>

#include "logict/sdk/serial.h"
#include "modbus_device.h"
#include "server.h"

namespace {

using namespace logict;
using namespace logict::modbus;
namespace status = sdk::status;

constexpr const char* kId = "modbus_server_serial";

// One serial line and the data tables of its stations.
class ServerLine {
 public:
  ServerLine(sdk::SerialSettings settings, bool ascii) : settings_(std::move(settings)), ascii_(ascii) {}
  ~ServerLine() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  std::shared_ptr<DataTable> table(std::uint8_t station) {
    std::lock_guard lock(mu_);
    auto& t = tables_[station];
    if (!t) t = std::make_shared<DataTable>();
    return t;
  }

  std::vector<std::uint8_t> process(DataTable& t, std::span<const std::uint8_t> pdu, Redirect redirect) {
    std::lock_guard lock(mu_);
    return process_request(t, pdu, redirect);
  }

  // Opens the port and starts answering; called from the channel thread.
  sdk::StatusCode start(sdk::Logger& log) {
    if (thread_.joinable()) return status::Good;
    if (sdk::StatusCode st = port_.open(settings_); !sdk::is_good(st)) {
      log.warn("cannot open serial port: {}", port_.last_error());
      return st;
    }
    log.info("serving Modbus {} on {} ({} {}{}{})", ascii_ ? "ASCII" : "RTU", settings_.port, settings_.baud,
             settings_.data_bits, settings_.parity, settings_.stop_bits);
    thread_ = std::thread([this] { ascii_ ? run_ascii() : run_rtu(); });
    return status::Good;
  }
  bool running() const { return thread_.joinable(); }

 private:
  static constexpr auto kPoll = std::chrono::milliseconds(100);

  // Answers a request (address + PDU); empty if no answer is due.
  std::vector<std::uint8_t> answer(std::span<const std::uint8_t> frame) {
    if (frame.size() < 2) return {};
    const std::uint8_t station = frame[0];
    std::lock_guard lock(mu_);
    if (station == 0) {  // broadcast: writes to every station, no answer
      for (auto& [_, t] : tables_) process_request(*t, frame.subspan(1));
      return {};
    }
    auto it = tables_.find(station);
    if (it == tables_.end()) return {};  // another slave on the line
    std::vector<std::uint8_t> out{station};
    auto resp = process_request(*it->second, frame.subspan(1));
    out.insert(out.end(), resp.begin(), resp.end());
    return out;
  }

  void run_rtu() {
    const auto silence = std::max(port_.char_time() * 7 / 2, std::chrono::microseconds(1750));
    std::vector<std::uint8_t> frame;
    while (!stop_) {
      frame.assign(1, 0);
      if (!sdk::is_good(port_.read_exact(frame, sdk::SteadyClock::now() + kPoll))) continue;
      // The rest of a frame follows without gaps; allow generous slack for USB adapters.
      const sdk::Deadline deadline = sdk::SteadyClock::now() + port_.char_time() * 260 + std::chrono::milliseconds(50);
      auto read_to = [&](std::size_t n) {
        const std::size_t have = frame.size();
        frame.resize(n);
        return sdk::is_good(port_.read_exact(std::span(frame).subspan(have), deadline));
      };
      if (!read_to(2)) continue;
      const std::size_t head = request_head_size(frame[1]);
      std::size_t length = 0;
      if (head != 0 && read_to(head)) length = rtu_request_length(frame);
      if (length == 0 || length > 256 || !read_to(length) ||
          crc16(std::span(frame).first(length - 2)) != (frame[length - 2] | frame[length - 1] << 8)) {
        // Unknown function, a reply from another slave or noise: skip to the next gap.
        port_.wait_silence(silence, sdk::SteadyClock::now() + std::chrono::seconds(1));
        continue;
      }
      auto out = answer(std::span(frame).first(length - 2));
      if (out.empty()) continue;
      const std::uint16_t crc = crc16(out);
      out.push_back(static_cast<std::uint8_t>(crc));
      out.push_back(static_cast<std::uint8_t>(crc >> 8));
      port_.write_all(out, sdk::SteadyClock::now() + std::chrono::seconds(1));
    }
  }

  void run_ascii() {
    std::string text;
    bool started = false;
    while (!stop_) {
      std::uint8_t c;
      if (!sdk::is_good(port_.read_exact(std::span(&c, 1), sdk::SteadyClock::now() + kPoll))) continue;
      if (c == ':') {
        started = true;
        text.clear();
      } else if (!started) {
        continue;
      } else if (c == '\n') {
        started = false;
        auto frame = parse_ascii_frame(text);
        if (!frame) continue;  // bad LRC or characters
        auto out = answer(*frame);
        if (!out.empty()) port_.write_all(ascii_frame(out), sdk::SteadyClock::now() + std::chrono::seconds(1));
      } else if (c != '\r') {
        if (text.size() < 2 * 260) text.push_back(static_cast<char>(c));
        else started = false;
      }
    }
  }

  sdk::SerialSettings settings_;
  bool ascii_;
  sdk::SerialPort port_;  // used by the line thread only, once started
  std::mutex mu_;         // guards the tables
  std::map<std::uint8_t, std::shared_ptr<DataTable>> tables_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

class ModbusServerDevice final : public ModbusDevice {
 public:
  ModbusServerDevice(std::shared_ptr<ServerLine> line, std::uint8_t station, const sdk::DeviceConfig& cfg,
                     sdk::HostContext& host)
      : ModbusDevice(cfg, host, kId), line_(std::move(line)), table_(line_->table(station)) {}

  sdk::StatusCode connect(sdk::Deadline) override { return line_->start(host_.log); }
  void disconnect() override {}  // the line keeps serving the other stations
  bool connected() const override { return line_->running(); }

  sdk::StatusCode write(const sdk::Address& address, const sdk::Value& value) override {
    const auto& a = static_cast<const ModbusAddress&>(address);
    if (a.area != Area::DiscreteInput && a.area != Area::InputRegister) return ModbusDevice::write(address, value);
    // Write the input areas through the coil / holding register paths, redirected.
    ModbusAddress as_output = a;
    as_output.area = a.area == Area::DiscreteInput ? Area::Coil : Area::HoldingRegister;
    redirect_ = a.area == Area::DiscreteInput ? Redirect::DiscreteInputs : Redirect::InputRegisters;
    const sdk::StatusCode st = ModbusDevice::write(as_output, value);
    redirect_ = Redirect::None;
    return st;
  }

 protected:
  sdk::StatusCode transact(const std::vector<std::uint8_t>& pdu, std::vector<std::uint8_t>& resp) override {
    resp = line_->process(*table_, pdu, redirect_);
    return status::Good;
  }

 private:
  std::shared_ptr<ServerLine> line_;
  std::shared_ptr<DataTable> table_;
  Redirect redirect_ = Redirect::None;
};

nlohmann::json prop(const std::string& key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

class ModbusServerSerialDriver final : public sdk::Driver {
 public:
  sdk::DriverInfo info() const override {
    return {kId, "Modbus Server Serial", "0.1.0", sdk::Medium::Serial, {"Modbus"}};
  }

  nlohmann::json schema() const override {
    using nlohmann::json;
    json device = json::array({prop("servermain.DEVICE_ID_STRING", "string", "1", "Station address this server answers (1-247)")});
    for (auto& p : common_device_schema(kId)) device.push_back(std::move(p));
    return {
        {"driver", kId},
        {"display_name", "Modbus Server Serial"},
        {"channel",
         json::array({
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_PORT", "string", "/dev/ttyUSB0", "Serial device"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_BAUD_RATE", "integer", 9600, "Baud rate"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_DATA_BITS", "integer", 8, "Data bits (7 or 8)"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_PARITY", "enum", "None", "None, Even, Odd"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_STOP_BITS", "integer", 1, "Stop bits (1 or 2)"),
             prop("servermain.CHANNEL_SERIAL_COMMUNICATIONS_RS485", "boolean", false, "Kernel RS-485 mode (RTS on send)"),
             prop(std::string(kId) + ".CHANNEL_PROTOCOL", "enum", "RTU", "RTU or ASCII framing"),
         })},
        {"device", device},
        {"address_help",
         "Same as Modbus TCP: 0xxxxx coils, 1xxxxx discrete inputs, 3xxxxx input registers, 4xxxxx holding "
         "registers, .bit, .nH/.nL strings, arrays. Tags may write all four areas."},
    };
  }

  sdk::Result<std::unique_ptr<sdk::DeviceRuntime>> create_device(const sdk::ChannelConfig& channel,
                                                                 const sdk::DeviceConfig& device,
                                                                 sdk::HostContext& host) override {
    unsigned station = 0;
    auto r = std::from_chars(device.id.data(), device.id.data() + device.id.size(), station);
    if (device.id.empty() || r.ec != std::errc{} || r.ptr != device.id.data() + device.id.size() || station < 1 ||
        station > 247)
      return sdk::make_error(status::BadConfigurationError, "device ID must be a station address 1-247");
    auto settings = sdk::serial_settings(channel.properties);
    if (!settings) return std::unexpected(settings.error());
    const std::string protocol = sdk::property<std::string>(channel.properties, std::string(kId) + ".CHANNEL_PROTOCOL", "RTU");
    if (protocol != "RTU" && protocol != "ASCII")
      return sdk::make_error(status::BadConfigurationError, "protocol must be RTU or ASCII");

    std::lock_guard lock(mu_);
    auto& weak = lines_[channel.name];
    auto line = weak.lock();
    if (!line) {
      line = std::make_shared<ServerLine>(*settings, protocol == "ASCII");
      weak = line;
    }
    return std::make_unique<ModbusServerDevice>(line, static_cast<std::uint8_t>(station), device, host);
  }

 private:
  std::mutex mu_;
  std::map<std::string, std::weak_ptr<ServerLine>> lines_;
};

}  // namespace

LOGICT_DECLARE_DRIVER(ModbusServerSerialDriver)
