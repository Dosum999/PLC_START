#include "logict/sdk/serial.h"

#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <thread>

namespace logict::sdk {

namespace {

int remaining_ms(Deadline d) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(d - SteadyClock::now()).count();
  return ms < 0 ? 0 : static_cast<int>(std::min<long long>(ms, 1 << 30));
}

std::optional<speed_t> baud_constant(std::uint32_t baud) {
  switch (baud) {
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
  }
  return std::nullopt;
}

}  // namespace

Result<SerialSettings> serial_settings(const nlohmann::json& p, SerialSettings s) {
  constexpr const char* k = "servermain.CHANNEL_SERIAL_COMMUNICATIONS_";
  auto key = [&](const char* name) { return std::string(k) + name; };
  s.port = property<std::string>(p, key("PORT"), s.port);
  s.baud = property<std::uint32_t>(p, key("BAUD_RATE"), s.baud);
  s.data_bits = property(p, key("DATA_BITS"), s.data_bits);
  s.stop_bits = property(p, key("STOP_BITS"), s.stop_bits);
  s.rs485 = property(p, key("RS485"), s.rs485);
  std::string parity = property<std::string>(p, key("PARITY"), std::string(1, s.parity));
  if (!parity.empty()) s.parity = static_cast<char>(std::toupper(static_cast<unsigned char>(parity[0])));
  if (!baud_constant(s.baud)) return make_error(status::BadConfigurationError, std::format("unsupported baud rate {}", s.baud));
  if (s.data_bits != 7 && s.data_bits != 8) return make_error(status::BadConfigurationError, "data bits must be 7 or 8");
  if (s.stop_bits != 1 && s.stop_bits != 2) return make_error(status::BadConfigurationError, "stop bits must be 1 or 2");
  if (s.parity != 'N' && s.parity != 'E' && s.parity != 'O')
    return make_error(status::BadConfigurationError, "parity must be None, Even or Odd");
  if (s.port.empty()) return make_error(status::BadConfigurationError, "serial port is empty");
  return s;
}

SerialPort::~SerialPort() { close(); }

void SerialPort::close() noexcept {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

StatusCode SerialPort::open(const SerialSettings& s) {
  close();
  settings_ = s;
  fd_ = ::open(s.port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    last_error_ = std::format("{}: {}", s.port, std::strerror(errno));
    return status::BadNotConnected;
  }
  termios t{};
  if (::tcgetattr(fd_, &t) != 0) {
    last_error_ = std::format("{}: not a serial port ({})", s.port, std::strerror(errno));
    close();
    return status::BadNotConnected;
  }
  ::cfmakeraw(&t);
  t.c_cflag |= CLOCAL | CREAD;
  t.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
  t.c_cflag |= s.data_bits == 7 ? CS7 : CS8;
  if (s.parity != 'N') t.c_cflag |= PARENB | (s.parity == 'O' ? PARODD : 0);
  if (s.stop_bits == 2) t.c_cflag |= CSTOPB;
  t.c_iflag &= ~(IXON | IXOFF | IXANY);
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = 0;
  const speed_t speed = *baud_constant(s.baud);
  ::cfsetispeed(&t, speed);
  ::cfsetospeed(&t, speed);
  if (::tcsetattr(fd_, TCSANOW, &t) != 0) {
    last_error_ = std::format("{}: cannot configure ({})", s.port, std::strerror(errno));
    close();
    return status::BadNotConnected;
  }
  if (s.rs485) {
    serial_rs485 rs{};
    rs.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
    if (::ioctl(fd_, TIOCSRS485, &rs) != 0) last_error_ = std::format("RS-485 mode not supported: {}", std::strerror(errno));
  }
  const int bits = 1 + s.data_bits + (s.parity != 'N' ? 1 : 0) + s.stop_bits;
  char_time_ = std::chrono::microseconds(1'000'000LL * bits / s.baud);
  ::tcflush(fd_, TCIOFLUSH);
  return status::Good;
}

StatusCode SerialPort::write_all(std::span<const std::uint8_t> data, Deadline deadline) {
  if (fd_ < 0) return status::BadNotConnected;
  if (observer_) observer_->on_tx(data);
  std::size_t done = 0;
  while (done < data.size()) {
    ssize_t n = ::write(fd_, data.data() + done, data.size() - done);
    if (n > 0) {
      done += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR) {
      last_error_ = std::strerror(errno);
      close();
      return status::BadNotConnected;
    }
    pollfd p{fd_, POLLOUT, 0};
    if (::poll(&p, 1, remaining_ms(deadline)) == 0) return status::BadTimeout;
  }
  ::tcdrain(fd_);  // wait until sent so the response timeout starts after transmission
  return status::Good;
}

StatusCode SerialPort::read_exact(std::span<std::uint8_t> data, Deadline deadline) {
  if (fd_ < 0) return status::BadNotConnected;
  std::size_t done = 0;
  while (done < data.size()) {
    ssize_t n = ::read(fd_, data.data() + done, data.size() - done);
    if (n > 0) {
      done += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR) {
      last_error_ = std::strerror(errno);
      close();
      return status::BadNotConnected;
    }
    pollfd p{fd_, POLLIN, 0};
    int r = ::poll(&p, 1, remaining_ms(deadline));
    if (r == 0) return status::BadTimeout;
    if (r > 0 && (p.revents & (POLLERR | POLLNVAL))) {
      close();
      return status::BadNotConnected;
    }
  }
  if (observer_) observer_->on_rx(data);
  return status::Good;
}

void SerialPort::flush_input() noexcept {
  if (fd_ >= 0) ::tcflush(fd_, TCIFLUSH);
}

void SerialPort::wait_silence(std::chrono::microseconds gap, Deadline deadline) noexcept {
  if (fd_ < 0) return;
  std::uint8_t buf[256];
  while (SteadyClock::now() < deadline) {
    pollfd p{fd_, POLLIN, 0};
    const int ms = static_cast<int>(std::max<long long>(1, (gap.count() + 999) / 1000));
    if (::poll(&p, 1, ms) == 0) return;  // silent long enough
    if (::read(fd_, buf, sizeof(buf)) <= 0) return;  // discard late bytes
  }
}

}  // namespace logict::sdk
