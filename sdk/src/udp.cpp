#include "logict/sdk/udp.h"

#include <netdb.h>
#include <netinet/in.h>
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

std::uint8_t last_octet(const sockaddr_storage& ss) {
  if (ss.ss_family != AF_INET) return 0;
  const auto& in = reinterpret_cast<const sockaddr_in&>(ss);
  return static_cast<std::uint8_t>(ntohl(in.sin_addr.s_addr) & 0xFF);
}
}  // namespace

UdpClient::~UdpClient() { close(); }

void UdpClient::close() noexcept {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

StatusCode UdpClient::open(const std::string& host, std::uint16_t port, std::uint16_t local_port) {
  close();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
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
    if (local_port != 0) {
      int one = 1;
      ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
      sockaddr_storage local{};
      local.ss_family = static_cast<sa_family_t>(ai->ai_family);
      if (ai->ai_family == AF_INET) reinterpret_cast<sockaddr_in&>(local).sin_port = htons(local_port);
      else reinterpret_cast<sockaddr_in6&>(local).sin6_port = htons(local_port);
      if (::bind(fd_, reinterpret_cast<sockaddr*>(&local), static_cast<socklen_t>(ai->ai_addrlen)) != 0) {
        last_error_ = std::format("cannot bind UDP port {}: {}", local_port, std::strerror(errno));
        close();
        continue;
      }
    }
    if (::connect(fd_, ai->ai_addr, ai->ai_addrlen) == 0) {
      st = status::Good;
      break;
    }
    last_error_ = std::strerror(errno);
    close();
  }
  ::freeaddrinfo(res);
  return st;
}

StatusCode UdpClient::send(std::span<const std::uint8_t> datagram) {
  if (fd_ < 0) return status::BadNotConnected;
  if (observer_) observer_->on_tx(datagram);
  for (;;) {
    ssize_t n = ::send(fd_, datagram.data(), datagram.size(), MSG_NOSIGNAL);
    if (n == static_cast<ssize_t>(datagram.size())) return status::Good;
    if (n < 0 && errno == EINTR) continue;
    last_error_ = n < 0 ? std::strerror(errno) : "short datagram send";
    return status::BadCommunicationError;
  }
}

StatusCode UdpClient::recv(std::span<std::uint8_t> buf, std::size_t& size, Deadline deadline) {
  if (fd_ < 0) return status::BadNotConnected;
  for (;;) {
    ssize_t n = ::recv(fd_, buf.data(), buf.size(), MSG_TRUNC);
    if (n >= 0) {
      size = std::min(static_cast<std::size_t>(n), buf.size());
      if (observer_) observer_->on_rx(buf.first(size));
      return status::Good;
    }
    if (errno == EINTR) continue;
    if (errno == ECONNREFUSED) {
      // ICMP port unreachable from an earlier send: nobody listens there.
      last_error_ = "port unreachable";
      return status::BadNotConnected;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      last_error_ = std::strerror(errno);
      return status::BadCommunicationError;
    }
    pollfd p{fd_, POLLIN, 0};
    int r = ::poll(&p, 1, remaining_ms(deadline));
    if (r == 0) return status::BadTimeout;
    if (r < 0 && errno != EINTR) {
      last_error_ = std::strerror(errno);
      return status::BadCommunicationError;
    }
  }
}

void UdpClient::drain() noexcept {
  if (fd_ < 0) return;
  std::uint8_t buf[2048];
  while (::recv(fd_, buf, sizeof(buf), MSG_DONTWAIT) >= 0) {
  }
}

std::uint8_t UdpClient::local_ipv4_last_octet() const noexcept {
  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (fd_ < 0 || ::getsockname(fd_, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return 0;
  return last_octet(ss);
}

std::uint8_t UdpClient::peer_ipv4_last_octet() const noexcept {
  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (fd_ < 0 || ::getpeername(fd_, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return 0;
  return last_octet(ss);
}

}  // namespace logict::sdk
