#include "logict/sdk/tcp.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>

namespace logict::sdk {

namespace {
int remaining_ms(Deadline deadline) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - SteadyClock::now()).count();
  return ms < 0 ? 0 : static_cast<int>(std::min<long long>(ms, 1 << 30));
}
}  // namespace

TcpClient::~TcpClient() { close(); }

void TcpClient::close() noexcept {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

StatusCode TcpClient::wait(short events, Deadline deadline) {
  pollfd p{fd_, events, 0};
  for (;;) {
    int r = ::poll(&p, 1, remaining_ms(deadline));
    if (r > 0) {
      if (p.revents & (POLLERR | POLLNVAL)) return status::BadCommunicationError;
      return status::Good;  // POLLHUP is reported through read() == 0
    }
    if (r == 0) return status::BadTimeout;
    if (errno != EINTR) {
      last_error_ = std::strerror(errno);
      return status::BadCommunicationError;
    }
  }
}

StatusCode TcpClient::connect(const std::string& host, std::uint16_t port, Deadline deadline) {
  close();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string service = std::to_string(port);
  if (int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res); rc != 0) {
    last_error_ = std::format("cannot resolve '{}': {}", host, gai_strerror(rc));
    return status::BadNotConnected;
  }
  StatusCode st = status::BadNotConnected;
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    fd_ = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai->ai_protocol);
    if (fd_ < 0) continue;
    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    if (::connect(fd_, ai->ai_addr, ai->ai_addrlen) == 0) {
      st = status::Good;
      break;
    }
    if (errno == EINPROGRESS) {
      st = wait(POLLOUT, deadline);
      if (st == status::Good) {
        int err = 0;
        socklen_t len = sizeof(err);
        ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err == 0) break;
        last_error_ = std::strerror(err);
        st = status::BadNotConnected;
      } else if (st == status::BadTimeout) {
        last_error_ = "connect timed out";
      }
    } else {
      last_error_ = std::strerror(errno);
    }
    close();
    if (st == status::BadTimeout) break;
  }
  ::freeaddrinfo(res);
  if (st != status::Good) close();
  return st;
}

StatusCode TcpClient::send_all(std::span<const std::uint8_t> data, Deadline deadline) {
  if (fd_ < 0) return status::BadNotConnected;
  if (observer_) observer_->on_tx(data);
  std::size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      if (auto st = wait(POLLOUT, deadline); st != status::Good) return st;
      continue;
    }
    last_error_ = std::strerror(errno);
    close();
    return status::BadNotConnected;
  }
  return status::Good;
}

StatusCode TcpClient::recv_exact(std::span<std::uint8_t> data, Deadline deadline) {
  if (fd_ < 0) return status::BadNotConnected;
  std::size_t got = 0;
  while (got < data.size()) {
    ssize_t n = ::recv(fd_, data.data() + got, data.size() - got, 0);
    if (n > 0) {
      got += static_cast<std::size_t>(n);
      continue;
    }
    if (n == 0) {
      last_error_ = "connection closed by peer";
      close();
      return status::BadNotConnected;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      if (auto st = wait(POLLIN, deadline); st != status::Good) return st;
      continue;
    }
    last_error_ = std::strerror(errno);
    close();
    return status::BadNotConnected;
  }
  if (observer_) observer_->on_rx(data);
  return status::Good;
}

StatusCode TcpClient::recv_some(std::span<std::uint8_t> data, std::size_t& size, Deadline deadline) {
  if (fd_ < 0) return status::BadNotConnected;
  for (;;) {
    ssize_t n = ::recv(fd_, data.data(), data.size(), 0);
    if (n > 0) {
      size = static_cast<std::size_t>(n);
      if (observer_) observer_->on_rx(data.first(size));
      return status::Good;
    }
    if (n == 0) {
      last_error_ = "connection closed by peer";
      close();
      return status::BadNotConnected;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      if (auto st = wait(POLLIN, deadline); st != status::Good) return st;
      continue;
    }
    last_error_ = std::strerror(errno);
    close();
    return status::BadNotConnected;
  }
}

void TcpClient::drain() noexcept {
  if (fd_ < 0) return;
  std::uint8_t buf[512];
  while (::recv(fd_, buf, sizeof(buf), MSG_DONTWAIT) > 0) {
  }
}

}  // namespace logict::sdk
