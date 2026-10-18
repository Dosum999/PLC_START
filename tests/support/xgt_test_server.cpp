#include "xgt_test_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <charconv>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace logict::test {

namespace {

constexpr std::size_t kAreaSize = 65536;

std::uint16_t g16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
void p16(std::vector<std::uint8_t>& o, std::uint16_t v) {
  o.push_back(static_cast<std::uint8_t>(v));
  o.push_back(static_cast<std::uint8_t>(v >> 8));
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

struct Var {
  std::string area;
  char size;
  std::string rest;  // number or dotted address
  std::optional<std::uint32_t> index;
};

std::optional<Var> parse_var(const std::string& v) {
  if (v.size() < 4 || v[0] != '%') return std::nullopt;
  Var out;
  std::size_t pos = 1;
  out.area = v.compare(1, 2, "ZR") == 0 ? "ZR" : v.substr(1, 1);
  pos += out.area.size();
  out.size = v[pos++];
  if (std::string("XBWDL").find(out.size) == std::string::npos) return std::nullopt;
  out.rest = v.substr(pos);
  std::uint32_t n;
  auto r = std::from_chars(out.rest.data(), out.rest.data() + out.rest.size(), n);
  if (r.ec == std::errc{} && r.ptr == out.rest.data() + out.rest.size()) out.index = n;
  else if (out.rest.find('.') == std::string::npos) return std::nullopt;
  return out;
}

std::size_t unit(char size) {
  switch (size) {
    case 'X':
    case 'B': return 1;
    case 'W': return 2;
    case 'D': return 4;
    case 'L': return 8;
  }
  return 0;
}

std::uint16_t type_of(char size) {
  switch (size) {
    case 'X': return 0;
    case 'B': return 1;
    case 'W': return 2;
    case 'D': return 3;
    case 'L': return 4;
  }
  return 0xFF;
}

}  // namespace

XgtTestServer::XgtTestServer() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_fd_, 4) != 0)
    throw std::runtime_error("xgt test server bind failed");
  socklen_t len = sizeof(addr);
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);
  thread_ = std::thread([this] { run(); });
}

XgtTestServer::~XgtTestServer() {
  stop_ = true;
  thread_.join();
  ::close(listen_fd_);
}

void XgtTestServer::set_word(const std::string& area, std::uint32_t w, std::uint16_t v) {
  set_bytes(area, w * 2, {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8)});
}
std::uint16_t XgtTestServer::word(const std::string& area, std::uint32_t w) {
  auto b = bytes(area, w * 2, 2);
  return static_cast<std::uint16_t>(b[0] | (b[1] << 8));
}
void XgtTestServer::set_bytes(const std::string& area, std::uint32_t off, const std::vector<std::uint8_t>& b) {
  std::lock_guard l(mu_);
  auto& m = mem_[area];
  m.resize(kAreaSize);
  std::copy(b.begin(), b.end(), m.begin() + off);
}
std::vector<std::uint8_t> XgtTestServer::bytes(const std::string& area, std::uint32_t off, std::size_t n) {
  std::lock_guard l(mu_);
  auto& m = mem_[area];
  m.resize(kAreaSize);
  return {m.begin() + off, m.begin() + off + static_cast<std::ptrdiff_t>(n)};
}
bool XgtTestServer::bit(const std::string& area, std::uint32_t i) { return (bytes(area, i / 8, 1)[0] >> (i % 8)) & 1; }
void XgtTestServer::set_contact(const std::string& var, bool v) { std::lock_guard l(mu_); contacts_[var] = v; }
bool XgtTestServer::contact(const std::string& var) { std::lock_guard l(mu_); return contacts_[var]; }
void XgtTestServer::set_var(const std::string& var, std::vector<std::uint8_t> d) { std::lock_guard l(mu_); vars_[var] = std::move(d); }
void XgtTestServer::reject_continuous(const std::string& area) { std::lock_guard l(mu_); reject_continuous_.insert(area); }
void XgtTestServer::add_illegal(const std::string& area, std::uint32_t off) { std::lock_guard l(mu_); illegal_.insert({area, off}); }
std::vector<XgtTestServer::Request> XgtTestServer::requests() { std::lock_guard l(mu_); return log_; }
void XgtTestServer::reset() { std::lock_guard l(mu_); log_.clear(); }

void XgtTestServer::run() {
  while (!stop_) {
    pollfd p{listen_fd_, POLLIN, 0};
    if (::poll(&p, 1, 50) <= 0) continue;
    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;
    while (!stop_) {
      std::uint8_t hdr[20];
      if (!read_exact(fd, hdr, 20, stop_)) break;
      std::uint8_t bcc = 0;
      for (int i = 0; i < 19; ++i) bcc = static_cast<std::uint8_t>(bcc + hdr[i]);
      if (std::memcmp(hdr, "LSIS-XGT\0\0", 10) != 0 || hdr[13] != 0x33 || hdr[19] != bcc) {
        ++framing_errors_;
        break;
      }
      std::vector<std::uint8_t> body(g16(hdr + 16));
      if (!read_exact(fd, body.data(), body.size(), stop_)) break;
      if (delay_ms_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_.load()));
      auto resp = handle(hdr, body);
      std::vector<std::uint8_t> out{'L', 'S', 'I', 'S', '-', 'X', 'G', 'T', 0, 0};
      p16(out, 0x0105);  // PLC info: XGI-CPUU, RUN
      out.push_back(hdr[12]);
      out.push_back(0x11);
      out.push_back(hdr[14]);
      out.push_back(hdr[15]);
      p16(out, static_cast<std::uint16_t>(resp.size()));
      out.push_back(hdr[18]);
      std::uint8_t sum = 0;
      for (auto b : out) sum = static_cast<std::uint8_t>(sum + b);
      out.push_back(sum);
      out.insert(out.end(), resp.begin(), resp.end());
      if (::send(fd, out.data(), out.size(), MSG_NOSIGNAL) < 0) break;
    }
    ::close(fd);
  }
}

