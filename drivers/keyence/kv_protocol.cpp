#include "kv_protocol.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>
#include <optional>

#include "logict/sdk/codec.h"

namespace logict::kv {

namespace status = sdk::status;
using sdk::DataType;
using sdk::make_error;
using sdk::TagType;
using sdk::Value;

namespace {

// Two-letter names first so that e.g. "DM" wins over the XYM alias "D".
constexpr std::array<Device, 27> kDevices{{
    {"DM", Kind::Word, Notation::Decimal, 65535},  {"EM", Kind::Word, Notation::Decimal, 65535},
    {"FM", Kind::Word, Notation::Decimal, 32768},  {"ZF", Kind::Word, Notation::Decimal, 524288},
    {"TM", Kind::Word, Notation::Decimal, 512},    {"CM", Kind::Word, Notation::Decimal, 7600},
    {"VM", Kind::Word, Notation::Decimal, 589824}, {"VB", Kind::Bit, Notation::Hex, 0xFA00},
    {"MR", Kind::Bit, Notation::Bank, 64000},      {"LR", Kind::Bit, Notation::Bank, 16000},
    {"CR", Kind::Bit, Notation::Bank, 1280},       {"TC", Kind::Dword, Notation::Decimal, 4000},
    {"TS", Kind::Dword, Notation::Decimal, 4000},  {"CC", Kind::Dword, Notation::Decimal, 4000},
    {"CS", Kind::Dword, Notation::Decimal, 4000},  {"R", Kind::Bit, Notation::Bank, 32000},
    {"B", Kind::Bit, Notation::Hex, 0x8000},       {"W", Kind::Word, Notation::Hex, 0x8000},
    {"X", Kind::Bit, Notation::Xy, 32000},         {"Y", Kind::Bit, Notation::Xy, 32000},
    {"M", Kind::Bit, Notation::Decimal, 64000},    {"L", Kind::Bit, Notation::Decimal, 16000},
    {"D", Kind::Word, Notation::Decimal, 65535},   {"E", Kind::Word, Notation::Decimal, 65535},
    {"F", Kind::Word, Notation::Decimal, 32768},    {"T", Kind::Contact, Notation::Decimal, 4000},
    {"C", Kind::Contact, Notation::Decimal, 4000},
}};

using Result = sdk::Result<std::shared_ptr<KvAddress>>;
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

// Device number text -> point index.
std::optional<std::uint32_t> point_index(const Device& d, std::string_view t) {
  switch (d.notation) {
    case Notation::Decimal: return num<std::uint32_t>(t);
    case Notation::Hex: return num<std::uint32_t>(t, 16);
    case Notation::Bank: {
      auto n = num<std::uint32_t>(t);
      if (!n || *n % 100 > 15) return std::nullopt;
      return *n / 100 * 16 + *n % 100;
    }
    case Notation::Xy: {
      if (t.empty()) return std::nullopt;
      auto bank = t.size() == 1 ? std::optional<std::uint32_t>(0) : num<std::uint32_t>(t.substr(0, t.size() - 1));
      auto bit = num<std::uint32_t>(t.substr(t.size() - 1), 16);
      if (!bank || !bit) return std::nullopt;
      return *bank * 16 + *bit;
    }
  }
  return std::nullopt;
}

}  // namespace

const Device* find_device(std::string_view name) {
  for (const auto& d : kDevices)
    if (name.starts_with(d.name)) return &d;
  return nullptr;
}

std::uint32_t device_index(const Device& d) { return static_cast<std::uint32_t>(&d - kDevices.data()); }
const Device& device_at(std::uint32_t index) { return kDevices.at(index); }

std::uint32_t max_points(Mode mode) { return mode == Mode::Dwords ? kMaxTimers : mode == Mode::Contact ? 1 : kMaxPoints; }

sdk::Result<std::shared_ptr<KvAddress>> parse_address(std::string_view text, TagType requested) {
  auto a = std::make_shared<KvAddress>();
  a->text = std::string(text);
  std::string s;
  for (char c : text)
    if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  std::uint32_t rows, cols;
  if (!split_dims(s, rows, cols)) return bad("invalid array dimensions");
  const Device* d = find_device(s);
  if (!d) return bad("unknown device (DM, EM, FM, ZF, W, TM, CM, VM, R, MR, LR, CR, B, VB, X, Y, M, L, T, C, TC, TS, CC, CS)");
  a->device = d;
  std::string_view rest = std::string_view(s).substr(std::strlen(d->name)), sub;
  if (auto dot = rest.find('.'); dot != std::string_view::npos) {
    sub = rest.substr(dot + 1);
    rest = rest.substr(0, dot);
  }
  auto index = point_index(*d, rest);
  if (!index || *index >= d->count) return bad(std::format("invalid device number for {}", d->name));
  const TagType req{requested.scalar, rows, cols};

  switch (d->kind) {
    case Kind::Contact: {
      if (!sub.empty() || req.is_array()) return bad("timer/counter contacts are single Boolean points");
      if (req.scalar != DataType::Default && req.scalar != DataType::Boolean)
        return bad(std::format("use {}C / {}S for the current / preset value", d->name, d->name));
      a->type = TagType{DataType::Boolean};
      a->mode = Mode::Contact;
      a->start = *index;
      a->points = 1;
      return a;
    }
    case Kind::Dword: {
      if (!sub.empty()) return bad("no sub-address on timer/counter values");
      TagType t = req;
      if (t.scalar == DataType::Default) t.scalar = DataType::DWord;
      if (t.scalar != DataType::DWord && t.scalar != DataType::Long) return bad("timer/counter values are DWord or Long");
      a->type = t;
      a->mode = Mode::Dwords;
      a->start = *index;
      a->points = t.element_count();
      if (a->points > kMaxTimers || *index + a->points > d->count) return bad("range exceeds the device");
      return a;
    }
    case Kind::Bit: {
      if (!sub.empty()) return bad("bit devices have no sub-address");
      if (req.scalar == DataType::Default || req.scalar == DataType::Boolean) {
        a->type = TagType{DataType::Boolean, rows, cols};
        a->mode = Mode::Bits;
        a->start = *index;
        a->points = a->type.element_count();
        if (a->points > kMaxPoints || *index + a->points > d->count) return bad("range exceeds the device");
        return a;
      }
      if (*index % 16 != 0) return bad("word access to a bit device must start at bit 0 of a channel");
      a->start = *index / 16;
      break;
    }
    case Kind::Word: {
      a->start = *index;
      if (!sub.empty() && req.scalar != DataType::String) {
        auto b = num<int>(sub);
        if (!b || *b > 15) return bad("bit must be 0-15");
        if (req.scalar != DataType::Default && req.scalar != DataType::Boolean) return bad("bit addresses require type Boolean");
        if (req.is_array()) return bad("arrays of word bits are not supported");
        a->type = TagType{DataType::Boolean};
        a->mode = Mode::Words;
        a->bit = *b;
        a->points = 1;
        return a;
      }
      if (req.scalar == DataType::Boolean) return bad(std::format("use {}.0 .. .15 for a bit of a word", text));
      break;
    }
  }

  // Word access (word devices, or bit devices by 16 points).
  a->mode = Mode::Words;
  const std::uint32_t words_in_device = d->kind == Kind::Bit ? d->count / 16 : d->count;
  if (req.scalar == DataType::String) {
    auto len = num<std::uint32_t>(sub);
    if (!len || *len == 0 || *len > 2 * kMaxPoints) return bad("expected DM100.<length> for String");
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
    if (a->points > kMaxPoints) return bad("tag larger than 1000 words");
  }
  if (a->start + a->points > words_in_device) return bad("range exceeds the device");
  return a;
}

std::string device_text(const Device& d, Mode mode, std::uint32_t start) {
  const std::uint32_t point = mode == Mode::Words && d.kind == Kind::Bit ? start * 16 : start;
  switch (d.notation) {
    case Notation::Decimal: return std::format("{}{}", d.name, point);
    case Notation::Hex: return std::format("{}{:X}", d.name, point);
    case Notation::Bank: return std::format("{}{}{:02}", d.name, point / 16, point % 16);
    case Notation::Xy: return std::format("{}{}{:X}", d.name, point / 16, point % 16);
  }
  return {};
}

namespace {
const char* suffix(Mode mode) {
  switch (mode) {
    case Mode::Bits: return "";
    case Mode::Words: return ".H";
    case Mode::Dwords:
    case Mode::Contact: return ".D";
  }
  return "";
}
}  // namespace

std::string read_command(const Device& d, Mode mode, std::uint32_t start, std::uint32_t count) {
  if (mode == Mode::Contact) return std::format("RD {}{}\r", device_text(d, mode, start), suffix(mode));
  return std::format("RDS {}{} {}\r", device_text(d, mode, start), suffix(mode), count);
}

std::string write_command(const Device& d, Mode mode, std::uint32_t start, std::span<const std::string> values) {
  std::string cmd = std::format("WRS {}{} {}", device_text(d, mode, start), suffix(mode), values.size());
  for (const auto& v : values) {
    cmd += ' ';
    cmd += v;
  }
  cmd += '\r';
  return cmd;
}

std::string format_token(Mode mode, std::uint32_t raw) {
  switch (mode) {
    case Mode::Bits:
    case Mode::Contact: return raw ? "1" : "0";
    case Mode::Words: return std::format("{:04X}", raw & 0xFFFF);
    case Mode::Dwords: return std::format("{}", raw);
  }
  return {};
}

sdk::Result<Answer> parse_read_answer(std::string_view line, Mode mode, std::uint32_t count) {
  Answer ans;
  if (line.size() == 2 && line[0] == 'E' && std::isdigit(static_cast<unsigned char>(line[1]))) {
    ans.error = std::string(line);
    return ans;
  }
  std::vector<std::string_view> tokens;
  for (std::size_t pos = 0; pos < line.size();) {
    while (pos < line.size() && line[pos] == ' ') ++pos;
    std::size_t end = line.find(' ', pos);
    if (end == std::string_view::npos) end = line.size();
    if (end > pos) tokens.push_back(line.substr(pos, end - pos));
    pos = end;
  }
  auto bad_answer = [&] { return make_error(status::BadCommunicationError, std::format("unexpected answer '{}'", line)); };
  if (mode == Mode::Contact) {
    // "contact,current,preset" (fields may also be space separated).
    if (tokens.empty()) return bad_answer();
    auto c = num<std::uint32_t>(tokens[0].substr(0, tokens[0].find(',')));
    if (!c || *c > 1) return bad_answer();
    ans.values.push_back(*c);
    return ans;
  }
  if (tokens.size() != count) return bad_answer();
  for (auto t : tokens) {
    std::optional<std::uint32_t> v;
    if (mode == Mode::Words) v = t.size() <= 4 ? num<std::uint32_t>(t, 16) : std::nullopt;
    else v = num<std::uint32_t>(t);
    if (!v || (mode == Mode::Bits && *v > 1)) return bad_answer();
    ans.values.push_back(*v);
  }
  return ans;
}

sdk::StatusCode error_status(std::string_view code) {
  if (code == "E0") return status::BadOutOfRange;    // abnormal device number
  if (code == "E1") return status::BadNotSupported;  // abnormal command
  if (code == "E4") return status::BadNotWritable;   // write disabled
  return status::BadDeviceFailure;                   // E2 program not registered, E5 unit error, ...
}

// ---- values ---------------------------------------------------------------------

namespace {

using sdk::codec::ByteOrder;
constexpr ByteOrder kOrder = ByteOrder::CDAB;  // low word first

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

Value bools(std::span<const std::uint32_t> raw, bool array) {
  if (!array) return Value(raw[0] != 0);
  sdk::Array arr;
  for (auto r : raw) arr.push_back(Value(r != 0));
  return Value(std::move(arr));
}

}  // namespace

sdk::Result<Value> decode(const KvAddress& a, std::span<const std::uint32_t> raw) {
  if (raw.size() < a.points) return make_error(status::BadInternalError, "short data");
  raw = raw.first(a.points);
  switch (a.mode) {
    case Mode::Bits:
    case Mode::Contact: return bools(raw, a.type.is_array());
    case Mode::Dwords: {
      auto one = [&](std::uint32_t r) {
        return a.type.scalar == DataType::Long ? Value(static_cast<std::int32_t>(r)) : Value(r);
      };
      if (!a.type.is_array()) return one(raw[0]);
      sdk::Array arr;
      for (auto r : raw) arr.push_back(one(r));
      return Value(std::move(arr));
    }
    case Mode::Words: break;
  }
  if (a.bit >= 0) return Value(((raw[0] >> a.bit) & 1) != 0);
  std::vector<std::uint8_t> d;
  for (auto w : raw) {
    d.push_back(static_cast<std::uint8_t>(w >> 8));
    d.push_back(static_cast<std::uint8_t>(w));
  }
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

sdk::Result<std::vector<std::uint32_t>> encode(const KvAddress& a, const Value& v) {
  std::vector<std::uint32_t> out;
  switch (a.mode) {
    case Mode::Contact: return make_error(status::BadNotWritable, "timer/counter contacts are read-only");
    case Mode::Bits: {
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
    case Mode::Dwords: {
      auto one = [&](const Value& e) -> sdk::StatusCode {
        if (a.type.scalar == DataType::Long) {
          auto c = sdk::convert(e, DataType::Long);
          if (!c) return c.error().code;
          out.push_back(static_cast<std::uint32_t>(*c->get_if<std::int32_t>()));
        } else {
          auto c = sdk::convert(e, DataType::DWord);
          if (!c) return c.error().code;
          out.push_back(*c->get_if<std::uint32_t>());
        }
        return status::Good;
      };
      if (const auto* arr = v.get_if<sdk::Array>()) {
        if (arr->size() != a.points) return make_error(status::BadTypeMismatch, "array size mismatch");
        for (const auto& e : *arr)
          if (auto st = one(e); st != status::Good) return make_error(st, "cannot convert value");
      } else if (auto st = one(v); st != status::Good) {
        return make_error(st, "cannot convert value");
      }
      return out;
    }
    case Mode::Words: break;
  }
  if (a.bit >= 0) return make_error(status::BadInternalError, "word bits are written by read-modify-write");
  std::vector<std::uint8_t> d(a.points * 2u, 0);
  if (a.type.scalar == DataType::String) {
    auto s = sdk::convert(v, DataType::String);
    if (!s) return std::unexpected(s.error());
    const auto& str = *s->get_if<std::string>();
    if (str.size() > a.string_len) return make_error(status::BadOutOfRange, "string longer than the tag length");
    std::memcpy(d.data(), str.data(), str.size());
  } else if (!a.type.is_array()) {
    if (auto st = encode_scalar(a.type.scalar, v, d.data()); st != status::Good) return make_error(st, "cannot encode value");
  } else {
    const std::size_t step = sdk::native_size(a.type.scalar);
    const auto* arr = v.get_if<sdk::Array>();
    if (!arr || arr->size() != a.type.element_count()) return make_error(status::BadTypeMismatch, "array size mismatch");
    for (std::size_t i = 0; i < arr->size(); ++i)
      if (auto st = encode_scalar(a.type.scalar, (*arr)[i], d.data() + i * step); st != status::Good)
        return make_error(st, "cannot encode array element");
  }
  for (std::size_t i = 0; i < d.size(); i += 2) out.push_back(static_cast<std::uint32_t>(d[i] << 8 | d[i + 1]));
  return out;
}

}  // namespace logict::kv
