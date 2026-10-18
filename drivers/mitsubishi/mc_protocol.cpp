#include "mc_protocol.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>

#include "logict/sdk/codec.h"

namespace logict::mc {

namespace status = sdk::status;
using sdk::DataType;
using sdk::make_error;
using sdk::TagType;
using sdk::Value;

namespace {

// Two-letter names first so that e.g. "SM" wins over "S".
constexpr std::array<Device, 27> kDevices{{
    {"SM", 0x91, true, 10},  {"SD", 0xA9, false, 10}, {"SB", 0xA1, true, 16},  {"SW", 0xB5, false, 16},
    {"DX", 0xA2, true, 16},  {"DY", 0xA3, true, 16},  {"TS", 0xC1, true, 10},  {"TC", 0xC0, true, 10},
    {"TN", 0xC2, false, 10}, {"SS", 0xC7, true, 10},  {"SC", 0xC6, true, 10},  {"SN", 0xC8, false, 10},
    {"CS", 0xC4, true, 10},  {"CC", 0xC3, true, 10},  {"CN", 0xC5, false, 10}, {"ZR", 0xB0, false, 10},
    {"X", 0x9C, true, 16},   {"Y", 0x9D, true, 16},   {"M", 0x90, true, 10},   {"L", 0x92, true, 10},
    {"F", 0x93, true, 10},   {"V", 0x94, true, 10},   {"B", 0xA0, true, 16},   {"D", 0xA8, false, 10},
    {"W", 0xB4, false, 16},  {"R", 0xAF, false, 10},  {"Z", 0xCC, false, 10},
}};

using Result = sdk::Result<std::shared_ptr<McAddress>>;
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

}  // namespace

const Device* find_device(std::string_view name) {
  for (const auto& d : kDevices)
    if (name.starts_with(d.name)) return &d;
  return nullptr;
}

sdk::Result<std::shared_ptr<McAddress>> parse_address(std::string_view text, TagType requested, const AddressOptions& opt) {
  auto a = std::make_shared<McAddress>();
  a->text = std::string(text);
  std::string s;
  for (char c : text)
    if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  std::uint32_t rows, cols;
  if (!split_dims(s, rows, cols)) return bad("invalid array dimensions");
  const Device* d = find_device(s);
  if (!d) return bad("unknown device (D, W, R, ZR, M, X, Y, B, L, F, TN, TS, CN, CS, SM, SD, ...)");
  a->device = d;
  std::string rest = s.substr(std::strlen(d->name)), sub;
  if (auto dot = rest.find('.'); dot != std::string::npos) {
    sub = rest.substr(dot + 1);
    rest.resize(dot);
  }
  int base = d->base;
  if (opt.xy_octal && (d->code == 0x9C || d->code == 0x9D)) base = 8;
  auto n = num<std::uint32_t>(rest, base);
  if (!n || *n > 0xFFFFFF) return bad(std::format("invalid device number for {} (base {})", d->name, base));
  a->number = *n;
  const TagType req{requested.scalar, rows, cols};

  if (d->bit) {
    if (!sub.empty()) return bad("bit devices have no sub-address");
    if (req.scalar == DataType::Default || req.scalar == DataType::Boolean) {
      a->type = TagType{DataType::Boolean, rows, cols};
      a->bit_units = true;
      a->points = a->type.element_count();
      if (a->points > kMaxBits) return bad("too many points");
      return a;
    }
    // A bit device read as words: 16 points per word from a multiple of 16.
    if (*n % 16 != 0) return bad("word access to a bit device must start at a multiple of 16");
  } else if (!sub.empty() && req.scalar != DataType::String) {
    auto b = num<int>(sub, 16);
    if (!b || *b > 15) return bad("bit must be 0-F");
    if (req.scalar != DataType::Default && req.scalar != DataType::Boolean) return bad("bit addresses require type Boolean");
    if (req.is_array()) return bad("arrays of word bits are not supported");
    a->type = TagType{DataType::Boolean};
    a->bit = *b;
    a->points = 1;
    return a;
  } else if (req.scalar == DataType::Boolean) {
    return bad(std::format("use {}{}.0 .. .F for a bit of a word device", d->name, rest));
  }

  TagType t = req;
  if (t.scalar == DataType::String) {
    auto len = num<std::uint32_t>(sub);
    if (!len || *len == 0 || *len > 960) return bad("expected D100.<length> for String");
    if (req.is_array()) return bad("string arrays are not supported");
    a->string_len = *len;
    a->type = TagType{DataType::String};
    a->points = (*len + 1) / 2;
    return a;
  }
  if (t.scalar == DataType::Default) t.scalar = DataType::Word;
  const std::size_t size = sdk::native_size(t.scalar);
  if (size < 2 || t.scalar == DataType::Date) return bad(std::format("type {} is not supported", sdk::to_string(t.scalar)));
  a->type = t;
  a->points = static_cast<std::uint32_t>(size / 2 * t.element_count());
  if (a->points > kMaxWords) return bad("tag larger than 960 words");
  return a;
}

// ---- values ---------------------------------------------------------------------

namespace {

sdk::Result<Value> decode_scalar(DataType t, const std::uint8_t* p) {
  using sdk::codec::ByteOrder;
  using sdk::codec::load;
  auto s = [&](std::size_t n) { return std::span<const std::uint8_t>(p, n); };
  switch (t) {
    case DataType::Short: return Value(load<std::int16_t>(s(2), ByteOrder::DCBA));
    case DataType::Word: return Value(load<std::uint16_t>(s(2), ByteOrder::DCBA));
    case DataType::Long: return Value(load<std::int32_t>(s(4), ByteOrder::DCBA));
    case DataType::DWord: return Value(load<std::uint32_t>(s(4), ByteOrder::DCBA));
    case DataType::LLong: return Value(load<std::int64_t>(s(8), ByteOrder::DCBA));
    case DataType::QWord: return Value(load<std::uint64_t>(s(8), ByteOrder::DCBA));
    case DataType::Float: return Value(load<float>(s(4), ByteOrder::DCBA));
    case DataType::Double: return Value(load<double>(s(8), ByteOrder::DCBA));
    case DataType::BCD: {
      auto v = sdk::codec::bcd_decode(load<std::uint16_t>(s(2), ByteOrder::DCBA), 4);
      if (!v) return make_error(status::BadOutOfRange, "invalid BCD value");
      return Value(static_cast<std::uint16_t>(*v));
    }
    case DataType::LBCD: {
      auto v = sdk::codec::bcd_decode(load<std::uint32_t>(s(4), ByteOrder::DCBA), 8);
      if (!v) return make_error(status::BadOutOfRange, "invalid LBCD value");
      return Value(*v);
    }
    default: return make_error(status::BadTypeMismatch, "unsupported type");
  }
}

sdk::StatusCode encode_scalar(DataType t, const Value& v, std::uint8_t* p) {
  using sdk::codec::ByteOrder;
  using sdk::codec::store;
  auto c = sdk::convert(v, t);
  if (!c) return c.error().code;
  auto s = [&](std::size_t n) { return std::span<std::uint8_t>(p, n); };
  const Value& x = *c;
  switch (t) {
    case DataType::Short: store(*x.get_if<std::int16_t>(), s(2), ByteOrder::DCBA); break;
    case DataType::Word: store(*x.get_if<std::uint16_t>(), s(2), ByteOrder::DCBA); break;
    case DataType::Long: store(*x.get_if<std::int32_t>(), s(4), ByteOrder::DCBA); break;
    case DataType::DWord: store(*x.get_if<std::uint32_t>(), s(4), ByteOrder::DCBA); break;
    case DataType::LLong: store(*x.get_if<std::int64_t>(), s(8), ByteOrder::DCBA); break;
    case DataType::QWord: store(*x.get_if<std::uint64_t>(), s(8), ByteOrder::DCBA); break;
    case DataType::Float: store(*x.get_if<float>(), s(4), ByteOrder::DCBA); break;
    case DataType::Double: store(*x.get_if<double>(), s(8), ByteOrder::DCBA); break;
    case DataType::BCD: store(static_cast<std::uint16_t>(*sdk::codec::bcd_encode(*x.get_if<std::uint16_t>(), 4)), s(2), ByteOrder::DCBA); break;
    case DataType::LBCD: store(*sdk::codec::bcd_encode(*x.get_if<std::uint32_t>(), 8), s(4), ByteOrder::DCBA); break;
    default: return status::BadTypeMismatch;
  }
  return status::Good;
}

}  // namespace

sdk::Result<Value> decode(const McAddress& a, std::span<const std::uint8_t> d) {
  if (a.bit_units) {
    // One byte (0/1) per point.
    if (d.size() < a.points) return make_error(status::BadInternalError, "short data");
    if (!a.type.is_array()) return Value(d[0] != 0);
    sdk::Array arr;
    for (std::uint32_t i = 0; i < a.points; ++i) arr.push_back(Value(d[i] != 0));
    return Value(std::move(arr));
  }
  if (d.size() < a.points * 2u) return make_error(status::BadInternalError, "short data");
  if (a.bit >= 0) return Value(((d[a.bit / 8] >> (a.bit % 8)) & 1) != 0);
  if (a.type.scalar == DataType::String) {
    std::string s;
    for (std::uint32_t i = 0; i < a.string_len && d[i] != 0; ++i) s.push_back(static_cast<char>(d[i]));
    return Value(std::move(s));
  }
  const std::size_t step = sdk::native_size(a.type.scalar);
  if (!a.type.is_array()) return decode_scalar(a.type.scalar, d.data());
  sdk::Array arr;
  for (std::uint32_t i = 0; i < a.type.element_count(); ++i) {
    auto v = decode_scalar(a.type.scalar, d.data() + i * step);
    if (!v) return v;
    arr.push_back(std::move(*v));
  }
  return Value(std::move(arr));
}

sdk::Result<std::vector<std::uint8_t>> encode(const McAddress& a, const Value& v) {
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
  if (a.bit >= 0) return make_error(status::BadInternalError, "word bits are written by read-modify-write");
  std::vector<std::uint8_t> out(a.points * 2u, 0);
  if (a.type.scalar == DataType::String) {
    auto s = sdk::convert(v, DataType::String);
    if (!s) return std::unexpected(s.error());
    const auto& str = *s->get_if<std::string>();
    if (str.size() > a.string_len) return make_error(status::BadOutOfRange, "string longer than the tag length");
    std::memcpy(out.data(), str.data(), str.size());
    return out;
  }
  const std::size_t step = sdk::native_size(a.type.scalar);
  if (!a.type.is_array()) {
    if (auto st = encode_scalar(a.type.scalar, v, out.data()); st != status::Good) return make_error(st, "cannot encode value");
    return out;
  }
  const auto* arr = v.get_if<sdk::Array>();
  if (!arr || arr->size() != a.type.element_count()) return make_error(status::BadTypeMismatch, "array size mismatch");
  for (std::size_t i = 0; i < arr->size(); ++i)
    if (auto st = encode_scalar(a.type.scalar, (*arr)[i], out.data() + i * step); st != status::Good)
      return make_error(st, "cannot encode array element");
  return out;
}

// ---- frames -------------------------------------------------------------------------

namespace {
void put16(std::vector<std::uint8_t>& o, std::uint16_t v) {
  o.push_back(static_cast<std::uint8_t>(v));
  o.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put_head(std::vector<std::uint8_t>& o, const Device& d, std::uint32_t number, std::uint16_t points) {
  o.push_back(static_cast<std::uint8_t>(number));
  o.push_back(static_cast<std::uint8_t>(number >> 8));
  o.push_back(static_cast<std::uint8_t>(number >> 16));
  o.push_back(d.code);
  put16(o, points);
}
}  // namespace

std::vector<std::uint8_t> request(const Route& r, std::uint16_t timer, std::uint16_t command, std::uint16_t sub,
                                  std::span<const std::uint8_t> data) {
  std::vector<std::uint8_t> f{0x50, 0x00, r.network, r.pc};
  put16(f, r.io);
  f.push_back(r.station);
  put16(f, static_cast<std::uint16_t>(6 + data.size()));
  put16(f, timer);
  put16(f, command);
  put16(f, sub);
  f.insert(f.end(), data.begin(), data.end());
  return f;
}

std::vector<std::uint8_t> batch_read(const Route& r, std::uint16_t timer, const Device& d, std::uint32_t number,
                                     std::uint16_t points, bool bit_units) {
  std::vector<std::uint8_t> data;
  put_head(data, d, number, points);
  return request(r, timer, kBatchRead, bit_units ? kSubBit : kSubWord, data);
}

std::vector<std::uint8_t> batch_write(const Route& r, std::uint16_t timer, const Device& d, std::uint32_t number,
                                      std::uint16_t points, bool bit_units, std::span<const std::uint8_t> payload) {
  std::vector<std::uint8_t> data;
  put_head(data, d, number, points);
  data.insert(data.end(), payload.begin(), payload.end());
  return request(r, timer, kBatchWrite, bit_units ? kSubBit : kSubWord, data);
}

std::vector<std::uint8_t> pack_bits(std::span<const std::uint8_t> points) {
  std::vector<std::uint8_t> out((points.size() + 1) / 2, 0);
  for (std::size_t i = 0; i < points.size(); ++i)
    if (points[i]) out[i / 2] |= static_cast<std::uint8_t>(i % 2 == 0 ? 0x10 : 0x01);
  return out;
}

bool unpack_bit(std::span<const std::uint8_t> data, std::uint32_t i) {
  const std::uint8_t b = data[i / 2];
  return (i % 2 == 0 ? (b >> 4) : b) & 1;
}

sdk::Result<std::uint16_t> response_length(std::span<const std::uint8_t> h) {
  if (h.size() < kResponseHeader || h[0] != 0xD0 || h[1] != 0x00)
    return make_error(status::BadCommunicationError, "not an MC 3E binary response");
  const std::uint16_t len = static_cast<std::uint16_t>(h[7] | (h[8] << 8));
  if (len < 2) return make_error(status::BadCommunicationError, "bad response length");
  return len;
}

sdk::Result<Response> parse_response(std::span<const std::uint8_t> rest) {
  if (rest.size() < 2) return make_error(status::BadCommunicationError, "truncated response");
  Response r;
  r.end_code = static_cast<std::uint16_t>(rest[0] | (rest[1] << 8));
  r.data.assign(rest.begin() + 2, rest.end());
  return r;
}

sdk::StatusCode end_code_status(std::uint16_t code) {
  switch (code) {
    case 0x0000: return status::Good;
    case 0xC050:  // ASCII data error
    case 0xC051:
    case 0xC052:
    case 0xC053:
    case 0xC054:  // number of points out of range
    case 0xC056:  // device range exceeded
    case 0xC05B:  // device cannot be accessed
    case 0x4031:  // iQ-R: device range
      return status::BadOutOfRange;
    case 0xC059: return status::BadNotSupported;  // command/subcommand not supported
    default: return status::BadDeviceFailure;
  }
}

}  // namespace logict::mc
