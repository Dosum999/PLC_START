#include "fins_protocol.h"

#include <cctype>
#include <charconv>
#include <cstring>
#include <format>
#include <optional>

namespace logict::fins {

namespace status = sdk::status;
using sdk::DataType;
using sdk::make_error;
using sdk::TagType;
using sdk::Value;
using sdk::codec::ByteOrder;

namespace {

struct Area {
  const char* name;
  std::uint8_t word_code, bit_code;
  std::uint32_t words;  // number of words in the area (offline range check)
};

constexpr Area kCio{"CIO", 0xB0, 0x30, 6144};
constexpr Area kAreas[] = {
    {"A", 0xB3, 0x33, 11536}, {"D", 0x82, 0x02, 32768}, {"H", 0xB2, 0x32, 1536}, {"W", 0xB1, 0x31, 512},
};
constexpr std::uint32_t kTimers = 4096, kCounterBase = 0x8000;

using Result = sdk::Result<std::shared_ptr<FinsAddress>>;
Result bad(std::string msg) { return make_error(status::BadInvalidArgument, std::move(msg)); }

template <typename T>
std::optional<T> num(std::string_view s, int base = 10) {
  T v{};
  if (s.empty()) return std::nullopt;
  auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
  return v;
}

bool split_dims(std::string& s, std::uint32_t& rows, std::uint32_t& cols) {
  rows = cols = 0;
  auto pos = s.find('[');
  if (pos == std::string::npos) return true;
  std::string_view d = std::string_view(s).substr(pos);
  std::uint32_t* target = &rows;
  while (!d.empty()) {
    if (d.front() != '[' || !target) return false;
    auto close = d.find(']');
    if (close == std::string_view::npos) return false;
    auto n = num<std::uint32_t>(d.substr(1, close - 1));
    if (!n || *n == 0 || *n > 65536) return false;
    *target = *n;
    target = target == &rows ? &cols : nullptr;
    d.remove_prefix(close + 1);
  }
  s.resize(pos);
  return true;
}

// Fills the word/bit part common to all word areas.
Result finish_word_area(std::shared_ptr<FinsAddress> a, const Area& area, std::string_view rest, const TagType& req) {
  std::string_view word_text = rest, sub;
  if (auto dot = rest.find('.'); dot != std::string_view::npos) {
    word_text = rest.substr(0, dot);
    sub = rest.substr(dot + 1);
  }
  auto word = num<std::uint32_t>(word_text);
  if (!word || *word >= area.words) return bad(std::format("{} word must be 0-{}", area.name, area.words - 1));

  if (!sub.empty() && req.scalar != DataType::String) {
    auto bit = num<std::uint32_t>(sub);
    if (!bit || *bit > 15 || sub.size() > 2) return bad("bit must be 00-15");
    if (req.scalar != DataType::Default && req.scalar != DataType::Boolean) return bad("bit addresses require type Boolean");
    a->type = TagType{DataType::Boolean, req.rows, req.cols};
    a->code = area.bit_code;
    a->bit_units = true;
    a->index = *word * 16 + *bit;
    a->points = a->type.element_count();
    if (a->points > kMaxBits) return bad("too many bits");
    if (a->index + a->points > area.words * 16) return bad("bit range exceeds the area");
    return a;
  }
  if (req.scalar == DataType::Boolean) return bad(std::format("use {}{}.00 .. .15 for a bit", area.name, *word));

  a->code = area.word_code;
  a->index = *word;
  if (req.scalar == DataType::String) {
    auto len = num<std::uint32_t>(sub);
    if (!len || *len == 0 || *len > 2 * kMaxWords) return bad("expected D100.<length> for String");
    if (req.is_array()) return bad("string arrays are not supported");
    a->type = TagType{DataType::String};
    a->string_len = *len;
    a->points = (*len + 1) / 2;
  } else {
    TagType t = req;
    if (t.scalar == DataType::Default) t.scalar = DataType::Word;
    const std::size_t size = sdk::native_size(t.scalar);
    if (size < 2 || t.scalar == DataType::Date) return bad(std::format("type {} is not supported", sdk::to_string(t.scalar)));
    a->type = t;
    a->points = static_cast<std::uint32_t>(size / 2 * t.element_count());
    if (a->points > kMaxWords) return bad("tag larger than 990 words");
  }
  if (a->index + a->points > area.words) return bad("range exceeds the area");
  return a;
}

}  // namespace

sdk::Result<std::shared_ptr<FinsAddress>> parse_address(std::string_view text, TagType requested) {
  auto a = std::make_shared<FinsAddress>();
  a->text = std::string(text);
  std::string s;
  for (char c : text)
    if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  std::uint32_t rows, cols;
  if (!split_dims(s, rows, cols)) return bad("invalid array dimensions");
  const TagType req{requested.scalar, rows, cols};
  std::string_view v = s;

  if (v.starts_with("CIO")) return finish_word_area(std::move(a), kCio, v.substr(3), req);
  if (v.starts_with("E") && v.find(':') != std::string_view::npos) {
    // E<bank>:<word>, bank in hex (0-18).
    const auto colon = v.find(':');
    auto bank = num<std::uint32_t>(v.substr(1, colon - 1), 16);
    if (!bank || *bank > 0x18) return bad("EM bank must be 0-18 (hex)");
    const std::uint8_t word_code = static_cast<std::uint8_t>(*bank < 0x10 ? 0xA0 + *bank : 0x60 + (*bank - 0x10));
    const std::uint8_t bit_code = static_cast<std::uint8_t>(*bank < 0x10 ? 0x20 + *bank : 0xE0 + (*bank - 0x10));
    return finish_word_area(std::move(a), Area{"E", word_code, bit_code, 32768}, v.substr(colon + 1), req);
  }
  if (v.starts_with("T") || v.starts_with("C")) {
    const bool counter = v.front() == 'C';
    auto n = num<std::uint32_t>(v.substr(1));
    if (!n || *n >= kTimers) return bad(std::format("{} number must be 0-4095", counter ? "counter" : "timer"));
    a->index = *n + (counter ? kCounterBase : 0);
    if (req.scalar == DataType::Boolean) {  // completion flags
      a->type = TagType{DataType::Boolean, rows, cols};
      a->code = 0x09;
      a->bit_units = true;
      a->points = a->type.element_count();
      if (*n + a->points > kTimers) return bad("range exceeds the area");
      return a;
    }
    TagType t = req;
    if (t.scalar == DataType::Default) t.scalar = DataType::Word;
    if (sdk::native_size(t.scalar) != 2) return bad("timer/counter PVs are 16-bit (Word, Short, BCD) or Boolean flags");
    a->type = t;
    a->code = 0x89;
    a->points = t.element_count();
    if (*n + a->points > kTimers) return bad("range exceeds the area");
    return a;
  }
  for (const auto& area : kAreas)
    if (v.starts_with(area.name)) return finish_word_area(std::move(a), area, v.substr(1), req);
  return bad("unknown area (CIO, A, D, E<bank>:, H, W, T, C)");
}

// ---- values ---------------------------------------------------------------------

namespace {

sdk::Result<Value> decode_scalar(DataType t, const std::uint8_t* p, ByteOrder o) {
  using sdk::codec::load;
  auto s = [&](std::size_t n) { return std::span<const std::uint8_t>(p, n); };
  switch (t) {
    case DataType::Short: return Value(load<std::int16_t>(s(2), o));
    case DataType::Word: return Value(load<std::uint16_t>(s(2), o));
    case DataType::Long: return Value(load<std::int32_t>(s(4), o));
    case DataType::DWord: return Value(load<std::uint32_t>(s(4), o));
    case DataType::LLong: return Value(load<std::int64_t>(s(8), o));
    case DataType::QWord: return Value(load<std::uint64_t>(s(8), o));
    case DataType::Float: return Value(load<float>(s(4), o));
    case DataType::Double: return Value(load<double>(s(8), o));
    case DataType::BCD: {
      auto v = sdk::codec::bcd_decode(load<std::uint16_t>(s(2), o), 4);
      if (!v) return make_error(status::BadOutOfRange, "invalid BCD value");
      return Value(static_cast<std::uint16_t>(*v));
    }
    case DataType::LBCD: {
      auto v = sdk::codec::bcd_decode(load<std::uint32_t>(s(4), o), 8);
      if (!v) return make_error(status::BadOutOfRange, "invalid LBCD value");
      return Value(*v);
    }
    default: return make_error(status::BadTypeMismatch, "unsupported type");
  }
}

sdk::StatusCode encode_scalar(DataType t, const Value& v, std::uint8_t* p, ByteOrder o) {
  using sdk::codec::store;
  auto c = sdk::convert(v, t);
  if (!c) return c.error().code;
  auto s = [&](std::size_t n) { return std::span<std::uint8_t>(p, n); };
  const Value& x = *c;
  switch (t) {
    case DataType::Short: store(*x.get_if<std::int16_t>(), s(2), o); break;
    case DataType::Word: store(*x.get_if<std::uint16_t>(), s(2), o); break;
    case DataType::Long: store(*x.get_if<std::int32_t>(), s(4), o); break;
    case DataType::DWord: store(*x.get_if<std::uint32_t>(), s(4), o); break;
    case DataType::LLong: store(*x.get_if<std::int64_t>(), s(8), o); break;
    case DataType::QWord: store(*x.get_if<std::uint64_t>(), s(8), o); break;
    case DataType::Float: store(*x.get_if<float>(), s(4), o); break;
    case DataType::Double: store(*x.get_if<double>(), s(8), o); break;
    case DataType::BCD: {
      auto b = sdk::codec::bcd_encode(*x.get_if<std::uint16_t>(), 4);
      if (!b) return status::BadOutOfRange;
      store(static_cast<std::uint16_t>(*b), s(2), o);
      break;
    }
    case DataType::LBCD: {
      auto b = sdk::codec::bcd_encode(*x.get_if<std::uint32_t>(), 8);
      if (!b) return status::BadOutOfRange;
      store(*b, s(4), o);
      break;
    }
    default: return status::BadTypeMismatch;
  }
  return status::Good;
}

}  // namespace

sdk::Result<Value> decode(const FinsAddress& a, std::span<const std::uint8_t> d, ByteOrder order) {
  if (a.bit_units) {
    if (d.size() < a.points) return make_error(status::BadInternalError, "short data");
    if (!a.type.is_array()) return Value(d[0] != 0);
    sdk::Array arr;
    for (std::uint32_t i = 0; i < a.points; ++i) arr.push_back(Value(d[i] != 0));
    return Value(std::move(arr));
  }
  if (d.size() < a.points * 2u) return make_error(status::BadInternalError, "short data");
  if (a.type.scalar == DataType::String) {
    // High byte first in each word.
    std::string s;
    for (std::uint32_t i = 0; i < a.string_len && d[i] != 0; ++i) s.push_back(static_cast<char>(d[i]));
    return Value(std::move(s));
  }
  if (!a.type.is_array()) return decode_scalar(a.type.scalar, d.data(), order);
  const std::size_t step = sdk::native_size(a.type.scalar);
  sdk::Array arr;
  for (std::uint32_t i = 0; i < a.type.element_count(); ++i) {
    auto v = decode_scalar(a.type.scalar, d.data() + i * step, order);
    if (!v) return v;
    arr.push_back(std::move(*v));
  }
  return Value(std::move(arr));
}

sdk::Result<std::vector<std::uint8_t>> encode(const FinsAddress& a, const Value& v, ByteOrder order) {
  if (a.bit_units) {
    std::vector<std::uint8_t> out;
    auto one = [&](const Value& e) -> sdk::StatusCode {
      auto b = sdk::convert(e, DataType::Boolean);
      if (!b) return b.error().code;
      out.push_back(*b->get_if<bool>() ? 1 : 0);
      return status::Good;
    };
    if (const auto* arr = v.get_if<sdk::Array>()) {
      if (arr->size() != a.points) return make_error(status::BadTypeMismatch, "array size mismatch");
      for (const auto& e : *arr)
        if (auto st = one(e); st != status::Good) return make_error(st, "not a boolean");
    } else if (auto st = one(v); st != status::Good) {
      return make_error(st, "not a boolean");
    }
    return out;
  }
  std::vector<std::uint8_t> out(a.points * 2u, 0);
  if (a.type.scalar == DataType::String) {
    auto s = sdk::convert(v, DataType::String);
    if (!s) return std::unexpected(s.error());
    const auto& str = *s->get_if<std::string>();
    if (str.size() > a.string_len) return make_error(status::BadOutOfRange, "string longer than the tag length");
    std::memcpy(out.data(), str.data(), str.size());
    return out;
  }
  if (!a.type.is_array()) {
    if (auto st = encode_scalar(a.type.scalar, v, out.data(), order); st != status::Good)
      return make_error(st, "cannot encode value");
    return out;
  }
  const std::size_t step = sdk::native_size(a.type.scalar);
  const auto* arr = v.get_if<sdk::Array>();
  if (!arr || arr->size() != a.type.element_count()) return make_error(status::BadTypeMismatch, "array size mismatch");
  for (std::size_t i = 0; i < arr->size(); ++i)
    if (auto st = encode_scalar(a.type.scalar, (*arr)[i], out.data() + i * step, order); st != status::Good)
      return make_error(st, "cannot encode array element");
  return out;
}

// ---- frames -------------------------------------------------------------------------

namespace {
void put16(std::vector<std::uint8_t>& o, std::uint16_t v) {
  o.push_back(static_cast<std::uint8_t>(v >> 8));
  o.push_back(static_cast<std::uint8_t>(v));
}
void put32(std::vector<std::uint8_t>& o, std::uint32_t v) {
  put16(o, static_cast<std::uint16_t>(v >> 16));
  put16(o, static_cast<std::uint16_t>(v));
}
std::uint32_t get32(std::span<const std::uint8_t> p) {
  return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
         static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

std::vector<std::uint8_t> command(const Header& h, std::uint8_t sid, std::uint8_t mrc, std::uint8_t src) {
  // ICF 0x80: command, response required. GCT 0x02: up to two bridges.
  return {0x80, 0x00, 0x02, h.dna, h.da1, h.da2, h.sna, h.sa1, h.sa2, sid, mrc, src};
}
void put_area(std::vector<std::uint8_t>& f, std::uint8_t code, std::uint16_t word, std::uint8_t bit, std::uint16_t count) {
  f.push_back(code);
  put16(f, word);
  f.push_back(bit);
  put16(f, count);
}
}  // namespace

std::vector<std::uint8_t> memory_read(const Header& h, std::uint8_t sid, std::uint8_t code, std::uint16_t word,
                                      std::uint8_t bit, std::uint16_t count) {
  auto f = command(h, sid, kMrcMemory, kSrcRead);
  put_area(f, code, word, bit, count);
  return f;
}

std::vector<std::uint8_t> memory_write(const Header& h, std::uint8_t sid, std::uint8_t code, std::uint16_t word,
                                       std::uint8_t bit, std::uint16_t count, std::span<const std::uint8_t> payload) {
  auto f = command(h, sid, kMrcMemory, kSrcWrite);
  put_area(f, code, word, bit, count);
  f.insert(f.end(), payload.begin(), payload.end());
  return f;
}

sdk::Result<Response> parse_response(std::span<const std::uint8_t> f, std::uint8_t mrc, std::uint8_t src) {
  if (f.size() < kResponseHeader) return make_error(status::BadCommunicationError, "truncated FINS response");
  if ((f[0] & 0x40) == 0) return make_error(status::BadCommunicationError, "not a FINS response");
  if (f[10] != mrc || f[11] != src) return make_error(status::BadCommunicationError, "response to another command");
  Response r;
  r.sid = f[9];
  // MRES bit 7: relay error; SRES bits 6/7: non-fatal / fatal CPU error flags.
  r.end_code = static_cast<std::uint16_t>((f[12] & 0x7F) << 8 | (f[13] & 0x3F));
  r.data.assign(f.begin() + kResponseHeader, f.end());
  return r;
}

sdk::StatusCode end_code_status(std::uint16_t code) {
  if (code == 0x0000) return status::Good;
  switch (code >> 8) {
    case 0x01:  // local node error
    case 0x02:  // destination node error
    case 0x03:  // communications controller error
      return status::BadCommunicationError;
    case 0x04: return status::BadNotSupported;  // undefined command / not supported by model
    case 0x10:                                  // command format error
    case 0x11:                                  // parameter error (area, address range)
      return status::BadOutOfRange;
    case 0x21: return status::BadNotWritable;   // read-only area, protected
    case 0x22: return status::BadDeviceFailure; // wrong mode (e.g. program running)
    default: return status::BadDeviceFailure;
  }
}

std::vector<std::uint8_t> tcp_node_request(std::uint8_t client_node) {
  std::vector<std::uint8_t> f{'F', 'I', 'N', 'S'};
  put32(f, 12);
  put32(f, kTcpNodeRequest);
  put32(f, 0);
  put32(f, client_node);
  return f;
}

std::vector<std::uint8_t> tcp_frame(std::span<const std::uint8_t> fins) {
  std::vector<std::uint8_t> f{'F', 'I', 'N', 'S'};
  put32(f, static_cast<std::uint32_t>(8 + fins.size()));
  put32(f, kTcpFrame);
  put32(f, 0);
  f.insert(f.end(), fins.begin(), fins.end());
  return f;
}

sdk::Result<TcpHeader> parse_tcp_header(std::span<const std::uint8_t> h) {
  if (h.size() < kTcpHeader || h[0] != 'F' || h[1] != 'I' || h[2] != 'N' || h[3] != 'S')
    return make_error(status::BadCommunicationError, "not a FINS/TCP header");
  TcpHeader t{get32(h.subspan(4)), get32(h.subspan(8)), get32(h.subspan(12))};
  if (t.length < 8 || t.length > 8 + 4096) return make_error(status::BadCommunicationError, "bad FINS/TCP length");
  return t;
}

}  // namespace logict::fins
