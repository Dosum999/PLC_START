#include "protocol.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>

#include "logict/sdk/codec.h"

namespace logict::modbus {

namespace status = sdk::status;
using sdk::DataType;
using sdk::make_error;
using sdk::Value;

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::optional<std::uint32_t> to_uint(std::string_view s) {
  std::uint32_t v;
  auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || s.empty()) return std::nullopt;
  return v;
}

bool split_dims(std::string_view& s, std::uint32_t& rows, std::uint32_t& cols) {
  rows = cols = 0;
  auto pos = s.find('[');
  if (pos == std::string_view::npos) return true;
  std::string_view dims = trim(s.substr(pos));
  s = trim(s.substr(0, pos));
  std::uint32_t* target = &rows;
  while (!dims.empty()) {
    if (dims.front() != '[' || !target) return false;
    auto close = dims.find(']');
    if (close == std::string_view::npos) return false;
    auto n = to_uint(trim(dims.substr(1, close - 1)));
    if (!n || *n == 0 || *n > 65536) return false;
    *target = *n;
    target = target == &rows ? &cols : nullptr;
    dims = trim(dims.substr(close + 1));
  }
  return true;
}

std::uint32_t registers_per_element(DataType t) {
  switch (t) {
    case DataType::Word:
    case DataType::Short:
    case DataType::BCD: return 1;
    case DataType::DWord:
    case DataType::Long:
    case DataType::Float:
    case DataType::LBCD: return 2;
    case DataType::Double:
    case DataType::LLong:
    case DataType::QWord: return 4;
    default: return 0;
  }
}

// Wire bytes of one value -> big-endian canonical order (and back; the
// transformation is its own inverse).
void canonicalize(std::uint8_t* p, std::size_t n, const Encoding& enc) {
  if (!enc.modbus_byte_order)
    for (std::size_t i = 0; i + 1 < n; i += 2) std::swap(p[i], p[i + 1]);
  if (n >= 4 && enc.first_word_low)
    for (std::size_t d = 0; d + 3 < n; d += 4) {
      std::swap(p[d], p[d + 2]);
      std::swap(p[d + 1], p[d + 3]);
    }
  if (n == 8 && enc.first_dword_low)
    for (std::size_t i = 0; i < 4; ++i) std::swap(p[i], p[i + 4]);
}

template <typename T>
T load(const std::uint8_t* wire, const Encoding& enc) {
  std::uint8_t buf[sizeof(T)];
  std::memcpy(buf, wire, sizeof(T));
  canonicalize(buf, sizeof(T), enc);
  return sdk::codec::load<T>(std::span<const std::uint8_t>(buf, sizeof(T)), sdk::codec::ByteOrder::ABCD);
}

template <typename T>
void store(T v, std::uint8_t* wire, const Encoding& enc) {
  sdk::codec::store<T>(v, std::span<std::uint8_t>(wire, sizeof(T)), sdk::codec::ByteOrder::ABCD);
  canonicalize(wire, sizeof(T), enc);
}

sdk::Result<Value> decode_scalar(DataType t, const std::uint8_t* p, const Encoding& enc) {
  switch (t) {
    case DataType::Word: return Value(load<std::uint16_t>(p, enc));
    case DataType::Short: return Value(load<std::int16_t>(p, enc));
    case DataType::DWord: return Value(load<std::uint32_t>(p, enc));
    case DataType::Long: return Value(load<std::int32_t>(p, enc));
    case DataType::Float: return Value(load<float>(p, enc));
    case DataType::Double: return Value(load<double>(p, enc));
    case DataType::LLong: return Value(load<std::int64_t>(p, enc));
    case DataType::QWord: return Value(load<std::uint64_t>(p, enc));
    case DataType::BCD: {
      auto v = sdk::codec::bcd_decode(load<std::uint16_t>(p, enc), 4);
      if (!v) return make_error(status::BadOutOfRange, "invalid BCD value");
      return Value(static_cast<std::uint16_t>(*v));
    }
    case DataType::LBCD: {
      auto v = sdk::codec::bcd_decode(load<std::uint32_t>(p, enc), 8);
      if (!v) return make_error(status::BadOutOfRange, "invalid LBCD value");
      return Value(*v);
    }
    default: return make_error(status::BadTypeMismatch, "unsupported register type");
  }
}

