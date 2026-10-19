#include "mc_protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>
#include <optional>

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

// ---- ASCII code ---------------------------------------------------------------------

namespace {

void hex_field(std::vector<std::uint8_t>& o, std::uint32_t v, int digits) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (int i = digits - 1; i >= 0; --i) o.push_back(static_cast<std::uint8_t>(kHex[(v >> (4 * i)) & 0xF]));
}

std::optional<std::uint32_t> parse_hex(std::span<const std::uint8_t> t) {
  std::uint32_t v = 0;
  if (t.empty()) return std::nullopt;
  for (auto c : t) {
    int n;
    if (c >= '0' && c <= '9') n = c - '0';
    else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
    else return std::nullopt;
    v = v << 4 | static_cast<std::uint32_t>(n);
  }
  return v;
}

// Request data from the command on (shared by 3E ASCII and 3C/4C frames).
std::vector<std::uint8_t> ascii_command(std::uint16_t command, std::uint16_t sub, const Device& d, int base,
                                        std::uint32_t number, std::uint16_t points) {
  std::vector<std::uint8_t> body;
  hex_field(body, command, 4);
  hex_field(body, sub, 4);
  body.push_back(static_cast<std::uint8_t>(d.name[0]));
  body.push_back(static_cast<std::uint8_t>(d.name[1] ? d.name[1] : '*'));
  const std::string num = base == 16  ? std::format("{:06X}", number)
                          : base == 8 ? std::format("{:06o}", number)
                                      : std::format("{:06}", number);
  body.insert(body.end(), num.begin(), num.end());
  hex_field(body, points, 4);
  return body;
}

// Write data in ASCII from the binary layout.
void append_ascii_data(std::vector<std::uint8_t>& body, std::span<const std::uint8_t> payload, bool bit_units,
                       std::uint16_t points) {
  if (bit_units) {
    for (std::uint32_t i = 0; i < points; ++i) body.push_back(unpack_bit(payload, i) ? '1' : '0');
  } else {
    for (std::size_t i = 0; i + 1 < payload.size(); i += 2)
      hex_field(body, static_cast<std::uint32_t>(payload[i] | (payload[i + 1] << 8)), 4);
  }
}

// Read data in ASCII -> binary layout.
sdk::Result<std::vector<std::uint8_t>> ascii_data(std::span<const std::uint8_t> data, bool bit_units, std::uint16_t points) {
  std::vector<std::uint8_t> out;
  if (bit_units) {
    if (data.size() != points) return make_error(status::BadCommunicationError, "bad bit data length");
    std::vector<std::uint8_t> pts;
    for (auto c : data) {
      if (c != '0' && c != '1') return make_error(status::BadCommunicationError, "bad bit data");
      pts.push_back(c == '1' ? 1 : 0);
    }
    return pack_bits(pts);
  }
  if (data.size() % 4 != 0) return make_error(status::BadCommunicationError, "bad word data length");
  for (std::size_t i = 0; i < data.size(); i += 4) {
    auto w = parse_hex(data.subspan(i, 4));
    if (!w) return make_error(status::BadCommunicationError, "bad word data");
    out.push_back(static_cast<std::uint8_t>(*w));
    out.push_back(static_cast<std::uint8_t>(*w >> 8));
  }
  return out;
}

// Command body from the monitoring timer on.
std::vector<std::uint8_t> ascii_body(std::uint16_t timer, std::uint16_t command, std::uint16_t sub, const Device& d,
                                     int base, std::uint32_t number, std::uint16_t points) {
  std::vector<std::uint8_t> body;
  hex_field(body, timer, 4);
  auto cmd = ascii_command(command, sub, d, base, number, points);
  body.insert(body.end(), cmd.begin(), cmd.end());
  return body;
}

std::vector<std::uint8_t> ascii_frame(const Route& r, const std::vector<std::uint8_t>& body) {
  std::vector<std::uint8_t> f{'5', '0', '0', '0'};
  hex_field(f, r.network, 2);
  hex_field(f, r.pc, 2);
  hex_field(f, r.io, 4);
  hex_field(f, r.station, 2);
  hex_field(f, static_cast<std::uint32_t>(body.size()), 4);  // characters from the timer field on
  f.insert(f.end(), body.begin(), body.end());
  return f;
}

}  // namespace

std::vector<std::uint8_t> batch_read_ascii(const Route& r, std::uint16_t timer, const Device& d, int base,
                                           std::uint32_t number, std::uint16_t points, bool bit_units) {
  return ascii_frame(r, ascii_body(timer, kBatchRead, bit_units ? kSubBit : kSubWord, d, base, number, points));
}

