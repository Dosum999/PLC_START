#include "s7_protocol.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>

#include "logict/sdk/codec.h"

namespace logict::s7 {

namespace status = sdk::status;
using sdk::DataType;
using sdk::make_error;
using sdk::TagType;
using sdk::Value;

namespace {

using Result = sdk::Result<std::shared_ptr<S7Address>>;
Result bad(std::string msg) { return make_error(status::BadInvalidArgument, std::move(msg)); }

template <typename T>
std::optional<T> num(std::string_view s) {
  T v{};
  if (s.empty()) return std::nullopt;
  auto r = std::from_chars(s.data(), s.data() + s.size(), v);
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

// Resolves the tag type from the size letter (B/W/D) and the requested type.
std::optional<DataType> resolve(char size, DataType requested) {
  const DataType natural = size == 'B' ? DataType::Byte : size == 'W' ? DataType::Word : DataType::DWord;
  if (requested == DataType::Default) return natural;
  const std::size_t n = sdk::native_size(requested);
  if (requested == DataType::Boolean || requested == DataType::String || requested == DataType::Date) return std::nullopt;
  if (size == 'B' && n == 1) return requested;
  if (size == 'W' && n == 2) return requested;
  if (size == 'D' && (n == 4 || n == 8)) return requested;  // 8 bytes: LREAL/LINT (S7-1500)
  return std::nullopt;
}

}  // namespace

sdk::Result<std::shared_ptr<S7Address>> parse_address(std::string_view text, TagType requested) {
  auto a = std::make_shared<S7Address>();
  a->text = std::string(text);
  std::string s;
  for (char c : text)
    if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  std::uint32_t rows, cols;
  if (!split_dims(s, rows, cols)) return bad("invalid array dimensions");
  const TagType req{requested.scalar, rows, cols};

  std::string rest;
  if (s.starts_with("DB")) {
    auto dot = s.find('.');
    if (dot == std::string::npos) return bad("expected DBn.DBXb.x, DBn.DBB/W/Db or DBn.STRINGb.len");
    auto db = num<std::uint16_t>(std::string_view(s).substr(2, dot - 2));
    if (!db || *db == 0) return bad("invalid data block number");
    a->area = DataBlock;
    a->db = *db;
    rest = s.substr(dot + 1);
    if (rest.starts_with("STRING")) {
      auto p = rest.find('.', 6);
      auto off = num<std::uint32_t>(std::string_view(rest).substr(6, p == std::string::npos ? std::string::npos : p - 6));
      auto len = p == std::string::npos ? std::nullopt : num<std::uint32_t>(std::string_view(rest).substr(p + 1));
      if (!off || !len || *len == 0 || *len > 254) return bad("expected DBn.STRINGoffset.length (length 1..254)");
      if (req.scalar != DataType::Default && req.scalar != DataType::String) return bad("STRING requires type String");
      if (req.is_array()) return bad("string arrays are not supported");
      a->type = TagType{DataType::String};
      a->byte_offset = *off;
      a->string_max = *len;
      a->byte_len = *len + 2;
      return a;
    }
    if (!rest.starts_with("DB") || rest.size() < 4) return bad("expected DBX, DBB, DBW or DBD after the data block");
    rest.erase(0, 2);  // now "X10.3", "W10", ...
  } else {
    switch (s.empty() ? '\0' : s.front()) {
      case 'I':
      case 'E': a->area = Inputs; break;
      case 'Q':
      case 'A': a->area = Outputs; break;
      case 'M': a->area = Flags; break;
      default: return bad("address must start with I/E, Q/A, M or DB");
    }
    rest = s.substr(1);
  }

  char size = 'X';
  if (!rest.empty() && std::string_view("XBWD").find(rest.front()) != std::string_view::npos) {
    size = rest.front();
    rest.erase(0, 1);
  } else if (rest.find('.') == std::string::npos) {
    return bad("missing size letter (B, W, D) or bit (.0-.7)");
  }
  std::string off_s = rest, bit_s;
  if (auto dot = rest.find('.'); dot != std::string::npos) {
    off_s = rest.substr(0, dot);
    bit_s = rest.substr(dot + 1);
  }
  auto off = num<std::uint32_t>(off_s);
  if (!off || *off > 0xFFFFF) return bad("invalid byte offset");
  a->byte_offset = *off;

  if (size == 'X') {
    auto bit = num<int>(bit_s);
    if (!bit || *bit < 0 || *bit > 7) return bad("bit must be .0 to .7");
    if (req.scalar != DataType::Default && req.scalar != DataType::Boolean) return bad("bit addresses require type Boolean");
    if (req.is_array()) return bad("Boolean arrays are not supported");
    a->bit = *bit;
    a->type = TagType{DataType::Boolean};
    a->byte_len = 1;
    return a;
  }
  if (!bit_s.empty()) return bad("unexpected '.' after a byte/word/dword address");
  auto t = resolve(size, req.scalar);
  if (!t) return bad(std::format("type {} does not fit size {}", sdk::to_string(req.scalar), size));
  a->type = TagType{*t, rows, cols};
  a->byte_len = static_cast<std::uint32_t>(sdk::native_size(*t) * a->type.element_count());
  if (a->byte_len > 65535) return bad("tag too large");
  return a;
}

// ---- values ---------------------------------------------------------------------

namespace {

sdk::Result<Value> decode_scalar(DataType t, const std::uint8_t* p) {
  using sdk::codec::ByteOrder;
  using sdk::codec::load;
  auto s = [&](std::size_t n) { return std::span<const std::uint8_t>(p, n); };
  switch (t) {
    case DataType::Char: return Value(static_cast<std::int8_t>(p[0]));
    case DataType::Byte: return Value(p[0]);
    case DataType::Short: return Value(load<std::int16_t>(s(2), ByteOrder::ABCD));
    case DataType::Word: return Value(load<std::uint16_t>(s(2), ByteOrder::ABCD));
    case DataType::Long: return Value(load<std::int32_t>(s(4), ByteOrder::ABCD));
    case DataType::DWord: return Value(load<std::uint32_t>(s(4), ByteOrder::ABCD));
    case DataType::LLong: return Value(load<std::int64_t>(s(8), ByteOrder::ABCD));
    case DataType::QWord: return Value(load<std::uint64_t>(s(8), ByteOrder::ABCD));
    case DataType::Float: return Value(load<float>(s(4), ByteOrder::ABCD));
    case DataType::Double: return Value(load<double>(s(8), ByteOrder::ABCD));
    case DataType::BCD: {
      auto v = sdk::codec::bcd_decode(load<std::uint16_t>(s(2), ByteOrder::ABCD), 4);
      if (!v) return make_error(status::BadOutOfRange, "invalid BCD value");
      return Value(static_cast<std::uint16_t>(*v));
    }
    case DataType::LBCD: {
      auto v = sdk::codec::bcd_decode(load<std::uint32_t>(s(4), ByteOrder::ABCD), 8);
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
    case DataType::Char: p[0] = static_cast<std::uint8_t>(*x.get_if<std::int8_t>()); break;
    case DataType::Byte: p[0] = *x.get_if<std::uint8_t>(); break;
    case DataType::Short: store(*x.get_if<std::int16_t>(), s(2), ByteOrder::ABCD); break;
    case DataType::Word: store(*x.get_if<std::uint16_t>(), s(2), ByteOrder::ABCD); break;
    case DataType::Long: store(*x.get_if<std::int32_t>(), s(4), ByteOrder::ABCD); break;
    case DataType::DWord: store(*x.get_if<std::uint32_t>(), s(4), ByteOrder::ABCD); break;
    case DataType::LLong: store(*x.get_if<std::int64_t>(), s(8), ByteOrder::ABCD); break;
    case DataType::QWord: store(*x.get_if<std::uint64_t>(), s(8), ByteOrder::ABCD); break;
    case DataType::Float: store(*x.get_if<float>(), s(4), ByteOrder::ABCD); break;
    case DataType::Double: store(*x.get_if<double>(), s(8), ByteOrder::ABCD); break;
    case DataType::BCD: store(static_cast<std::uint16_t>(*sdk::codec::bcd_encode(*x.get_if<std::uint16_t>(), 4)), s(2), ByteOrder::ABCD); break;
    case DataType::LBCD: store(*sdk::codec::bcd_encode(*x.get_if<std::uint32_t>(), 8), s(4), ByteOrder::ABCD); break;
    default: return status::BadTypeMismatch;
  }
  return status::Good;
}

}  // namespace

sdk::Result<Value> decode(const S7Address& a, std::span<const std::uint8_t> b) {
  if (b.size() < a.byte_len) return make_error(status::BadInternalError, "short data");
  if (a.bit >= 0) return Value(((b[0] >> a.bit) & 1) != 0);
  if (a.type.scalar == DataType::String) {
    const std::size_t len = std::min<std::size_t>({b[1], b[0], a.string_max});
    return Value(std::string(reinterpret_cast<const char*>(b.data() + 2), len));
  }
  const std::size_t step = sdk::native_size(a.type.scalar);
  if (!a.type.is_array()) return decode_scalar(a.type.scalar, b.data());
  sdk::Array arr;
  for (std::uint32_t i = 0; i < a.type.element_count(); ++i) {
    auto v = decode_scalar(a.type.scalar, b.data() + i * step);
    if (!v) return v;
    arr.push_back(std::move(*v));
  }
  return Value(std::move(arr));
}

sdk::Result<std::vector<std::uint8_t>> encode(const S7Address& a, const Value& v) {
  if (a.bit >= 0) {
    auto b = sdk::convert(v, DataType::Boolean);
    if (!b) return std::unexpected(b.error());
    return std::vector<std::uint8_t>{static_cast<std::uint8_t>(*b->get_if<bool>() ? 1 : 0)};
  }
  std::vector<std::uint8_t> out(a.byte_len, 0);
  if (a.type.scalar == DataType::String) {
    auto s = sdk::convert(v, DataType::String);
    if (!s) return std::unexpected(s.error());
    const auto& str = *s->get_if<std::string>();
    if (str.size() > a.string_max) return make_error(status::BadOutOfRange, "string longer than the STRING length");
    out[0] = static_cast<std::uint8_t>(a.string_max);
    out[1] = static_cast<std::uint8_t>(str.size());
    std::memcpy(out.data() + 2, str.data(), str.size());
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

// ---- framing ----------------------------------------------------------------------

namespace {
void put16(std::vector<std::uint8_t>& o, std::uint16_t v) {
  o.push_back(static_cast<std::uint8_t>(v >> 8));
  o.push_back(static_cast<std::uint8_t>(v));
}
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }

std::vector<std::uint8_t> job(std::uint16_t ref, const std::vector<std::uint8_t>& param, const std::vector<std::uint8_t>& data) {
  std::vector<std::uint8_t> p{0x32, 0x01, 0x00, 0x00};
  put16(p, ref);
  put16(p, static_cast<std::uint16_t>(param.size()));
  put16(p, static_cast<std::uint16_t>(data.size()));
  p.insert(p.end(), param.begin(), param.end());
  p.insert(p.end(), data.begin(), data.end());
  return p;
}

void put_item(std::vector<std::uint8_t>& o, std::uint8_t transport, std::uint16_t count, std::uint16_t db, Area area,
              std::uint32_t bit_address) {
  o.insert(o.end(), {0x12, 0x0A, 0x10, transport});
  put16(o, count);
  put16(o, db);
  o.push_back(area);
  o.push_back(static_cast<std::uint8_t>(bit_address >> 16));
  o.push_back(static_cast<std::uint8_t>(bit_address >> 8));
  o.push_back(static_cast<std::uint8_t>(bit_address));
}
}  // namespace

std::vector<std::uint8_t> connection_request(std::uint16_t local_tsap, std::uint16_t remote_tsap) {
  std::vector<std::uint8_t> f{0x03, 0x00, 0x00, 0x16,  // TPKT
                              0x11, 0xE0, 0x00, 0x00, 0x00, 0x01, 0x00,  // COTP CR, src ref 1, class 0
                              0xC0, 0x01, 0x0A,                          // TPDU size 1024
                              0xC1, 0x02};
  put16(f, local_tsap);
  f.insert(f.end(), {0xC2, 0x02});
  put16(f, remote_tsap);
  return f;
}

std::vector<std::uint8_t> data_frame(std::span<const std::uint8_t> pdu) {
  std::vector<std::uint8_t> f{0x03, 0x00};
  put16(f, static_cast<std::uint16_t>(7 + pdu.size()));
  f.insert(f.end(), {0x02, 0xF0, 0x80});  // COTP DT, last data unit
  f.insert(f.end(), pdu.begin(), pdu.end());
  return f;
}

std::vector<std::uint8_t> setup_communication(std::uint16_t ref, std::uint16_t pdu_size) {
  std::vector<std::uint8_t> param{0xF0, 0x00, 0x00, 0x01, 0x00, 0x01};
  put16(param, pdu_size);
  return job(ref, param, {});
}

std::vector<std::uint8_t> read_var(std::uint16_t ref, std::span<const ReadItem> items) {
  std::vector<std::uint8_t> param{0x04, static_cast<std::uint8_t>(items.size())};
  for (const auto& it : items) put_item(param, 0x02 /*BYTE*/, it.length, it.area == DataBlock ? it.db : 0, it.area, it.start * 8);
  return job(ref, param, {});
}

std::vector<std::uint8_t> write_var(std::uint16_t ref, Area area, std::uint16_t db, std::uint32_t start, int bit,
                                    std::span<const std::uint8_t> bytes) {
  std::vector<std::uint8_t> param{0x05, 0x01};
  std::vector<std::uint8_t> data{0x00};
  const std::uint16_t dbn = area == DataBlock ? db : 0;
  if (bit >= 0) {
    put_item(param, 0x01 /*BIT*/, 1, dbn, area, start * 8 + static_cast<std::uint32_t>(bit));
    data.push_back(0x03);  // transport: BIT
    put16(data, 1);        // length in bits
    data.push_back(bytes.empty() ? 0 : static_cast<std::uint8_t>(bytes[0] & 1));
  } else {
    put_item(param, 0x02 /*BYTE*/, static_cast<std::uint16_t>(bytes.size()), dbn, area, start * 8);
    data.push_back(0x04);  // transport: BYTE/WORD/DWORD, length in bits
    put16(data, static_cast<std::uint16_t>(bytes.size() * 8));
    data.insert(data.end(), bytes.begin(), bytes.end());
  }
  return job(ref, param, data);
}

sdk::Result<AckData> parse_ack(std::span<const std::uint8_t> p) {
  if (p.size() < kHeaderAck || p[0] != 0x32 || p[1] != 0x03)
    return make_error(status::BadCommunicationError, "not an S7 ack-data PDU");
  AckData a;
  a.pdu_ref = get16(&p[4]);
  const std::size_t plen = get16(&p[6]), dlen = get16(&p[8]);
  a.error_class = p[10];
  a.error_code = p[11];
  if (kHeaderAck + plen + dlen > p.size()) return make_error(status::BadCommunicationError, "truncated S7 PDU");
  a.param.assign(p.begin() + kHeaderAck, p.begin() + static_cast<std::ptrdiff_t>(kHeaderAck + plen));
  a.data.assign(p.begin() + static_cast<std::ptrdiff_t>(kHeaderAck + plen),
                p.begin() + static_cast<std::ptrdiff_t>(kHeaderAck + plen + dlen));
  a.function = a.param.empty() ? 0 : a.param[0];
  return a;
}

sdk::Result<std::vector<ItemResult>> parse_read_data(std::span<const std::uint8_t> d, std::size_t items) {
  std::vector<ItemResult> out;
  std::size_t pos = 0;
  for (std::size_t i = 0; i < items; ++i) {
    if (pos + 4 > d.size()) return make_error(status::BadCommunicationError, "truncated read response");
    ItemResult r;
    r.code = d[pos];
    const std::uint8_t transport = d[pos + 1];
    std::size_t len = get16(&d[pos + 2]);
    pos += 4;
    if (transport == 0x04 || transport == 0x05) len /= 8;       // length given in bits
    else if (transport == 0x03) len = (len + 7) / 8;            // BIT
    if (r.code == kItemOk) {
      if (pos + len > d.size()) return make_error(status::BadCommunicationError, "truncated read item");
      r.bytes.assign(d.begin() + static_cast<std::ptrdiff_t>(pos), d.begin() + static_cast<std::ptrdiff_t>(pos + len));
      pos += len;
      if (len % 2 && i + 1 < items) ++pos;  // fill byte between items
    }
    out.push_back(std::move(r));
  }
  return out;
}

sdk::StatusCode item_status(std::uint8_t code) {
  switch (code) {
    case kItemOk: return status::Good;
    case 0x03: return status::BadNotReadable;   // access denied
    case 0x05: return status::BadOutOfRange;    // address out of range
    case 0x06:
    case 0x07: return status::BadTypeMismatch;  // type not supported / inconsistent
    case 0x0A: return status::BadOutOfRange;    // object does not exist (e.g. missing DB)
    default: return status::BadDeviceFailure;
  }
}

std::string_view item_error_text(std::uint8_t code) {
  switch (code) {
    case 0x01: return "hardware fault";
    case 0x03: return "access denied (check PUT/GET and protection level)";
    case 0x05: return "address out of range (optimized block access enabled, or DB too small)";
    case 0x06: return "data type not supported";
    case 0x07: return "data type inconsistent";
    case 0x0A: return "object does not exist (data block missing)";
    default: return "unknown error";
  }
}

}  // namespace logict::s7
