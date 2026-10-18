#include "xgt_protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>

#include "logict/sdk/codec.h"

namespace logict::xgt {

namespace status = sdk::status;
using sdk::DataType;
using sdk::make_error;
using sdk::TagType;
using sdk::Value;

namespace {

std::string upper(std::string_view s) {
  std::string out;
  for (char c : s)
    if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  return out;
}

template <typename T>
std::optional<T> to_num(std::string_view s, int base = 10) {
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
  std::string_view dims = std::string_view(s).substr(pos);
  std::uint32_t* target = &rows;
  while (!dims.empty()) {
    if (dims.front() != '[' || !target) return false;
    auto close = dims.find(']');
    if (close == std::string_view::npos) return false;
    auto n = to_num<std::uint32_t>(dims.substr(1, close - 1));
    if (!n || *n == 0 || *n > 4096) return false;
    *target = *n;
    target = target == &rows ? &cols : nullptr;
    dims.remove_prefix(close + 1);
  }
  s.resize(pos);
  return true;
}

bool contains(std::initializer_list<std::string_view> set, std::string_view v) {
  return std::find(set.begin(), set.end(), v) != set.end();
}

// XGK device areas (word devices) and their capabilities.
// TODO(verify): continuous (byte block) support for R and ZR is not listed in
// the FEnet manual, so those are read with individual word requests.
constexpr std::initializer_list<std::string_view> kXgkAreas{"P", "M", "K", "F", "T", "C", "L", "N", "D", "R", "ZR"};
constexpr std::initializer_list<std::string_view> kXgkBlockAreas{"P", "M", "K", "F", "T", "C", "L", "N", "D"};
constexpr std::initializer_list<std::string_view> kXgkBitAreas{"P", "M", "K", "F", "L"};  // %?X per manual table 8.2.1
constexpr std::initializer_list<std::string_view> kXgiAreas{"I", "Q", "M", "R", "W", "F", "K", "L", "N", "A", "U"};
constexpr std::initializer_list<std::string_view> kXgiBlockAreas{"M", "R", "W", "F", "K", "L", "N", "A"};
constexpr std::initializer_list<std::string_view> kXgiBitAreas{"I", "Q", "M", "F", "K", "L", "N"};

std::uint32_t size_bytes(char size) {
  switch (size) {
    case 'X':
    case 'B': return 1;
    case 'W': return 2;
    case 'D': return 4;
    case 'L': return 8;
  }
  return 0;
}

std::optional<DataType> natural_type(char size) {
  switch (size) {
    case 'X': return DataType::Boolean;
    case 'B': return DataType::Byte;
    case 'W': return DataType::Word;
    case 'D': return DataType::DWord;
    case 'L': return DataType::QWord;
  }
  return std::nullopt;
}

using Result = sdk::Result<std::shared_ptr<XgtAddress>>;

Result bad(std::string msg) { return make_error(status::BadInvalidArgument, std::move(msg)); }

// Completes a word-addressed (XG5000 notation / non-bit direct) value item.
Result finish_value(std::shared_ptr<XgtAddress> a, TagType t, std::uint64_t byte_offset, bool block, char unit_size) {
  a->type = t;
  a->byte_offset = byte_offset;
  if (t.scalar == DataType::String) {
    a->byte_len = a->string_len + (a->string_len % 2);
  } else {
    a->byte_len = static_cast<std::uint32_t>(sdk::native_size(t.scalar) * t.element_count());
  }
  if (a->byte_len == 0) return bad("unsupported data type");
  a->block = block;
  if (!block) {
    // Individual reads: split the item into unit-sized variables of one type.
    const std::uint32_t unit = size_bytes(unit_size);
    if (a->byte_len % unit != 0 || byte_offset % unit != 0) return bad("value is not aligned to the device unit");
    const std::uint32_t n = a->byte_len / unit;
    if (n > kMaxBlocks) return bad(std::format("at most {} words per tag in area {}", kMaxBlocks, a->area));
    a->size = unit_size;
    a->var.clear();
  }
  if (a->byte_len > kMaxContinuousBytes) return bad("tag larger than 1400 bytes");
  return a;
}

// %<area><size><address>[.<sub>] for both series.
Result parse_direct(std::shared_ptr<XgtAddress> a, std::string s, TagType req, Series series) {
  s.erase(0, 1);  // '%'
  const auto& areas = series == Series::XGK ? kXgkAreas : kXgiAreas;
  std::string area = s.starts_with("ZR") ? "ZR" : s.substr(0, 1);
  if (!contains(areas, area)) return bad(std::format("unknown device area '{}'", area));
  s.erase(0, area.size());
  if (s.empty()) return bad("missing size letter (X, B, W, D, L)");
  const char size = s.front();
  if (size_bytes(size) == 0) return bad("size letter must be X, B, W, D or L");
  s.erase(0, 1);
  a->area = area;

  // I/Q/U with base.slot.point addressing: passed through as one variable.
  if ((area == "I" || area == "Q" || area == "U") && std::count(s.begin(), s.end(), '.') >= 2) {
    if (req.is_array()) return bad("arrays are not supported for I/Q/U addresses");
    for (char c : s)
      if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') return bad("invalid I/Q/U address");
    const DataType nat = *natural_type(size);
    DataType t = req.scalar == DataType::Default ? nat : req.scalar;
    if (sdk::native_size(t) != size_bytes(size) || (size == 'X') != (t == DataType::Boolean))
      return bad(std::format("type {} does not match size letter {}", sdk::to_string(t), size));
    a->type = TagType{t};
    a->block = false;
    a->size = size;
    a->var = std::format("%{}{}{}", area, size, s);
    a->bit_write_direct = size == 'X';
    a->byte_len = size_bytes(size);
    if (size == 'X') a->bit = 0;
    return a;
  }

  std::string num = s, sub;
  if (auto dot = s.find('.'); dot != std::string::npos) {
    num = s.substr(0, dot);
    sub = s.substr(dot + 1);
  }
  auto idx = to_num<std::uint64_t>(num);
  if (!idx || *idx > (1u << 24)) return bad("invalid address number");
  const auto& block_areas = series == Series::XGK ? kXgkBlockAreas : kXgiBlockAreas;
  const auto& bit_areas = series == Series::XGK ? kXgkBitAreas : kXgiBitAreas;
  const bool block = contains(block_areas, area);

  if (size == 'X' || (!sub.empty() && req.scalar != DataType::String)) {
    // A bit: %MX<n> or %MW<n>.<b> (decimal bit for XGI, hex for XGK like XG5000).
    std::uint64_t bit_index;
    if (size == 'X') {
      if (!sub.empty()) return bad("unexpected '.' after a bit address");
      bit_index = *idx;
    } else {
      auto b = to_num<unsigned>(sub, series == Series::XGK ? 16 : 10);
      if (!b || *b >= size_bytes(size) * 8) return bad("bit number out of range");
      bit_index = *idx * size_bytes(size) * 8 + *b;
    }
    if (req.scalar != DataType::Default && req.scalar != DataType::Boolean) return bad("bit addresses require type Boolean");
    a->type = TagType{DataType::Boolean, req.rows, req.cols};
    a->bit_index = bit_index;
    a->bit_write_direct = contains(bit_areas, area);
    a->size = 'X';
    a->var = std::format("%{}X{}", area, bit_index);
    if ((area == "T" || area == "C") && size == 'X') {
      // Timer/counter contacts are numbered per timer, not per word bit.
      if (req.is_array()) return bad("arrays of timer/counter contacts are not supported");
      a->block = false;
      a->bit_write_direct = true;
      a->bit = 0;
      a->byte_len = 1;
      return a;
    }
    a->byte_offset = bit_index / 8;
    a->bit = static_cast<int>(bit_index % 8);
    a->byte_len = static_cast<std::uint32_t>((a->bit + a->type.element_count() + 7) / 8);
    a->block = block;
    if (!block && (req.is_array() || !a->bit_write_direct))
      return bad(std::format("bit access in area {} is not supported", area));
    return a;
  }

  TagType t = req;
  if (t.scalar == DataType::String) {
    if (size != 'B' && size != 'W') return bad("strings need a byte or word address");
    auto len = to_num<std::uint32_t>(sub);
    if (!len || *len == 0 || *len > 255) return bad("string length must be 1..255");
    if (req.is_array()) return bad("string arrays are not supported");
    a->string_len = *len;
  } else {
    if (t.scalar == DataType::Default) t.scalar = *natural_type(size);
    if (sdk::native_size(t.scalar) != size_bytes(size))
      return bad(std::format("type {} does not match size letter {}", sdk::to_string(t.scalar), size));
  }
  const char unit = series == Series::XGK ? 'W' : size;
  return finish_value(std::move(a), t, *idx * size_bytes(size), block, unit);
}

// XG5000 device notation for XGK/XGB: D100, D100.A, M0010A (bit), T5, ZR100.
Result parse_xgk(std::shared_ptr<XgtAddress> a, std::string s, TagType req) {
  std::string area = s.starts_with("ZR") ? "ZR" : s.substr(0, 1);
  if (!contains(kXgkAreas, area)) return bad(std::format("unknown XGK device '{}'", area));
  s.erase(0, area.size());
  a->area = area;
  std::string num = s, sub;
  if (auto dot = s.find('.'); dot != std::string::npos) {
    num = s.substr(0, dot);
    sub = s.substr(dot + 1);
  }
  const bool want_bool = req.scalar == DataType::Boolean;

  if ((area == "T" || area == "C") && want_bool && sub.empty()) {
    auto n = to_num<std::uint32_t>(num);
    if (!n) return bad("invalid timer/counter number");
    return parse_direct(std::move(a), std::format("%{}X{}", area, *n), req, Series::XGK);
  }
  if (want_bool && sub.empty()) {
    // Compact bit form: word number followed by one hex bit digit (M0010A).
    if (!contains(kXgkBitAreas, area)) return bad(std::format("use {}<word>.<bit> for bits of {}", area, area));
    if (num.empty()) return bad("missing address");
    auto bit = to_num<unsigned>(std::string_view(num).substr(num.size() - 1), 16);
    auto word = num.size() == 1 ? std::optional<std::uint64_t>(0) : to_num<std::uint64_t>(std::string_view(num).substr(0, num.size() - 1));
    if (!bit || !word) return bad("invalid bit address (word number followed by a hex bit digit)");
    return parse_direct(std::move(a), std::format("%{}X{}", area, *word * 16 + *bit), req, Series::XGK);
  }
  auto word = to_num<std::uint64_t>(num);
  if (!word) return bad("invalid word number");
  if (!sub.empty() && req.scalar != DataType::String) {
    return parse_direct(std::move(a), std::format("%{}W{}.{}", area, *word, sub), req, Series::XGK);
  }
  if (area == "T" || area == "C") {
    if (req.scalar != DataType::Default && sdk::native_size(req.scalar) != 2)
      return bad("timer/counter current values are 16-bit");
  }
  TagType t = req;
  if (t.scalar == DataType::String) {
    auto len = to_num<std::uint32_t>(sub);
    if (!len || *len == 0 || *len > 255) return bad("string length must be 1..255 (e.g. D100.20)");
    if (req.is_array()) return bad("string arrays are not supported");
    a->string_len = *len;
  } else if (t.scalar == DataType::Default) {
    t.scalar = DataType::Word;
  }
  if (t.scalar == DataType::Date) return bad("Date is not supported");
  return finish_value(std::move(a), t, *word * 2, contains(kXgkBlockAreas, area), 'W');
}

}  // namespace

std::uint8_t cpu_info_byte(std::string_view model) {
  std::string m = upper(model);
  if (m.starts_with("XGR")) return 0xA8;
  if (m.starts_with("XGI") || m.starts_with("XEC")) return 0xA4;
  return 0xA0;
}

Series series_of(std::string_view model) {
  std::string m = upper(model);
  return m.starts_with("XGI") || m.starts_with("XGR") || m.starts_with("XEC") ? Series::XGI : Series::XGK;
}

sdk::Result<std::shared_ptr<XgtAddress>> parse_address(std::string_view text, TagType requested, Series series) {
  auto a = std::make_shared<XgtAddress>();
  a->text = std::string(text);
  std::string s = upper(text);
  std::uint32_t rows, cols;
  if (!split_dims(s, rows, cols)) return bad("invalid array dimensions");
  requested.rows = rows;
  requested.cols = cols;
  if (s.empty()) return bad("empty address");
  if (requested.scalar == DataType::Date) return bad("Date is not supported");
  Result r = s.front() == '%' ? parse_direct(a, s, requested, series)
             : series == Series::XGK ? parse_xgk(a, s, requested)
                                     : bad("XGI addresses use IEC notation, e.g. %MW100");
  if (!r) return r;
  for (const auto& v : unit_vars(**r))
    if (v.size() > kMaxVarLength) return bad("variable name longer than 16 characters");
  return r;
}

std::vector<std::string> unit_vars(const XgtAddress& a) {
  if (!a.var.empty()) return {a.var};
  std::vector<std::string> out;
  const std::uint32_t unit = size_bytes(a.size);
  for (std::uint32_t i = 0; i < a.byte_len / unit; ++i)
    out.push_back(std::format("%{}{}{}", a.area, a.size, a.byte_offset / unit + i));
  return out;
}

// ---- values ---------------------------------------------------------------------

namespace {

sdk::Result<Value> decode_scalar(DataType t, const std::uint8_t* p) {
  using sdk::codec::ByteOrder;
  using sdk::codec::load;
  auto span = [&](std::size_t n) { return std::span<const std::uint8_t>(p, n); };
  switch (t) {
    case DataType::Char: return Value(static_cast<std::int8_t>(p[0]));
    case DataType::Byte: return Value(p[0]);
    case DataType::Short: return Value(load<std::int16_t>(span(2), ByteOrder::DCBA));
    case DataType::Word: return Value(load<std::uint16_t>(span(2), ByteOrder::DCBA));
    case DataType::Long: return Value(load<std::int32_t>(span(4), ByteOrder::DCBA));
    case DataType::DWord: return Value(load<std::uint32_t>(span(4), ByteOrder::DCBA));
    case DataType::LLong: return Value(load<std::int64_t>(span(8), ByteOrder::DCBA));
    case DataType::QWord: return Value(load<std::uint64_t>(span(8), ByteOrder::DCBA));
    case DataType::Float: return Value(load<float>(span(4), ByteOrder::DCBA));
    case DataType::Double: return Value(load<double>(span(8), ByteOrder::DCBA));
    case DataType::BCD: {
      auto v = sdk::codec::bcd_decode(load<std::uint16_t>(span(2), ByteOrder::DCBA), 4);
      if (!v) return make_error(status::BadOutOfRange, "invalid BCD value");
      return Value(static_cast<std::uint16_t>(*v));
    }
    case DataType::LBCD: {
      auto v = sdk::codec::bcd_decode(load<std::uint32_t>(span(4), ByteOrder::DCBA), 8);
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
  auto span = [&](std::size_t n) { return std::span<std::uint8_t>(p, n); };
  const Value& x = *c;
  switch (t) {
    case DataType::Char: p[0] = static_cast<std::uint8_t>(*x.get_if<std::int8_t>()); break;
    case DataType::Byte: p[0] = *x.get_if<std::uint8_t>(); break;
    case DataType::Short: store(*x.get_if<std::int16_t>(), span(2), ByteOrder::DCBA); break;
    case DataType::Word: store(*x.get_if<std::uint16_t>(), span(2), ByteOrder::DCBA); break;
    case DataType::Long: store(*x.get_if<std::int32_t>(), span(4), ByteOrder::DCBA); break;
    case DataType::DWord: store(*x.get_if<std::uint32_t>(), span(4), ByteOrder::DCBA); break;
    case DataType::LLong: store(*x.get_if<std::int64_t>(), span(8), ByteOrder::DCBA); break;
    case DataType::QWord: store(*x.get_if<std::uint64_t>(), span(8), ByteOrder::DCBA); break;
    case DataType::Float: store(*x.get_if<float>(), span(4), ByteOrder::DCBA); break;
    case DataType::Double: store(*x.get_if<double>(), span(8), ByteOrder::DCBA); break;
    case DataType::BCD:
      store(static_cast<std::uint16_t>(*sdk::codec::bcd_encode(*x.get_if<std::uint16_t>(), 4)), span(2), ByteOrder::DCBA);
      break;
    case DataType::LBCD: store(*sdk::codec::bcd_encode(*x.get_if<std::uint32_t>(), 8), span(4), ByteOrder::DCBA); break;
    default: return status::BadTypeMismatch;
  }
  return status::Good;
}

}  // namespace

sdk::Result<Value> decode(const XgtAddress& a, std::span<const std::uint8_t> bytes) {
  if (bytes.size() < a.byte_len) return make_error(status::BadInternalError, "short data");
  if (a.type.scalar == DataType::Boolean) {
    auto bit = [&](std::uint32_t i) {
      const std::uint32_t b = static_cast<std::uint32_t>(a.bit) + i;
      return ((bytes[b / 8] >> (b % 8)) & 1) != 0;
    };
    if (!a.type.is_array()) return Value(bit(0));
    sdk::Array arr;
    for (std::uint32_t i = 0; i < a.type.element_count(); ++i) arr.push_back(Value(bit(i)));
    return Value(std::move(arr));
  }
  if (a.type.scalar == DataType::String) {
    std::string s;
    for (std::uint32_t i = 0; i < a.string_len && bytes[i] != 0; ++i) s.push_back(static_cast<char>(bytes[i]));
    return Value(std::move(s));
  }
  const std::size_t step = sdk::native_size(a.type.scalar);
  if (!a.type.is_array()) return decode_scalar(a.type.scalar, bytes.data());
  sdk::Array arr;
  for (std::uint32_t i = 0; i < a.type.element_count(); ++i) {
    auto v = decode_scalar(a.type.scalar, bytes.data() + i * step);
    if (!v) return v;
    arr.push_back(std::move(*v));
  }
  return Value(std::move(arr));
}

sdk::Result<std::vector<std::uint8_t>> encode(const XgtAddress& a, const Value& v) {
  std::vector<std::uint8_t> out(a.byte_len, 0);
  if (a.type.scalar == DataType::Boolean) return make_error(status::BadInternalError, "bits are written individually");
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

// ---- frames -----------------------------------------------------------------------

std::uint16_t type_code(char size) {
  switch (size) {
    case 'X': return kTypeBit;
    case 'B': return kTypeByte;
    case 'W': return kTypeWord;
    case 'D': return kTypeDWord;
    case 'L': return kTypeLWord;
  }
  return kTypeWord;
}

namespace {
void put16(std::vector<std::uint8_t>& o, std::uint16_t v) {
  o.push_back(static_cast<std::uint8_t>(v));
  o.push_back(static_cast<std::uint8_t>(v >> 8));
}
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
void put_var(std::vector<std::uint8_t>& o, std::string_view var) {
  put16(o, static_cast<std::uint16_t>(var.size()));
  o.insert(o.end(), var.begin(), var.end());
}
}  // namespace

std::vector<std::uint8_t> frame(const Header& h, std::span<const std::uint8_t> body) {
  std::vector<std::uint8_t> f{'L', 'S', 'I', 'S', '-', 'X', 'G', 'T', 0, 0};  // company id + reserved
  put16(f, 0);                    // PLC info: don't care from client
  f.push_back(h.cpu_info);
  f.push_back(0x33);              // source of frame: client -> server
  put16(f, h.invoke_id);
  put16(f, static_cast<std::uint16_t>(body.size()));
  f.push_back(h.fenet_position);
  std::uint8_t bcc = 0;
  for (auto b : f) bcc = static_cast<std::uint8_t>(bcc + b);
  f.push_back(bcc);
  f.insert(f.end(), body.begin(), body.end());
  return f;
}

std::vector<std::uint8_t> read_individual(std::uint16_t type, std::span<const std::string> vars) {
  std::vector<std::uint8_t> b;
  put16(b, kReadRequest);
  put16(b, type);
  put16(b, 0);
  put16(b, static_cast<std::uint16_t>(vars.size()));
  for (const auto& v : vars) put_var(b, v);
  return b;
}

std::vector<std::uint8_t> read_continuous(std::string_view var, std::uint16_t bytes) {
  std::vector<std::uint8_t> b;
  put16(b, kReadRequest);
  put16(b, kTypeContinuous);
  put16(b, 0);
  put16(b, 1);
  put_var(b, var);
  put16(b, bytes);
  return b;
}

std::vector<std::uint8_t> write_individual(std::uint16_t type, std::span<const std::string> vars,
                                           std::span<const std::vector<std::uint8_t>> data) {
  std::vector<std::uint8_t> b;
  put16(b, kWriteRequest);
  put16(b, type);
  put16(b, 0);
  put16(b, static_cast<std::uint16_t>(vars.size()));
  for (const auto& v : vars) put_var(b, v);
  for (const auto& d : data) {
    put16(b, static_cast<std::uint16_t>(d.size()));
    b.insert(b.end(), d.begin(), d.end());
  }
  return b;
}

std::vector<std::uint8_t> write_continuous(std::string_view var, std::span<const std::uint8_t> data) {
  std::vector<std::uint8_t> b;
  put16(b, kWriteRequest);
  put16(b, kTypeContinuous);
  put16(b, 0);
  put16(b, 1);
  put_var(b, var);
  put16(b, static_cast<std::uint16_t>(data.size()));
  b.insert(b.end(), data.begin(), data.end());
  return b;
}

sdk::Result<ParsedHeader> parse_header(std::span<const std::uint8_t> h) {
  if (h.size() < kHeaderSize) return make_error(status::BadCommunicationError, "short header");
  if (std::memcmp(h.data(), "LSIS-XGT", 8) != 0 && std::memcmp(h.data(), "LGIS-GLOF", 9) != 0)
    return make_error(status::BadCommunicationError, "bad company id");
  if (h[13] != 0x11) return make_error(status::BadCommunicationError, "frame is not a server response");
  ParsedHeader p;
  p.plc_info = get16(&h[10]);
  p.invoke_id = get16(&h[14]);
  p.length = get16(&h[16]);
  return p;
}

sdk::Result<Response> parse_body(std::span<const std::uint8_t> b, bool has_data) {
  auto truncated = [] { return make_error(status::BadCommunicationError, "truncated response"); };
  if (b.size() < 8) return truncated();
  Response r;
  r.command = get16(&b[0]);
  r.type = get16(&b[2]);
  r.error_status = get16(&b[6]);
  if (r.nak()) {
    r.error_code = b.size() > 8 ? b[8] : 0;
    return r;
  }
  if (b.size() < 10) return truncated();
  r.blocks = get16(&b[8]);
  if (!has_data) return r;
  std::size_t pos = 10;
  for (std::uint16_t i = 0; i < r.blocks; ++i) {
    if (pos + 2 > b.size()) return truncated();
    const std::uint16_t n = get16(&b[pos]);
    pos += 2;
    if (pos + n > b.size()) return truncated();
    r.data.emplace_back(b.begin() + static_cast<std::ptrdiff_t>(pos), b.begin() + static_cast<std::ptrdiff_t>(pos + n));
    pos += n;
  }
  return r;
}

}  // namespace logict::xgt