std::vector<std::uint8_t> batch_write_ascii(const Route& r, std::uint16_t timer, const Device& d, int base,
                                            std::uint32_t number, std::uint16_t points, bool bit_units,
                                            std::span<const std::uint8_t> payload) {
  auto body = ascii_body(timer, kBatchWrite, bit_units ? kSubBit : kSubWord, d, base, number, points);
  append_ascii_data(body, payload, bit_units, points);
  return ascii_frame(r, body);
}

sdk::Result<std::uint16_t> response_length_ascii(std::span<const std::uint8_t> h) {
  if (h.size() < kAsciiResponseHeader || h[0] != 'D' || h[1] != '0' || h[2] != '0' || h[3] != '0')
    return make_error(status::BadCommunicationError, "not an MC 3E ASCII response");
  auto len = parse_hex(h.subspan(14, 4));
  if (!len || *len < 4) return make_error(status::BadCommunicationError, "bad response length");
  return static_cast<std::uint16_t>(*len);
}

sdk::Result<Response> parse_response_ascii(std::span<const std::uint8_t> rest, bool bit_units, std::uint16_t points) {
  if (rest.size() < 4) return make_error(status::BadCommunicationError, "truncated response");
  auto end = parse_hex(rest.first(4));
  if (!end) return make_error(status::BadCommunicationError, "bad end code");
  Response r;
  r.end_code = static_cast<std::uint16_t>(*end);
  auto data = rest.subspan(4);
  if (r.end_code != 0 || data.empty()) return r;  // error responses carry diagnostic info, not data
  auto bin = ascii_data(data, bit_units, points);
  if (!bin) return std::unexpected(bin.error());
  r.data = std::move(*bin);
  return r;
}

// ---- 3C / 4C frames (serial, ASCII formats 1 and 4) -------------------------------------

namespace {

std::uint8_t sum_of(std::span<const std::uint8_t> bytes) {
  unsigned sum = 0;
  for (auto b : bytes) sum += b;
  return static_cast<std::uint8_t>(sum);
}

// Frame ID and access route as sent in requests and echoed in responses.
std::vector<std::uint8_t> c_header(const CFrame& c) {
  std::vector<std::uint8_t> h;
  h.push_back('F');
  h.push_back(c.four_c ? '8' : '9');
  hex_field(h, c.station, 2);
  hex_field(h, c.network, 2);
  hex_field(h, c.pc, 2);
  if (c.four_c) {
    hex_field(h, c.dest_io, 4);
    hex_field(h, c.dest_station, 2);
  }
  hex_field(h, c.self_station, 2);
  return h;
}

std::vector<std::uint8_t> c_request(const CFrame& c, const std::vector<std::uint8_t>& data) {
  std::vector<std::uint8_t> f{0x05};  // ENQ
  auto h = c_header(c);
  f.insert(f.end(), h.begin(), h.end());
  f.insert(f.end(), data.begin(), data.end());
  if (c.sum_check) hex_field(f, sum_of(std::span(f).subspan(1)), 2);
  if (c.format4) {
    f.push_back('\r');
    f.push_back('\n');
  }
  return f;
}

}  // namespace

std::size_t c_header_size(const CFrame& c) { return c.four_c ? 16 : 10; }

std::vector<std::uint8_t> c_batch_read(const CFrame& c, const Device& d, int base, std::uint32_t number,
                                       std::uint16_t points, bool bit_units) {
  return c_request(c, ascii_command(kBatchRead, bit_units ? kSubBit : kSubWord, d, base, number, points));
}

std::vector<std::uint8_t> c_batch_write(const CFrame& c, const Device& d, int base, std::uint32_t number,
                                        std::uint16_t points, bool bit_units, std::span<const std::uint8_t> payload) {
  auto data = ascii_command(kBatchWrite, bit_units ? kSubBit : kSubWord, d, base, number, points);
  append_ascii_data(data, payload, bit_units, points);
  return c_request(c, data);
}