sdk::StatusCode encode_scalar(DataType t, const Value& v, std::uint8_t* p, const Encoding& enc) {
  auto c = sdk::convert(v, t);
  if (!c) return c.error().code;
  const Value& x = *c;
  switch (t) {
    case DataType::Word: store(*x.get_if<std::uint16_t>(), p, enc); break;
    case DataType::Short: store(*x.get_if<std::int16_t>(), p, enc); break;
    case DataType::DWord: store(*x.get_if<std::uint32_t>(), p, enc); break;
    case DataType::Long: store(*x.get_if<std::int32_t>(), p, enc); break;
    case DataType::Float: store(*x.get_if<float>(), p, enc); break;
    case DataType::Double: store(*x.get_if<double>(), p, enc); break;
    case DataType::LLong: store(*x.get_if<std::int64_t>(), p, enc); break;
    case DataType::QWord: store(*x.get_if<std::uint64_t>(), p, enc); break;
    case DataType::BCD: store(static_cast<std::uint16_t>(*sdk::codec::bcd_encode(*x.get_if<std::uint16_t>(), 4)), p, enc); break;
    case DataType::LBCD: store(*sdk::codec::bcd_encode(*x.get_if<std::uint32_t>(), 8), p, enc); break;
    default: return status::BadTypeMismatch;
  }
  return status::Good;
}

}  // namespace

std::uint32_t ModbusAddress::units() const {
  const std::uint32_t n = type.element_count();
  if (is_bit_area(area) || bit >= 0) return n;
  if (type.scalar == DataType::String) return (string_len + 1) / 2;
  return registers_per_element(type.scalar) * n;
}

sdk::Result<std::shared_ptr<ModbusAddress>> parse_address(std::string_view text, sdk::TagType requested,
                                                          const AddressOptions& opt) {
  auto a = std::make_shared<ModbusAddress>();
  a->text = std::string(text);
  std::string_view s = trim(text);
  std::uint32_t rows, cols;
  if (!split_dims(s, rows, cols)) return make_error(status::BadInvalidArgument, "invalid array dimensions");

  std::string_view num = s, suffix;
  if (auto dot = s.find('.'); dot != std::string_view::npos) {
    num = s.substr(0, dot);
    suffix = s.substr(dot + 1);
  }
  if (num.size() != 5 && num.size() != 6) return make_error(status::BadInvalidArgument, "expected a 5 or 6 digit Modbus address");
  switch (num.front()) {
    case '0': a->area = Area::Coil; break;
    case '1': a->area = Area::DiscreteInput; break;
    case '3': a->area = Area::InputRegister; break;
    case '4': a->area = Area::HoldingRegister; break;
    default: return make_error(status::BadInvalidArgument, "address must start with 0, 1, 3 or 4");
  }
  auto n = to_uint(num.substr(1));
  const std::uint32_t max = num.size() == 6 ? 65536 : 9999;
  if (!n || *n < 1 || *n > max) return make_error(status::BadOutOfRange, std::format("register out of range 1..{}", max));
  a->offset = opt.zero_based ? *n - 1 : *n;
  if (a->offset > 65535) return make_error(status::BadOutOfRange, "register out of range");

  DataType t = requested.scalar;
  if (!suffix.empty()) {
    if (is_bit_area(a->area)) return make_error(status::BadInvalidArgument, "coils have no sub-address");
    const char last = static_cast<char>(std::toupper(static_cast<unsigned char>(suffix.back())));
    if (last == 'H' || last == 'L' || t == DataType::String) {
      auto len = to_uint(last == 'H' || last == 'L' ? suffix.substr(0, suffix.size() - 1) : suffix);
      if (!len || *len < 1 || *len > 250) return make_error(status::BadInvalidArgument, "string length must be 1..250");
      if (t != DataType::Default && t != DataType::String)
        return make_error(status::BadTypeMismatch, "string addresses require type String");
      a->string_len = *len;
      a->string_high_first = last != 'L';
      t = DataType::String;
    } else {
      auto b = to_uint(suffix);
      const int lo = opt.zero_based_bits ? 0 : 1;
      if (!b || static_cast<int>(*b) < lo || static_cast<int>(*b) > lo + 15)
        return make_error(status::BadInvalidArgument, std::format("bit must be {}..{}", lo, lo + 15));
      if (t != DataType::Default && t != DataType::Boolean)
        return make_error(status::BadTypeMismatch, "bit addresses require type Boolean");
      a->bit = static_cast<int>(*b) - lo;
      t = DataType::Boolean;
    }
  }

  if (is_bit_area(a->area)) {
    if (t == DataType::Default) t = DataType::Boolean;
    if (t != DataType::Boolean) return make_error(status::BadTypeMismatch, "coils and discrete inputs are Boolean");
  } else if (a->bit < 0 && t != DataType::String) {
    if (t == DataType::Default) t = DataType::Word;
    if (registers_per_element(t) == 0)
      return make_error(status::BadTypeMismatch, std::format("type {} is not supported for registers", sdk::to_string(t)));
  }
  if ((rows > 0) && (a->bit >= 0 || t == DataType::String))
    return make_error(status::BadInvalidArgument, "arrays of bits or strings are not supported");
  a->type = sdk::TagType{t, rows, cols};
  if (std::uint64_t{a->offset} + a->units() > 65536) return make_error(status::BadOutOfRange, "address range exceeds 65536");
  return a;
}

