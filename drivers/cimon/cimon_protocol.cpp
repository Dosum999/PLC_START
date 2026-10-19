#include "cimon_protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>
#include <optional>

#include "logict/sdk/codec.h"

namespace logict::cimon {

namespace status = sdk::status;
using sdk::DataType;
using sdk::make_error;
using sdk::TagType;
using sdk::Value;

namespace {

// Two-letter names first. Bit devices: words addressable with a 5-digit word.
constexpr std::array<Device, 15> kDevices{{
    {"TC", Kind::Word, 1000000}, {"TS", Kind::Word, 1000000}, {"CC", Kind::Word, 1000000}, {"CS", Kind::Word, 1000000},
    {"X", Kind::Bit, 100000},    {"Y", Kind::Bit, 100000},    {"M", Kind::Bit, 100000},    {"L", Kind::Bit, 100000},
    {"K", Kind::Bit, 100000},    {"F", Kind::Bit, 100000},    {"T", Kind::Contact, 1000000}, {"C", Kind::Contact, 1000000},
    {"D", Kind::Word, 1000000},  {"S", Kind::Word, 1000000},  {"Z", Kind::Word, 1000000},
}};

using AddrResult = sdk::Result<std::shared_ptr<CimonAddress>>;
AddrResult bad(std::string msg) { return make_error(status::BadInvalidArgument, std::move(msg)); }

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

constexpr std::uint32_t kMaxTagWords = 512, kMaxTagBits = 1024;

}  // namespace

const Device* find_device(std::string_view name) {
  for (const auto& d : kDevices)
    if (name.starts_with(d.name)) return &d;
  return nullptr;
}

std::uint32_t device_index(const Device& d) { return static_cast<std::uint32_t>(&d - kDevices.data()); }
const Device& device_at(std::uint32_t index) { return kDevices.at(index); }

sdk::Result<std::shared_ptr<CimonAddress>> parse_address(std::string_view text, TagType requested) {
  auto a = std::make_shared<CimonAddress>();
  a->text = std::string(text);
  std::string s;
  for (char c : text)
    if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  std::uint32_t rows, cols;
  if (!split_dims(s, rows, cols)) return bad("invalid array dimensions");
  const Device* d = find_device(s);
  if (!d) return bad("unknown device (X, Y, M, L, K, F, T, C, D, S, Z, TC, TS, CC, CS)");
  a->device = d;
  std::string_view rest = std::string_view(s).substr(std::strlen(d->name)), sub;
  if (auto dot = rest.find('.'); dot != std::string_view::npos) {
    sub = rest.substr(dot + 1);
    rest = rest.substr(0, dot);
  }
  const TagType req{requested.scalar, rows, cols};

  if (d->kind == Kind::Contact) {
    auto n = num<std::uint32_t>(rest);
    if (!n || !sub.empty()) return bad(std::format("invalid {} number", d->name));
    if (req.scalar != DataType::Default && req.scalar != DataType::Boolean) return bad("timer/counter contacts are Boolean (use TC/TS/CC/CS for values)");
    a->type = TagType{DataType::Boolean, rows, cols};
    a->bit_units = true;
    a->start = *n;
    a->points = a->type.element_count();
    if (a->points > kMaxTagBits || *n + a->points > d->count) return bad("range exceeds the device");
    return a;
  }

  if (d->kind == Kind::Bit) {
    if (!sub.empty()) return bad("bit devices have no sub-address");
    if (req.scalar == DataType::Default || req.scalar == DataType::Boolean) {
      // Word digits and a hex bit digit: M0104 = word 10, bit 4.
      if (rest.size() < 2) return bad("bit address needs a word and a bit digit (e.g. M0104)");
      auto word = num<std::uint32_t>(rest.substr(0, rest.size() - 1));
      auto bit = num<std::uint32_t>(rest.substr(rest.size() - 1), 16);
      if (!word || !bit || *word >= d->count) return bad(std::format("invalid {} bit address", d->name));
      a->type = TagType{DataType::Boolean, rows, cols};
      a->bit_units = true;
      a->start = *word * 16 + *bit;
      a->points = a->type.element_count();
      if (a->points > kMaxTagBits || a->start + a->points > d->count * 16) return bad("range exceeds the device");
      return a;
    }
    auto word = num<std::uint32_t>(rest);
    if (!word || *word >= d->count) return bad(std::format("invalid {} word number", d->name));
    a->start = *word;
    if (req.scalar == DataType::String) return bad("strings need a word device (D, S, Z)");
  } else {
    auto word = num<std::uint32_t>(rest);
    if (!word || *word >= d->count) return bad(std::format("invalid {} number", d->name));
    a->start = *word;
    if (!sub.empty() && req.scalar != DataType::String) {
      auto b = num<int>(sub);
      if (!b || *b > 15) return bad("bit must be 0-15");
      if (req.scalar != DataType::Default && req.scalar != DataType::Boolean) return bad("bit addresses require type Boolean");
      if (req.is_array()) return bad("arrays of word bits are not supported");
      a->type = TagType{DataType::Boolean};
      a->bit = *b;
      a->points = 1;
      return a;
    }
    if (req.scalar == DataType::Boolean) return bad(std::format("use {}.0 .. .15 for a bit of a word", text));
  }

  if (req.scalar == DataType::String) {
    auto len = num<std::uint32_t>(sub);
    if (!len || *len == 0 || *len > 2 * kMaxTagWords) return bad("expected D100.<length> for String");
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
    if (a->points > kMaxTagWords) return bad("tag larger than 512 words");
  }
  if (a->start + a->points > d->count) return bad("range exceeds the device");
  return a;
}

std::string wire_address(const Device& d, bool bit_units, std::uint32_t start) {
  std::string w{d.name[0], d.name[1] ? d.name[1] : '0'};
  if (d.kind == Kind::Bit) w += bit_units ? std::format("{:05}{:X}", start / 16, start % 16) : std::format("{:05}0", start);
  else w += std::format("{:06}", start);
  return w;
}

// ---- values ---------------------------------------------------------------------

namespace {

using sdk::codec::ByteOrder;
constexpr ByteOrder kOrder = ByteOrder::CDAB;  // low word first, big-endian words

sdk::Result<Value> decode_scalar(DataType t, const std::uint8_t* p) {
  using sdk::codec::load;
  auto s = [&](std::size_t n) { return std::span<const std::uint8_t>(p, n); };
  switch (t) {
    case DataType::Short: return Value(load<std::int16_t>(s(2), kOrder));
    case DataType::Word: return Value(load<std::uint16_t>(s(2), kOrder));
    case DataType::Long: return Value(load<std::int32_t>(s(4), kOrder));
    case DataType::DWord: return Value(load<std::uint32_t>(s(4), kOrder));
    case DataType::LLong: return Value(load<std::int64_t>(s(8), kOrder));
    case DataType::QWord: return Value(load<std::uint64_t>(s(8), kOrder));
    case DataType::Float: return Value(load<float>(s(4), kOrder));
    case DataType::Double: return Value(load<double>(s(8), kOrder));
    case DataType::BCD: {
      auto v = sdk::codec::bcd_decode(load<std::uint16_t>(s(2), kOrder), 4);
      if (!v) return make_error(status::BadOutOfRange, "invalid BCD value");
      return Value(static_cast<std::uint16_t>(*v));
    }
    case DataType::LBCD: {
      auto v = sdk::codec::bcd_decode(load<std::uint32_t>(s(4), kOrder), 8);
      if (!v) return make_error(status::BadOutOfRange, "invalid LBCD value");
      return Value(*v);
    }
    default: return make_error(status::BadTypeMismatch, "unsupported type");
  }
}

sdk::StatusCode encode_scalar(DataType t, const Value& v, std::uint8_t* p) {
  using sdk::codec::store;
  auto c = sdk::convert(v, t);
  if (!c) return c.error().code;
  auto s = [&](std::size_t n) { return std::span<std::uint8_t>(p, n); };
  const Value& x = *c;
  switch (t) {
    case DataType::Short: store(*x.get_if<std::int16_t>(), s(2), kOrder); break;
    case DataType::Word: store(*x.get_if<std::uint16_t>(), s(2), kOrder); break;
    case DataType::Long: store(*x.get_if<std::int32_t>(), s(4), kOrder); break;
    case DataType::DWord: store(*x.get_if<std::uint32_t>(), s(4), kOrder); break;
    case DataType::LLong: store(*x.get_if<std::int64_t>(), s(8), kOrder); break;
    case DataType::QWord: store(*x.get_if<std::uint64_t>(), s(8), kOrder); break;
    case DataType::Float: store(*x.get_if<float>(), s(4), kOrder); break;
    case DataType::Double: store(*x.get_if<double>(), s(8), kOrder); break;
    case DataType::BCD: {
      auto b = sdk::codec::bcd_encode(*x.get_if<std::uint16_t>(), 4);
      if (!b) return status::BadOutOfRange;
      store(static_cast<std::uint16_t>(*b), s(2), kOrder);
      break;
    }
    case DataType::LBCD: {
      auto b = sdk::codec::bcd_encode(*x.get_if<std::uint32_t>(), 8);
      if (!b) return status::BadOutOfRange;
      store(*b, s(4), kOrder);
      break;
    }
    default: return status::BadTypeMismatch;
  }
  return status::Good;
}

}  // namespace

sdk::Result<Value> decode(const CimonAddress& a, std::span<const std::uint8_t> d) {
  if (a.bit_units) {
    if (d.size() < a.points) return make_error(status::BadInternalError, "short data");
    if (!a.type.is_array()) return Value(d[0] != 0);
    sdk::Array arr;
    for (std::uint32_t i = 0; i < a.points; ++i) arr.push_back(Value(d[i] != 0));
    return Value(std::move(arr));
  }
  if (d.size() < a.points * 2u) return make_error(status::BadInternalError, "short data");
  if (a.bit >= 0) return Value((((d[0] << 8) | d[1]) >> a.bit & 1) != 0);
  if (a.type.scalar == DataType::String) {
    std::string s;
    for (std::uint32_t i = 0; i < a.string_len && d[i] != 0; ++i) s.push_back(static_cast<char>(d[i]));
    return Value(std::move(s));
  }
  if (!a.type.is_array()) return decode_scalar(a.type.scalar, d.data());
  const std::size_t step = sdk::native_size(a.type.scalar);
  sdk::Array arr;
  for (std::uint32_t i = 0; i < a.type.element_count(); ++i) {
    auto v = decode_scalar(a.type.scalar, d.data() + i * step);
    if (!v) return v;
    arr.push_back(std::move(*v));
  }
  return Value(std::move(arr));
}

sdk::Result<std::vector<std::uint8_t>> encode(const CimonAddress& a, const Value& v) {
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
  if (!a.type.is_array()) {
    if (auto st = encode_scalar(a.type.scalar, v, out.data()); st != status::Good) return make_error(st, "cannot encode value");
    return out;
  }
  const std::size_t step = sdk::native_size(a.type.scalar);
  const auto* arr = v.get_if<sdk::Array>();
  if (!arr || arr->size() != a.type.element_count()) return make_error(status::BadTypeMismatch, "array size mismatch");
  for (std::size_t i = 0; i < arr->size(); ++i)
    if (auto st = encode_scalar(a.type.scalar, (*arr)[i], out.data() + i * step); st != status::Good)
      return make_error(st, "cannot encode array element");
  return out;
}

// ---- serial -----------------------------------------------------------------------

namespace {

constexpr std::uint8_t ENQ = 0x05, EOT = 0x04, STX = 0x02, ETX = 0x03;

void hex(std::string& o, std::uint32_t v, int digits) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (int i = digits - 1; i >= 0; --i) o.push_back(kHex[(v >> (4 * i)) & 0xF]);
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

std::uint8_t bcc(std::string_view s) {
  unsigned sum = 0;
  for (char c : s) sum += static_cast<std::uint8_t>(c);
  return static_cast<std::uint8_t>(sum);
}

std::vector<std::uint8_t> serial_frame(std::uint8_t station, char cmd, const std::string& data) {
  std::string body{cmd};
  hex(body, static_cast<std::uint32_t>(data.size()), 2);
  body += data;
  std::string f{static_cast<char>(ENQ)};
  hex(f, station, 2);
  f += body;
  hex(f, bcc(body), 2);
  f.push_back(static_cast<char>(EOT));
  return {f.begin(), f.end()};
}

}  // namespace

std::vector<std::uint8_t> serial_read(std::uint8_t station, const Device& d, bool bit_units, std::uint32_t start,
                                      std::uint32_t count) {
  std::string data = wire_address(d, bit_units, start);
  hex(data, count, 2);
  return serial_frame(station, bit_units ? 'r' : 'R', data);
}

std::vector<std::uint8_t> serial_write(std::uint8_t station, const Device& d, bool bit_units, std::uint32_t start,
                                       std::span<const std::uint8_t> raw) {
  std::string data = wire_address(d, bit_units, start);
  if (bit_units) {
    hex(data, static_cast<std::uint32_t>(raw.size()), 2);
    for (auto b : raw) data.push_back(b ? '1' : '0');
  } else {
    hex(data, static_cast<std::uint32_t>(raw.size() / 2), 2);
    for (std::size_t i = 0; i + 1 < raw.size(); i += 2) hex(data, static_cast<std::uint32_t>(raw[i] << 8 | raw[i + 1]), 4);
  }
  return serial_frame(station, bit_units ? 'w' : 'W', data);
}

sdk::Result<Answer> parse_serial(std::span<const std::uint8_t> f, std::uint8_t station, char command, bool bit_units,
                                 std::uint32_t count) {
  auto badf = [](const char* why) { return make_error(status::BadCommunicationError, why); };
  if (f.size() < 9 || f[0] != STX || f.back() != ETX) return badf("not an STX..ETX frame");
  auto st = parse_hex(f.subspan(1, 2));
  if (!st || *st != station) return badf("answer from another station");
  auto len = parse_hex(f.subspan(4, 2));
  if (!len || f.size() != 6 + *len + 3) return badf("bad length");
  const std::string_view body(reinterpret_cast<const char*>(f.data()) + 3, 3 + *len);
  auto sum = parse_hex(f.subspan(6 + *len, 2));
  if (!sum || *sum != bcc(body)) return badf("BCC mismatch");
  auto data = f.subspan(6, *len);
  Answer ans;
  if (f[3] == 'E') {
    auto code = num<int>(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
    ans.error = code && *code != 0 ? *code : -1;
    return ans;
  }
  if (f[3] != static_cast<std::uint8_t>(command)) return badf("answer to another command");
  if (command == 'W' || command == 'w') {
    if (!data.empty()) return badf("unexpected write answer data");
    return ans;
  }
  if (bit_units) {
    if (data.size() != count) return badf("bad bit count");
    for (auto c : data) {
      if (c != '0' && c != '1') return badf("bad bit data");
      ans.data.push_back(c == '1' ? 1 : 0);
    }
    return ans;
  }
  if (data.size() != count * 4) return badf("bad word count");
  for (std::size_t i = 0; i < data.size(); i += 4) {
    auto w = parse_hex(data.subspan(i, 4));
    if (!w) return badf("bad word data");
    ans.data.push_back(static_cast<std::uint8_t>(*w >> 8));
    ans.data.push_back(static_cast<std::uint8_t>(*w));
  }
  return ans;
}

sdk::StatusCode serial_error_status(int code) {
  switch (code) {
    case 1: return status::BadNotSupported;  // unknown command
    case 2: return status::BadCommunicationError;  // BCC error
    case 4:                                  // unknown device
    case 5:                                  // exceeds the device
    case 6: return status::BadOutOfRange;    // invalid address
    case 15: return status::BadNotWritable;  // write disabled
    default: return status::BadDeviceFailure;
  }
}

// ---- Ethernet ----------------------------------------------------------------------

namespace {

constexpr std::string_view kMasterId = "KDT_PLC_M", kSlaveId = "KDT_PLC_S";

void put16(std::vector<std::uint8_t>& o, std::uint32_t v) {
  o.push_back(static_cast<std::uint8_t>(v >> 8));
  o.push_back(static_cast<std::uint8_t>(v));
}

std::vector<std::uint8_t> enet_frame(std::uint8_t frame_no, std::uint8_t cmd, const std::vector<std::uint8_t>& data) {
  std::vector<std::uint8_t> f(kMasterId.begin(), kMasterId.end());
  f.push_back(frame_no);
  f.push_back(cmd);
  f.push_back(0x00);
  put16(f, static_cast<std::uint32_t>(data.size()));
  f.insert(f.end(), data.begin(), data.end());
  unsigned sum = 0;
  for (auto b : f) sum += b;
  put16(f, sum & 0xFFFF);
  return f;
}

std::vector<std::uint8_t> enet_head(const Device& d, bool bit_units, std::uint32_t start, std::uint32_t count) {
  const std::string w = wire_address(d, bit_units, start);
  std::vector<std::uint8_t> data(w.begin(), w.end());
  put16(data, count);
  return data;
}

}  // namespace

std::vector<std::uint8_t> enet_read(std::uint8_t frame_no, const Device& d, bool bit_units, std::uint32_t start,
                                    std::uint32_t count) {
  return enet_frame(frame_no, bit_units ? 0x72 : 0x52, enet_head(d, bit_units, start, count));
}

std::vector<std::uint8_t> enet_write(std::uint8_t frame_no, const Device& d, bool bit_units, std::uint32_t start,
                                     std::span<const std::uint8_t> raw) {
  const std::uint32_t count = static_cast<std::uint32_t>(bit_units ? raw.size() : raw.size() / 2);
  auto data = enet_head(d, bit_units, start, count);
  if (bit_units) {
    for (auto b : raw) data.push_back(b ? '1' : '0');
  } else {
    data.insert(data.end(), raw.begin(), raw.end());
  }
  return enet_frame(frame_no, bit_units ? 0x77 : 0x57, data);
}

sdk::Result<std::uint16_t> enet_length(std::span<const std::uint8_t> h) {
  if (h.size() < kEnetHeader || !std::equal(kSlaveId.begin(), kSlaveId.end(), h.begin()))
    return make_error(status::BadCommunicationError, "not a KDT_PLC_S frame");
  return static_cast<std::uint16_t>(h[12] << 8 | h[13]);
}

sdk::Result<Answer> parse_enet(std::span<const std::uint8_t> f, std::uint8_t frame_no, bool bit_units, std::uint32_t count,
                               bool is_write) {
  auto badf = [](const char* why) { return make_error(status::BadCommunicationError, why); };
  auto len = enet_length(f);
  if (!len) return std::unexpected(len.error());
  if (f.size() != kEnetHeader + *len + 2) return badf("bad frame length");
  unsigned sum = 0;
  for (std::size_t i = 0; i < f.size() - 2; ++i) sum += f[i];
  if ((sum & 0xFFFF) != static_cast<unsigned>(f[f.size() - 2] << 8 | f.back())) return badf("checksum mismatch");
  if (f[9] != static_cast<std::uint8_t>(frame_no + 128)) return badf("answer to another frame");
  auto data = f.subspan(kEnetHeader, *len);
  Answer ans;
  if (f[10] == 0x41) {  // ACK: error code (0 for a successful write)
    if (data.size() != 2) return badf("bad ACK length");
    ans.error = data[0] << 8 | data[1];
    if (ans.error == 0 && !is_write) ans.error = -1;  // a read must return data
    return ans;
  }
  if (is_write) return badf("unexpected answer to a write");
  if (f[10] != (bit_units ? 0x72 : 0x52) || data.size() < 10) return badf("answer to another command");
  data = data.subspan(10);  // block address echo
  if (bit_units) {
    if (data.size() != count) return badf("bad bit count");
    for (auto c : data) {
      if (c != '0' && c != '1') return badf("bad bit data");
      ans.data.push_back(c == '1' ? 1 : 0);
    }
    return ans;
  }
  if (data.size() != count * 2) return badf("bad word count");
  ans.data.assign(data.begin(), data.end());
  return ans;
}

sdk::StatusCode enet_error_status(int code) {
  switch (code) {
    case 0x02:  // invalid device prefix
    case 0x03:  // invalid device address
    case 0x04:  // read size
    case 0x0C: return status::BadOutOfRange;  // write size
    case 0x0E: return status::BadNotSupported;
    case 0x0F: return status::BadNotWritable;
    case 0x09:
    case 0x0A:
    case 0x0B: return status::BadCommunicationError;
    default: return status::BadDeviceFailure;
  }
}

}  // namespace logict::cimon