sdk::Result<Response> parse_c_response(const CFrame& c, std::span<const std::uint8_t> f, bool bit_units,
                                       std::uint16_t points) {
  auto bad = [](const char* why) { return make_error(status::BadCommunicationError, why); };
  if (c.format4) {
    if (f.size() < 2 || f[f.size() - 2] != '\r' || f.back() != '\n') return bad("missing CR LF");
    f = f.first(f.size() - 2);
  }
  const std::size_t hs = c_header_size(c);
  if (f.size() < 1 + hs) return bad("truncated response");
  const auto h = c_header(c);
  if (!std::equal(h.begin(), h.end(), f.begin() + 1)) return bad("response for another station or frame");
  Response r;
  switch (f[0]) {
    case 0x06:  // ACK: normal completion without data
      if (f.size() != 1 + hs) return bad("bad ACK length");
      return r;
    case 0x15: {  // NAK + error code
      if (f.size() != 1 + hs + 4) return bad("bad NAK length");
      auto code = parse_hex(f.subspan(1 + hs, 4));
      if (!code) return bad("bad error code");
      r.end_code = static_cast<std::uint16_t>(*code);
      if (r.end_code == 0) r.end_code = 0xFFFF;  // never report a NAK as success
      return r;
    }
    case 0x02: {  // STX ... ETX [sum]
      const std::size_t tail = c.sum_check ? 3 : 1;
      if (f.size() < 1 + hs + tail || f[f.size() - tail] != 0x03) return bad("missing ETX");
      if (c.sum_check) {
        auto sum = parse_hex(f.last(2));
        if (!sum || *sum != sum_of(f.subspan(1, f.size() - 3))) return bad("sum check mismatch");
      }
      auto bin = ascii_data(f.subspan(1 + hs, f.size() - 1 - hs - tail), bit_units, points);
      if (!bin) return std::unexpected(bin.error());
      r.data = std::move(*bin);
      return r;
    }
    default: return bad("unknown control code");
  }
}

// ---- 1E frame ------------------------------------------------------------------------

std::uint16_t device_code_1e(const Device& d) {
  const std::string_view n = d.name;
  if (n == "D") return 0x4420;
  if (n == "R") return 0x5220;
  if (n == "W") return 0x5720;
  if (n == "TN") return 0x544E;
  if (n == "TS") return 0x5453;
  if (n == "TC") return 0x5443;
  if (n == "CN") return 0x434E;
  if (n == "CS") return 0x4353;
  if (n == "CC") return 0x4343;
  if (n == "X") return 0x5820;
  if (n == "Y") return 0x5920;
  if (n == "M") return 0x4D20;  // FX special relays are M8000 and up
  if (n == "B") return 0x4220;
  if (n == "F") return 0x4620;
  return 0;
}

namespace {
std::vector<std::uint8_t> head_1e(std::uint8_t command, std::uint8_t pc, std::uint16_t timer, std::uint16_t code,
                                  std::uint32_t number, std::uint16_t points) {
  return {command,
          pc,
          static_cast<std::uint8_t>(timer),
          static_cast<std::uint8_t>(timer >> 8),
          static_cast<std::uint8_t>(number),
          static_cast<std::uint8_t>(number >> 8),
          static_cast<std::uint8_t>(number >> 16),
          static_cast<std::uint8_t>(number >> 24),
          static_cast<std::uint8_t>(code),
          static_cast<std::uint8_t>(code >> 8),
          static_cast<std::uint8_t>(points),  // 256 -> 0
          0x00};
}
}  // namespace

std::vector<std::uint8_t> batch_read_1e(std::uint8_t pc, std::uint16_t timer, std::uint16_t code, std::uint32_t number,
                                        std::uint16_t points, bool bit_units) {
  return head_1e(bit_units ? 0x00 : 0x01, pc, timer, code, number, points);
}

std::vector<std::uint8_t> batch_write_1e(std::uint8_t pc, std::uint16_t timer, std::uint16_t code, std::uint32_t number,
                                         std::uint16_t points, bool bit_units, std::span<const std::uint8_t> payload) {
  auto f = head_1e(bit_units ? 0x02 : 0x03, pc, timer, code, number, points);
  f.insert(f.end(), payload.begin(), payload.end());
  return f;
}

sdk::StatusCode end_code_status_1e(std::uint8_t end, std::uint8_t abnormal) {
  switch (end) {
    case 0x00: return status::Good;
    case 0x50: return status::BadNotSupported;   // command / response type error
    case 0x55: return status::BadNotWritable;    // write during RUN not permitted
    case 0x51:                                   // device range
    case 0x52:
    case 0x56:                                   // device specification error
    case 0x57:                                   // number of points
    case 0x58: return status::BadOutOfRange;     // head + points beyond the range
    case 0x5B: return abnormal == 0x10 || abnormal == 0x11 ? status::BadOutOfRange : status::BadDeviceFailure;
    default: return status::BadDeviceFailure;
  }
}

}  // namespace logict::mc
