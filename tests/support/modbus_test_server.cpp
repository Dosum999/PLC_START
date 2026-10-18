#include "modbus_test_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <stdexcept>

namespace logict::test {

namespace {
std::uint16_t g16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
void p16(std::vector<std::uint8_t>& o, std::uint16_t v) {
  o.push_back(static_cast<std::uint8_t>(v >> 8));
  o.push_back(static_cast<std::uint8_t>(v));
}
bool read_exact(int fd, std::uint8_t* buf, std::size_t n, const std::atomic<bool>& stop) {
  std::size_t got = 0;
  while (got < n && !stop) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 50) <= 0) continue;
    ssize_t r = ::recv(fd, buf + got, n - got, 0);
    if (r <= 0) return false;
    got += static_cast<std::size_t>(r);
  }
  return got == n;
}
}  // namespace

ModbusTestServer::ModbusTestServer() : holding_(65536), input_(65536), coils_(65536), discrete_(65536) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_fd_, 4) != 0)
    throw std::runtime_error("test server bind failed");
  socklen_t len = sizeof(addr);
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);
  thread_ = std::thread([this] { run(); });
}

ModbusTestServer::~ModbusTestServer() {
  stop_ = true;
  thread_.join();
  ::close(listen_fd_);
}

void ModbusTestServer::set_holding(std::uint16_t a, std::uint16_t v) { std::lock_guard l(mu_); holding_[a] = v; }
std::uint16_t ModbusTestServer::holding(std::uint16_t a) { std::lock_guard l(mu_); return holding_[a]; }
void ModbusTestServer::set_input(std::uint16_t a, std::uint16_t v) { std::lock_guard l(mu_); input_[a] = v; }
void ModbusTestServer::set_coil(std::uint16_t a, bool v) { std::lock_guard l(mu_); coils_[a] = v; }
bool ModbusTestServer::coil(std::uint16_t a) { std::lock_guard l(mu_); return coils_[a]; }
void ModbusTestServer::set_discrete(std::uint16_t a, bool v) { std::lock_guard l(mu_); discrete_[a] = v; }
void ModbusTestServer::add_illegal(std::uint16_t a) { std::lock_guard l(mu_); illegal_.insert(a); }

int ModbusTestServer::requests(std::uint8_t fc) {
  std::lock_guard l(mu_);
  int n = 0;
  for (auto& p : log_) n += !p.empty() && p[0] == fc;
  return n;
}
std::vector<std::vector<std::uint8_t>> ModbusTestServer::pdus() { std::lock_guard l(mu_); return log_; }
void ModbusTestServer::reset_counters() { std::lock_guard l(mu_); log_.clear(); }
void ModbusTestServer::drop_client() {
  int fd = client_fd_.exchange(-1);
  if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
}

void ModbusTestServer::run() {
  while (!stop_) {
    pollfd p{listen_fd_, POLLIN, 0};
    if (::poll(&p, 1, 50) <= 0) continue;
    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;
    client_fd_ = fd;
    while (!stop_) {
      std::uint8_t hdr[7];
      if (!read_exact(fd, hdr, 7, stop_)) break;
      std::uint16_t len = g16(hdr + 4);
      std::vector<std::uint8_t> pdu(len - 1u);
      if (!read_exact(fd, pdu.data(), pdu.size(), stop_)) break;
      if (delay_ms_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_.load()));
      auto resp = handle(pdu);
      std::vector<std::uint8_t> adu(hdr, hdr + 4);
      p16(adu, static_cast<std::uint16_t>(resp.size() + 1));
      adu.push_back(hdr[6]);
      adu.insert(adu.end(), resp.begin(), resp.end());
      if (::send(fd, adu.data(), adu.size(), MSG_NOSIGNAL) < 0) break;
    }
    client_fd_ = -1;
    ::close(fd);
  }
}

std::vector<std::uint8_t> ModbusTestServer::handle(const std::vector<std::uint8_t>& pdu) {
  std::lock_guard l(mu_);
  log_.push_back(pdu);
  const std::uint8_t fc = pdu[0];
  auto exc = [&](std::uint8_t code) { return std::vector<std::uint8_t>{static_cast<std::uint8_t>(fc | 0x80), code}; };
  auto illegal = [&](std::uint32_t start, std::uint32_t count) {
    for (std::uint32_t a = start; a < start + count; ++a)
      if (illegal_.contains(static_cast<std::uint16_t>(a)) || a > 65535) return true;
    return false;
  };
  const std::uint16_t start = g16(&pdu[1]);
  switch (fc) {
    case 0x01:
    case 0x02: {
      std::uint16_t n = g16(&pdu[3]);
      if (illegal(start, n)) return exc(2);
      auto& src = fc == 1 ? coils_ : discrete_;
      std::vector<std::uint8_t> r{fc, static_cast<std::uint8_t>((n + 7) / 8)};
      r.resize(2 + (n + 7) / 8, 0);
      for (std::uint16_t i = 0; i < n; ++i)
        if (src[start + i]) r[2 + i / 8] |= static_cast<std::uint8_t>(1 << (i % 8));
      return r;
    }
    case 0x03:
    case 0x04: {
      std::uint16_t n = g16(&pdu[3]);
      if (n > 125) return exc(3);
      if (illegal(start, n)) return exc(2);
      auto& src = fc == 3 ? holding_ : input_;
      std::vector<std::uint8_t> r{fc, static_cast<std::uint8_t>(n * 2)};
      for (std::uint16_t i = 0; i < n; ++i) p16(r, src[start + i]);
      return r;
    }
    case 0x05:
      if (illegal(start, 1)) return exc(2);
      coils_[start] = g16(&pdu[3]) == 0xFF00;
      return pdu;
    case 0x06:
      if (illegal(start, 1)) return exc(2);
      holding_[start] = g16(&pdu[3]);
      return pdu;
    case 0x0F: {
      std::uint16_t n = g16(&pdu[3]);
      if (illegal(start, n)) return exc(2);
      for (std::uint16_t i = 0; i < n; ++i) coils_[start + i] = (pdu[6 + i / 8] >> (i % 8)) & 1;
      return std::vector<std::uint8_t>(pdu.begin(), pdu.begin() + 5);
    }
    case 0x10: {
      std::uint16_t n = g16(&pdu[3]);
      if (illegal(start, n)) return exc(2);
      for (std::uint16_t i = 0; i < n; ++i) holding_[start + i] = g16(&pdu[6 + 2 * i]);
      return std::vector<std::uint8_t>(pdu.begin(), pdu.begin() + 5);
    }
    case 0x16: {
      if (illegal(start, 1)) return exc(2);
      std::uint16_t andm = g16(&pdu[3]), orm = g16(&pdu[5]);
      holding_[start] = static_cast<std::uint16_t>((holding_[start] & andm) | (orm & ~andm));
      return pdu;
    }
    default:
      return exc(1);
  }
}

}  // namespace logict::test