sdk::Result<Value> decode_registers(const ModbusAddress& a, std::span<const std::uint8_t> wire, const Encoding& enc) {
  if (wire.size() < a.units() * 2) return make_error(status::BadInternalError, "short register data");
  if (a.bit >= 0) {
    std::uint16_t reg = load<std::uint16_t>(wire.data(), enc);
    return Value(((reg >> a.bit) & 1) != 0);
  }
  if (a.type.scalar == DataType::String) {
    std::string s;
    for (std::uint32_t i = 0; i < a.string_len; ++i) {
      std::uint8_t hi = wire[(i / 2) * 2], lo = wire[(i / 2) * 2 + 1];
      if (!enc.modbus_byte_order) std::swap(hi, lo);
      char c = static_cast<char>((i % 2 == 0) == a.string_high_first ? hi : lo);
      if (c == '\0') break;
      s.push_back(c);
    }
    return Value(std::move(s));
  }
  const std::uint32_t step = registers_per_element(a.type.scalar) * 2;
  if (!a.type.is_array()) return decode_scalar(a.type.scalar, wire.data(), enc);
  sdk::Array arr;
  arr.reserve(a.type.element_count());
  for (std::uint32_t i = 0; i < a.type.element_count(); ++i) {
    auto v = decode_scalar(a.type.scalar, wire.data() + i * step, enc);
    if (!v) return v;
    arr.push_back(std::move(*v));
  }
  return Value(std::move(arr));
}

sdk::Result<std::vector<std::uint8_t>> encode_registers(const ModbusAddress& a, const Value& v, const Encoding& enc) {
  std::vector<std::uint8_t> out(a.units() * 2, 0);
  if (a.bit >= 0) return make_error(status::BadInternalError, "register bits are written by read-modify-write");
  if (a.type.scalar == DataType::String) {
    auto s = sdk::convert(v, DataType::String);
    if (!s) return std::unexpected(s.error());
    const std::string& str = *s->get_if<std::string>();
    if (str.size() > a.string_len) return make_error(status::BadOutOfRange, "string longer than the tag length");
    for (std::size_t i = 0; i < str.size(); ++i) {
      std::size_t reg = i / 2;
      bool high = (i % 2 == 0) == a.string_high_first;
      std::size_t idx = reg * 2 + (high == enc.modbus_byte_order ? 0 : 1);
      out[idx] = static_cast<std::uint8_t>(str[i]);
    }
    return out;
  }
  const std::uint32_t step = registers_per_element(a.type.scalar) * 2;
  if (!a.type.is_array()) {
    if (auto st = encode_scalar(a.type.scalar, v, out.data(), enc); st != status::Good) return make_error(st, "cannot encode value");
    return out;
  }
  const auto* arr = v.get_if<sdk::Array>();
  if (!arr || arr->size() != a.type.element_count()) return make_error(status::BadTypeMismatch, "array size mismatch");
  for (std::size_t i = 0; i < arr->size(); ++i)
    if (auto st = encode_scalar(a.type.scalar, (*arr)[i], out.data() + i * step, enc); st != status::Good)
      return make_error(st, "cannot encode array element");
  return out;
}

Value decode_bits(const ModbusAddress& a, std::span<const std::uint8_t> packed, std::uint32_t first) {
  auto bit = [&](std::uint32_t i) { return ((packed[i / 8] >> (i % 8)) & 1) != 0; };
  if (!a.type.is_array()) return Value(bit(first));
  sdk::Array arr;
  for (std::uint32_t i = 0; i < a.type.element_count(); ++i) arr.push_back(Value(bit(first + i)));
  return Value(std::move(arr));
}

std::uint8_t read_function(Area a) {
  switch (a) {
    case Area::Coil: return kReadCoils;
    case Area::DiscreteInput: return kReadDiscreteInputs;
    case Area::InputRegister: return kReadInputRegisters;
    case Area::HoldingRegister: return kReadHoldingRegisters;
  }
  return kReadHoldingRegisters;
}

std::vector<std::uint8_t> make_read(Area a, std::uint16_t start, std::uint16_t count) {
  std::vector<std::uint8_t> pdu{read_function(a)};
  put16(pdu, start);
  put16(pdu, count);
  return pdu;
}

sdk::StatusCode exception_status(std::uint8_t code) {
  switch (code) {
    case 0x01: return status::BadNotSupported;       // illegal function
    case 0x02: return status::BadOutOfRange;         // illegal data address
    case 0x03: return status::BadOutOfRange;         // illegal data value
    case 0x04: return status::BadDeviceFailure;      // server device failure
    case 0x05:                                        // acknowledge (long operation)
    case 0x06: return status::BadTimeout;            // server busy
    case 0x0A: return status::BadNotConnected;       // gateway path unavailable
    case 0x0B: return status::BadTimeout;            // gateway target failed to respond
    default: return status::BadCommunicationError;
  }
}

}  // namespace logict::modbus