std::vector<std::uint8_t> XgtTestServer::handle(const std::uint8_t* hdr, const std::vector<std::uint8_t>& b) {
  std::lock_guard l(mu_);
  const std::uint16_t cmd = g16(&b[0]), type = g16(&b[2]), n = g16(&b[6]);
  Request req{cmd, type, n, {}, hdr[12]};
  std::vector<std::uint8_t> resp;
  p16(resp, static_cast<std::uint16_t>(cmd + 1));
  p16(resp, type);
  p16(resp, 0);
  auto nak = [&](std::uint8_t code) {
    log_.push_back(req);
    std::vector<std::uint8_t> r(resp.begin(), resp.end());
    p16(r, 0xFFFF);
    r.push_back(code);
    r.push_back(0);
    return r;
  };
  if ((cmd != 0x54 && cmd != 0x58) || n == 0 || n > 16) return nak(0x01);
  std::size_t pos = 8;
  for (std::uint16_t i = 0; i < n; ++i) {
    const std::uint16_t len = g16(&b[pos]);
    req.vars.emplace_back(reinterpret_cast<const char*>(&b[pos + 2]), len);
    pos += 2 + len;
  }
  auto illegal = [&](const std::string& area, std::size_t off, std::size_t len) {
    for (std::size_t o = off; o < off + len; ++o)
      if (illegal_.contains({area, static_cast<std::uint32_t>(o)})) return true;
    return off + len > kAreaSize;
  };

  if (type == 0x14) {  // continuous
    auto v = parse_var(req.vars[0]);
    if (n != 1 || !v || v->size != 'B' || !v->index) return nak(0x11);
    if (reject_continuous_.contains(v->area)) return nak(0x12);
    const std::uint16_t count = g16(&b[pos]);
    if (count == 0 || count > 1400 || illegal(v->area, *v->index, count)) return nak(0x13);
    auto& m = mem_[v->area];
    m.resize(kAreaSize);
    p16(resp, 0);
    p16(resp, 1);
    if (cmd == 0x54) {
      p16(resp, count);
      resp.insert(resp.end(), m.begin() + *v->index, m.begin() + *v->index + count);
    } else {
      std::copy(b.begin() + static_cast<std::ptrdiff_t>(pos + 2), b.begin() + static_cast<std::ptrdiff_t>(pos + 2 + count),
                m.begin() + *v->index);
    }
    log_.push_back(req);
    return resp;
  }

  // Individual: all variables must match the request data type.
  std::vector<std::vector<std::uint8_t>> data;
  for (const auto& name : req.vars) {
    auto v = parse_var(name);
    if (!v || type_of(v->size) != type) return nak(0x14);
    const std::size_t u = unit(v->size);
    if (!v->index) {  // dotted I/Q/U variable
      auto it = vars_.find(name);
      if (it == vars_.end()) return nak(0x15);
      data.push_back(it->second);
      continue;
    }
    if (v->size == 'X' && (v->area == "T" || v->area == "C")) {
      data.push_back({static_cast<std::uint8_t>(contacts_[name] ? 1 : 0)});
      continue;
    }
    const std::size_t off = v->size == 'X' ? *v->index / 8 : *v->index * u;
    if (illegal(v->area, off, u)) return nak(0x16);
    auto& m = mem_[v->area];
    m.resize(kAreaSize);
    if (v->size == 'X') data.push_back({static_cast<std::uint8_t>((m[off] >> (*v->index % 8)) & 1)});
    else data.emplace_back(m.begin() + static_cast<std::ptrdiff_t>(off), m.begin() + static_cast<std::ptrdiff_t>(off + u));
  }
  if (cmd == 0x58) {
    for (std::size_t i = 0; i < req.vars.size(); ++i) {
      const std::uint16_t len = g16(&b[pos]);
      std::vector<std::uint8_t> d(b.begin() + static_cast<std::ptrdiff_t>(pos + 2), b.begin() + static_cast<std::ptrdiff_t>(pos + 2 + len));
      pos += 2 + len;
      auto v = *parse_var(req.vars[i]);
      if (d.size() != unit(v.size)) return nak(0x17);
      if (!v.index) {
        vars_[req.vars[i]] = d;
      } else if (v.size == 'X' && (v.area == "T" || v.area == "C")) {
        contacts_[req.vars[i]] = d[0] & 1;
      } else if (v.size == 'X') {
        auto& m = mem_[v.area];
        auto& byte = m[*v.index / 8];
        byte = static_cast<std::uint8_t>((d[0] & 1) ? (byte | (1u << (*v.index % 8))) : (byte & ~(1u << (*v.index % 8))));
      } else {
        std::copy(d.begin(), d.end(), mem_[v.area].begin() + static_cast<std::ptrdiff_t>(*v.index * unit(v.size)));
      }
    }
    p16(resp, 0);
    p16(resp, n);
  } else {
    p16(resp, 0);
    p16(resp, n);
    for (auto& d : data) {
      p16(resp, static_cast<std::uint16_t>(d.size()));
      resp.insert(resp.end(), d.begin(), d.end());
    }
  }
  log_.push_back(req);
  return resp;
}

}  // namespace logict::test
