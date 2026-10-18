#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "logict/sdk/driver.h"
#include "logict/sdk/status.h"
#include "logict/sdk/types.h"

namespace logict::sdk {

struct SerialSettings {
  std::string port = "/dev/ttyUSB0";
  std::uint32_t baud = 9600;
  int data_bits = 8;     // 7 or 8
  char parity = 'N';     // 'N', 'E', 'O'
  int stop_bits = 1;     // 1 or 2
  bool rs485 = false;    // let the kernel drive RTS for RS-485 direction (TIOCSRS485)
};

// Parses Kepware-style channel serial properties:
//   servermain.CHANNEL_SERIAL_COMMUNICATIONS_PORT      (path, e.g. /dev/ttyUSB0)
//   servermain.CHANNEL_SERIAL_COMMUNICATIONS_BAUD_RATE
//   servermain.CHANNEL_SERIAL_COMMUNICATIONS_DATA_BITS
//   servermain.CHANNEL_SERIAL_COMMUNICATIONS_PARITY    ("None", "Even", "Odd")
//   servermain.CHANNEL_SERIAL_COMMUNICATIONS_STOP_BITS
//   servermain.CHANNEL_SERIAL_COMMUNICATIONS_RS485
Result<SerialSettings> serial_settings(const nlohmann::json& channel_properties, SerialSettings defaults = {});

// Blocking serial port with deadlines (raw mode, no flow control), for drivers
// running on a channel thread. Status codes as for TcpClient.
class SerialPort {
 public:
  SerialPort() = default;
  ~SerialPort();
  SerialPort(const SerialPort&) = delete;
  SerialPort& operator=(const SerialPort&) = delete;

  StatusCode open(const SerialSettings& s);
  void close() noexcept;
  bool is_open() const noexcept { return fd_ >= 0; }

  StatusCode write_all(std::span<const std::uint8_t> data, Deadline deadline);
  // Reads exactly data.size() bytes.
  StatusCode read_exact(std::span<std::uint8_t> data, Deadline deadline);
  // Discards pending input (stale or partial frames).
  void flush_input() noexcept;
  // Waits until the line has been silent for `gap`, or the deadline passes.
  void wait_silence(std::chrono::microseconds gap, Deadline deadline) noexcept;

  // Seconds per character at the current settings (start + data + parity + stop bits).
  std::chrono::microseconds char_time() const noexcept { return char_time_; }
  const SerialSettings& settings() const noexcept { return settings_; }
  const std::string& last_error() const noexcept { return last_error_; }
  void set_observer(FrameObserver* obs) noexcept { observer_ = obs; }

 private:
  int fd_ = -1;
  SerialSettings settings_;
  std::chrono::microseconds char_time_{1042};
  FrameObserver* observer_ = nullptr;
  std::string last_error_;
};

}  // namespace logict::sdk
