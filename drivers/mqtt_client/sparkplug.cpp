#include "sparkplug.h"

#include <bit>
#include <cstring>

namespace logict::sparkplug {

namespace status = sdk::status;

namespace {

constexpr std::string_view kNamespace = "spBv1.0";

constexpr std::pair<std::string_view, MsgType> kTypes[] = {
    {"NBIRTH", MsgType::NBirth}, {"NDEATH", MsgType::NDeath}, {"DBIRTH", MsgType::DBirth}, {"DDEATH", MsgType::DDeath},
    {"NDATA", MsgType::NData},   {"DDATA", MsgType::DData},   {"NCMD", MsgType::NCmd},     {"DCMD", MsgType::DCmd},
};

bool is_device_type(MsgType t) {
  return t == MsgType::DBirth || t == MsgType::DDeath || t == MsgType::DData || t == MsgType::DCmd;
}

// ---- protobuf wire format ---------------------------------------------------------

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> d) : d_(d) {}
  bool done() const { return pos_ >= d_.size(); }

  bool varint(std::uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (pos_ >= d_.size()) return false;
      const std::uint8_t b = d_[pos_++];
      v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
      if (!(b & 0x80)) return true;
    }
    return false;
  }
  bool fixed(std::size_t n, std::uint64_t& v) {
    if (d_.size() - pos_ < n) return false;
    v = 0;
    for (std::size_t i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(d_[pos_ + i]) << (8 * i);
    pos_ += n;
    return true;
  }
  bool bytes(std::span<const std::uint8_t>& out) {
    std::uint64_t n;
    if (!varint(n) || n > d_.size() - pos_) return false;
    out = d_.subspan(pos_, static_cast<std::size_t>(n));
    pos_ += static_cast<std::size_t>(n);
    return true;
  }
  // Skips a field of the given wire type.
  bool skip(std::uint64_t wire) {
    std::uint64_t v;
    std::span<const std::uint8_t> s;
    switch (wire) {
      case 0: return varint(v);
      case 1: return fixed(8, v);
      case 2: return bytes(s);
      case 5: return fixed(4, v);
      default: return false;  // groups are not used by Sparkplug
    }
  }

 private:
  std::span<const std::uint8_t> d_;
  std::size_t pos_ = 0;
};

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<std::uint8_t>(v | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<std::uint8_t>(v));
}
void put_key(std::vector<std::uint8_t>& out, int field, int wire) { put_varint(out, static_cast<std::uint64_t>(field << 3 | wire)); }
void put_fixed(std::vector<std::uint8_t>& out, std::uint64_t v, int n) {
  for (int i = 0; i < n; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_bytes(std::vector<std::uint8_t>& out, int field, std::span<const std::uint8_t> b) {
  put_key(out, field, 2);
  put_varint(out, b.size());
  out.insert(out.end(), b.begin(), b.end());
}

sdk::Result<Metric> decode_metric(std::span<const std::uint8_t> data) {
  Metric m;
  Reader r(data);
  while (!r.done()) {
    std::uint64_t key, v;
    std::span<const std::uint8_t> s;
    if (!r.varint(key)) return sdk::make_error(status::BadCommunicationError, "truncated metric");
    const auto field = key >> 3, wire = key & 7;
    bool ok = true;
    auto want = [&](std::uint64_t w) { return wire == w; };
    switch (field) {
      case 1: ok = want(2) && r.bytes(s); m.name.assign(s.begin(), s.end()); m.has_name = true; break;
      case 2: ok = want(0) && r.varint(m.alias); m.has_alias = true; break;
      case 3: ok = want(0) && r.varint(m.timestamp); break;
      case 4: ok = want(0) && r.varint(v); m.datatype = static_cast<std::uint32_t>(v); break;
      case 7: ok = want(0) && r.varint(v); m.is_null = v != 0; break;
      case 10: ok = want(0) && r.varint(m.bits); m.field = Metric::Field::Int; break;
      case 11: ok = want(0) && r.varint(m.bits); m.field = Metric::Field::Long; break;
      case 12: ok = want(5) && r.fixed(4, m.bits); m.field = Metric::Field::Float; break;
      case 13: ok = want(1) && r.fixed(8, m.bits); m.field = Metric::Field::Double; break;
      case 14: ok = want(0) && r.varint(m.bits); m.field = Metric::Field::Bool; break;
      case 15: ok = want(2) && r.bytes(s); m.text.assign(s.begin(), s.end()); m.field = Metric::Field::String; break;
      case 16: case 17: case 18: case 19: ok = r.skip(wire); m.field = Metric::Field::Other; break;
      default: ok = r.skip(wire); break;  // historical/transient flags, metadata, properties
    }
    if (!ok) return sdk::make_error(status::BadCommunicationError, "malformed metric");
  }
  return m;
}

std::int64_t sign_extend(std::uint64_t v, int bits) {
  const std::uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
  v &= mask;
  if (bits < 64 && (v >> (bits - 1)) & 1) v |= ~mask;
  return static_cast<std::int64_t>(v);
}

}  // namespace

std::optional<Topic> parse_topic(std::string_view topic) {
  std::vector<std::string_view> parts;
  for (std::size_t start = 0;;) {
    auto slash = topic.find('/', start);
    parts.push_back(topic.substr(start, slash - start));
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  if (parts.size() < 4 || parts.size() > 5 || parts[0] != kNamespace) return std::nullopt;
  Topic t;
  bool known = false;
  for (auto& [name, type] : kTypes)
    if (parts[2] == name) t.type = type, known = true;
  if (!known || is_device_type(t.type) != (parts.size() == 5)) return std::nullopt;
  t.group = parts[1];
  t.edge = parts[3];
  if (parts.size() == 5) t.device = parts[4];
  return t;
}

std::string make_topic(std::string_view group, MsgType type, std::string_view edge, std::string_view device) {
  std::string out = std::string(kNamespace) + "/" + std::string(group) + "/";
  for (auto& [name, t] : kTypes)
    if (t == type) out += name;
  out += "/" + std::string(edge);
  if (!device.empty()) out += "/" + std::string(device);
  return out;
}

sdk::Result<Payload> decode(std::span<const std::uint8_t> data) {
  Payload p;
  Reader r(data);
  while (!r.done()) {
    std::uint64_t key, v;
    std::span<const std::uint8_t> s;
    if (!r.varint(key)) return sdk::make_error(status::BadCommunicationError, "truncated payload");
    const auto field = key >> 3, wire = key & 7;
    bool ok;
    if (field == 1 && wire == 0) {
      ok = r.varint(p.timestamp);
    } else if (field == 2 && wire == 2) {
      ok = r.bytes(s);
      if (ok) {
        auto m = decode_metric(s);
        if (!m) return std::unexpected(m.error());
        p.metrics.push_back(std::move(*m));
      }
    } else if (field == 3 && wire == 0) {
      ok = r.varint(v);
      p.seq = v;
    } else {
      ok = r.skip(wire);  // uuid, body
    }
    if (!ok) return sdk::make_error(status::BadCommunicationError, "malformed payload");
  }
  return p;
}

std::vector<std::uint8_t> encode(const Payload& p) {
  std::vector<std::uint8_t> out;
  put_key(out, 1, 0);
  put_varint(out, p.timestamp);
  for (const auto& m : p.metrics) {
    std::vector<std::uint8_t> b;
    if (m.has_name) put_bytes(b, 1, std::span(reinterpret_cast<const std::uint8_t*>(m.name.data()), m.name.size()));
    if (m.has_alias) put_key(b, 2, 0), put_varint(b, m.alias);
    put_key(b, 3, 0), put_varint(b, m.timestamp);
    put_key(b, 4, 0), put_varint(b, m.datatype);
    if (m.is_null) put_key(b, 7, 0), put_varint(b, 1);
    switch (m.field) {
      case Metric::Field::Int: put_key(b, 10, 0), put_varint(b, m.bits & 0xFFFFFFFFu); break;
      case Metric::Field::Long: put_key(b, 11, 0), put_varint(b, m.bits); break;
      case Metric::Field::Float: put_key(b, 12, 5), put_fixed(b, m.bits, 4); break;
      case Metric::Field::Double: put_key(b, 13, 1), put_fixed(b, m.bits, 8); break;
      case Metric::Field::Bool: put_key(b, 14, 0), put_varint(b, m.bits ? 1 : 0); break;
      case Metric::Field::String:
        put_bytes(b, 15, std::span(reinterpret_cast<const std::uint8_t*>(m.text.data()), m.text.size()));
        break;
      default: break;
    }
    put_bytes(out, 2, b);
  }
  if (p.seq) put_key(out, 3, 0), put_varint(out, *p.seq);
  return out;
}

sdk::Result<sdk::Value> value(const Metric& m, std::uint32_t datatype) {
  using F = Metric::Field;
  auto field_is = [&](F f) { return m.field == f; };
  auto mismatch = [] { return sdk::make_error(status::BadTypeMismatch, "metric value does not match its data type"); };
  switch (datatype) {
    case Int8: if (!field_is(F::Int)) return mismatch(); return sdk::Value(static_cast<std::int8_t>(sign_extend(m.bits, 8)));
    case Int16: if (!field_is(F::Int)) return mismatch(); return sdk::Value(static_cast<std::int16_t>(sign_extend(m.bits, 16)));
    case Int32: if (!field_is(F::Int)) return mismatch(); return sdk::Value(static_cast<std::int32_t>(sign_extend(m.bits, 32)));
    case Int64: if (!field_is(F::Long)) return mismatch(); return sdk::Value(static_cast<std::int64_t>(m.bits));
    case UInt8: if (!field_is(F::Int)) return mismatch(); return sdk::Value(static_cast<std::uint8_t>(m.bits));
    case UInt16: if (!field_is(F::Int)) return mismatch(); return sdk::Value(static_cast<std::uint16_t>(m.bits));
    case UInt32: if (!field_is(F::Int) && !field_is(F::Long)) return mismatch(); return sdk::Value(static_cast<std::uint32_t>(m.bits));
    case UInt64: if (!field_is(F::Long)) return mismatch(); return sdk::Value(m.bits);
    case Float: if (!field_is(F::Float)) return mismatch(); return sdk::Value(std::bit_cast<float>(static_cast<std::uint32_t>(m.bits)));
    case Double: if (!field_is(F::Double)) return mismatch(); return sdk::Value(std::bit_cast<double>(m.bits));
    case Boolean: if (!field_is(F::Bool)) return mismatch(); return sdk::Value(m.bits != 0);
    case String:
    case Text:
    case UUID: if (!field_is(F::String)) return mismatch(); return sdk::Value(m.text);
    case DateTime:
      if (!field_is(F::Long)) return mismatch();
      return sdk::Value(sdk::DateTime{sdk::Timestamp(std::chrono::milliseconds(static_cast<std::int64_t>(m.bits)))});
    default: return sdk::make_error(status::BadNotSupported, "unsupported metric data type");
  }
}

sdk::StatusCode set_value(Metric& m, std::uint32_t datatype, const sdk::Value& v) {
  using F = Metric::Field;
  sdk::DataType target;
  switch (datatype) {
    case Int8: target = sdk::DataType::Char; break;
    case Int16: target = sdk::DataType::Short; break;
    case Int32: target = sdk::DataType::Long; break;
    case Int64: target = sdk::DataType::LLong; break;
    case UInt8: target = sdk::DataType::Byte; break;
    case UInt16: target = sdk::DataType::Word; break;
    case UInt32: target = sdk::DataType::DWord; break;
    case UInt64: target = sdk::DataType::QWord; break;
    case Float: target = sdk::DataType::Float; break;
    case Double: target = sdk::DataType::Double; break;
    case Boolean: target = sdk::DataType::Boolean; break;
    case String: case Text: case UUID: target = sdk::DataType::String; break;
    case DateTime: target = sdk::DataType::Date; break;
    default: return status::BadNotSupported;
  }
  auto c = sdk::convert(v, target);
  if (!c) return c.error().code;
  m.datatype = datatype;
  std::visit(
      [&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, bool>) m.field = F::Bool, m.bits = x;
        else if constexpr (std::is_same_v<T, float>) m.field = F::Float, m.bits = std::bit_cast<std::uint32_t>(x);
        else if constexpr (std::is_same_v<T, double>) m.field = F::Double, m.bits = std::bit_cast<std::uint64_t>(x);
        else if constexpr (std::is_same_v<T, std::string>) m.field = F::String, m.text = x;
        else if constexpr (std::is_same_v<T, sdk::DateTime>)
          m.field = F::Long, m.bits = static_cast<std::uint64_t>(
                                 std::chrono::duration_cast<std::chrono::milliseconds>(x.time.time_since_epoch()).count());
        else if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, std::uint64_t>)
          m.field = F::Long, m.bits = static_cast<std::uint64_t>(x);
        else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
          // Width-specific two's complement, as Eclipse Tahu writes it.
          m.field = F::Int, m.bits = static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(x));
        else if constexpr (std::is_integral_v<T>) m.field = F::Int, m.bits = x;
      },
      c->data);
  return status::Good;
}

}  // namespace logict::sparkplug
