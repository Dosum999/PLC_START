#include "cnet_protocol.h"

#include <format>

namespace logict::xgt::cnet {

namespace status = sdk::status;

namespace {

void hex2(std::vector<std::uint8_t>& out, unsigned v) {
  static const char* digits = "0123456789ABCDEF";
  out.push_back(static_cast<std::uint8_t>(digits[(v >> 4) & 0xF]));
  out.push_back(static_cast<std::uint8_t>(digits[v & 0xF]));
}

void text(std::vector<std::uint8_t>& out, std::string_view s) { out.insert(out.end(), s.begin(), s.end()); }

std::vector<std::uint8_t> start(std::uint8_t station, char cmd, bool bcc, std::string_view type) {
  std::vector<std::uint8_t> f{ENQ};
  hex2(f, station);
  f.push_back(static_cast<std::uint8_t>(bcc ? cmd | 0x20 : cmd & ~0x20));  // lowercase = with BCC
  text(f, type);
  return f;
}

void finish(std::vector<std::uint8_t>& f, bool bcc) {
  f.push_back(EOT);
  if (bcc) hex2(f, bcc_of(f));
}

int hexval(std::uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Reads 2*n hex characters at pos as n bytes.
bool hex_bytes(std::span<const std::uint8_t> f, std::size_t& pos, std::size_t n, std::vector<std::uint8_t>& out) {
  if (pos + 2 * n > f.size()) return false;
  for (std::size_t i = 0; i < n; ++i) {
    int hi = hexval(f[pos + 2 * i]), lo = hexval(f[pos + 2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(static_cast<std::uint8_t>(hi << 4 | lo));
  }
  pos += 2 * n;
  return true;
}

}  // namespace

std::uint8_t bcc_of(std::span<const std::uint8_t> bytes) {
  unsigned sum = 0;
  for (auto b : bytes) sum += b;
  return static_cast<std::uint8_t>(sum);
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
  std::vector<std::uint8_t> out;
  for (auto b : bytes) hex2(out, b);
  return std::string(out.begin(), out.end());
}

std::vector<std::uint8_t> read_individual(std::uint8_t station, bool bcc, std::span<const std::string> vars) {
  auto f = start(station, 'R', bcc, "SS");
  hex2(f, static_cast<unsigned>(vars.size()));
  for (const auto& v : vars) {
    hex2(f, static_cast<unsigned>(v.size()));
    text(f, v);
  }
  finish(f, bcc);
  return f;
}

std::vector<std::uint8_t> read_continuous(std::uint8_t station, bool bcc, std::string_view var, unsigned count) {
  auto f = start(station, 'R', bcc, "SB");
  hex2(f, static_cast<unsigned>(var.size()));
  text(f, var);
  hex2(f, count);
  finish(f, bcc);
  return f;
}

std::vector<std::uint8_t> write_individual(std::uint8_t station, bool bcc, std::span<const std::string> vars,
                                           std::span<const std::string> data) {
  auto f = start(station, 'W', bcc, "SS");
  hex2(f, static_cast<unsigned>(vars.size()));
  for (std::size_t i = 0; i < vars.size(); ++i) {
    hex2(f, static_cast<unsigned>(vars[i].size()));
    text(f, vars[i]);
    text(f, data[i]);
  }
  finish(f, bcc);
  return f;
}

std::vector<std::uint8_t> write_continuous(std::uint8_t station, bool bcc, std::string_view var, unsigned count,
                                           std::string_view data) {
  auto f = start(station, 'W', bcc, "SB");
  hex2(f, static_cast<unsigned>(var.size()));
  text(f, var);
  hex2(f, count);
  text(f, data);
  finish(f, bcc);
  return f;
}

sdk::Result<Response> parse(std::span<const std::uint8_t> f, std::uint8_t station, bool bcc, bool is_read) {
  auto bad = [](std::string_view why) { return sdk::make_error(status::BadCommunicationError, std::string(why)); };
  if (f.size() < 7 || (f[0] != ACK && f[0] != NAK)) return bad("not an ACK/NAK frame");
  const std::size_t etx = bcc ? f.size() - 3 : f.size() - 1;
  if (f[etx] != ETX) return bad("missing ETX");
  if (bcc) {
    std::vector<std::uint8_t> got;
    std::size_t p = etx + 1;
    if (!hex_bytes(f, p, 1, got) || got[0] != bcc_of(f.first(etx + 1))) return bad("BCC mismatch");
  }
  std::size_t pos = 1;
  std::vector<std::uint8_t> st;
  if (!hex_bytes(f, pos, 1, st) || st[0] != station) return bad("wrong station");
  pos += 3;  // command + type
  Response r;
  r.ack = f[0] == ACK;
  if (!r.ack) {
    std::vector<std::uint8_t> code;
    if (!hex_bytes(f, pos, 2, code)) return bad("bad NAK code");
    r.error = static_cast<std::uint16_t>(code[0] << 8 | code[1]);
    return r;
  }
  if (!is_read) return r;
  std::vector<std::uint8_t> n;
  if (!hex_bytes(f, pos, 1, n)) return bad("bad block count");
  for (unsigned b = 0; b < n[0]; ++b) {
    std::vector<std::uint8_t> len;
    if (!hex_bytes(f, pos, 1, len)) return bad("bad data length");
    std::vector<std::uint8_t> data;
    if (!hex_bytes(f, pos, len[0], data)) return bad("bad data");
    r.blocks.push_back(std::move(data));
  }
  if (pos != etx) return bad("trailing bytes");
  return r;
}

}  // namespace logict::xgt::cnet
