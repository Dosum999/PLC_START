#include "logict/sdk/value.h"

#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <utility>

namespace logict::sdk {

namespace {

// Uniform numeric view used for conversions.
struct Num {
  enum Kind { Signed, Unsigned, Floating } kind;
  std::int64_t i = 0;
  std::uint64_t u = 0;
  double f = 0;
};

std::optional<Num> parse_number(std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  if (s.empty()) return std::nullopt;
  if (s == "true" || s == "TRUE" || s == "True") return Num{Num::Unsigned, 0, 1, 0};
  if (s == "false" || s == "FALSE" || s == "False") return Num{Num::Unsigned, 0, 0, 0};
  const char* b = s.data();
  const char* e = s.data() + s.size();
  if (s.front() == '-') {
    std::int64_t i;
    if (auto r = std::from_chars(b, e, i); r.ec == std::errc{} && r.ptr == e)
      return Num{Num::Signed, i, 0, 0};
  } else {
    std::uint64_t u;
    if (auto r = std::from_chars(b, e, u); r.ec == std::errc{} && r.ptr == e)
      return Num{Num::Unsigned, 0, u, 0};
  }
  double f;
  if (auto r = std::from_chars(b, e, f); r.ec == std::errc{} && r.ptr == e)
    return Num{Num::Floating, 0, 0, f};
  return std::nullopt;
}

std::optional<Num> to_num(const Value& v) {
  return std::visit(
      [](const auto& x) -> std::optional<Num> {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, bool>) {
          return Num{Num::Unsigned, 0, x ? 1u : 0u, 0};
        } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
          return Num{Num::Signed, x, 0, 0};
        } else if constexpr (std::is_integral_v<T>) {
          return Num{Num::Unsigned, 0, x, 0};
        } else if constexpr (std::is_floating_point_v<T>) {
          return Num{Num::Floating, 0, 0, static_cast<double>(x)};
        } else if constexpr (std::is_same_v<T, std::string>) {
          return parse_number(x);
        } else {
          return std::nullopt;
        }
      },
      v.data);
}

template <typename T>
Result<Value> num_to(const Num& n) {
  if constexpr (std::is_floating_point_v<T>) {
    double d = n.kind == Num::Signed ? static_cast<double>(n.i)
               : n.kind == Num::Unsigned ? static_cast<double>(n.u)
                                         : n.f;
    if constexpr (std::is_same_v<T, float>) {
      if (std::isfinite(d) && std::fabs(d) > std::numeric_limits<float>::max())
        return make_error(status::BadOutOfRange, "value out of range for Float");
    }
    return Value(static_cast<T>(d));
  } else {
    constexpr auto lo = std::numeric_limits<T>::min();
    constexpr auto hi = std::numeric_limits<T>::max();
    switch (n.kind) {
      case Num::Signed:
        if (std::cmp_less(n.i, lo) || std::cmp_greater(n.i, hi)) break;
        return Value(static_cast<T>(n.i));
      case Num::Unsigned:
        if (std::cmp_greater(n.u, hi)) break;
        return Value(static_cast<T>(n.u));
      case Num::Floating: {
        if (!std::isfinite(n.f)) break;
        double r = std::trunc(n.f);
        if (r < static_cast<double>(lo) || r > static_cast<double>(hi)) break;
        return Value(static_cast<T>(r));
      }
    }
    return make_error(status::BadOutOfRange, "value out of range for integer type");
  }
}

std::string scalar_to_string(const Value& v) {
  return std::visit(
      [](const auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return "";
        } else if constexpr (std::is_same_v<T, bool>) {
          return x ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::int8_t> || std::is_same_v<T, std::uint8_t>) {
          return std::to_string(static_cast<int>(x));
        } else if constexpr (std::is_arithmetic_v<T>) {
          return std::format("{}", x);
        } else if constexpr (std::is_same_v<T, std::string>) {
          return x;
        } else if constexpr (std::is_same_v<T, DateTime>) {
          return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(x.time));
        } else {
          return "";
        }
      },
      v.data);
}

}  // namespace

DataType type_of(const Value& v) noexcept {
  return std::visit(
      [](const auto& x) -> DataType {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, bool>) return DataType::Boolean;
        else if constexpr (std::is_same_v<T, std::int8_t>) return DataType::Char;
        else if constexpr (std::is_same_v<T, std::uint8_t>) return DataType::Byte;
        else if constexpr (std::is_same_v<T, std::int16_t>) return DataType::Short;
        else if constexpr (std::is_same_v<T, std::uint16_t>) return DataType::Word;
        else if constexpr (std::is_same_v<T, std::int32_t>) return DataType::Long;
        else if constexpr (std::is_same_v<T, std::uint32_t>) return DataType::DWord;
        else if constexpr (std::is_same_v<T, std::int64_t>) return DataType::LLong;
        else if constexpr (std::is_same_v<T, std::uint64_t>) return DataType::QWord;
        else if constexpr (std::is_same_v<T, float>) return DataType::Float;
        else if constexpr (std::is_same_v<T, double>) return DataType::Double;
        else if constexpr (std::is_same_v<T, std::string>) return DataType::String;
        else if constexpr (std::is_same_v<T, DateTime>) return DataType::Date;
        else if constexpr (std::is_same_v<T, Array>) return x.empty() ? DataType::Default : type_of(x.front());
        else return DataType::Default;
      },
      v.data);
}

std::optional<double> to_double(const Value& v) noexcept {
  auto n = to_num(v);
  if (!n) return std::nullopt;
  switch (n->kind) {
    case Num::Signed: return static_cast<double>(n->i);
    case Num::Unsigned: return static_cast<double>(n->u);
    case Num::Floating: return n->f;
  }
  return std::nullopt;
}

std::string to_string(const Value& v, const TagType* shape) {
  const auto* arr = v.get_if<Array>();
  if (!arr) return scalar_to_string(v);
  std::size_t cols = arr->size();
  if (shape && shape->rows > 0 && shape->cols > 0) cols = shape->cols;
  std::string out;
  for (std::size_t i = 0; i < arr->size(); ++i) {
    if (i % cols == 0) out += '[';
    else out += ',';
    out += scalar_to_string((*arr)[i]);
    if (i % cols == cols - 1 || i + 1 == arr->size()) out += ']';
  }
  return arr->empty() ? "[]" : out;
}

Result<Value> convert(const Value& v, DataType target) {
  if (const auto* arr = v.get_if<Array>()) {
    Array out;
    out.reserve(arr->size());
    for (const auto& e : *arr) {
      auto c = convert(e, target);
      if (!c) return c;
      out.push_back(std::move(*c));
    }
    return Value(std::move(out));
  }
  if (v.empty()) return make_error(status::BadTypeMismatch, "empty value");

  switch (target) {
    case DataType::Default:
      return v;
    case DataType::String:
      return Value(scalar_to_string(v));
    case DataType::Date:
      if (v.get_if<DateTime>()) return v;
      return make_error(status::BadTypeMismatch, "cannot convert to Date");
    default:
      break;
  }
  if (v.get_if<DateTime>()) return make_error(status::BadTypeMismatch, "cannot convert Date to number");

  auto n = to_num(v);
  if (!n) return make_error(status::BadTypeMismatch, "value is not numeric");

  switch (target) {
    case DataType::Boolean: {
      bool b = n->kind == Num::Signed ? n->i != 0 : n->kind == Num::Unsigned ? n->u != 0 : n->f != 0.0;
      return Value(b);
    }
    case DataType::Char: return num_to<std::int8_t>(*n);
    case DataType::Byte: return num_to<std::uint8_t>(*n);
    case DataType::Short: return num_to<std::int16_t>(*n);
    case DataType::Word: return num_to<std::uint16_t>(*n);
    case DataType::Long: return num_to<std::int32_t>(*n);
    case DataType::DWord: return num_to<std::uint32_t>(*n);
    case DataType::LLong: return num_to<std::int64_t>(*n);
    case DataType::QWord: return num_to<std::uint64_t>(*n);
    case DataType::Float: return num_to<float>(*n);
    case DataType::Double: return num_to<double>(*n);
    case DataType::BCD: {
      auto r = num_to<std::uint16_t>(*n);
      if (r && *r->get_if<std::uint16_t>() > 9999) return make_error(status::BadOutOfRange, "BCD range is 0..9999");
      return r;
    }
    case DataType::LBCD: {
      auto r = num_to<std::uint32_t>(*n);
      if (r && *r->get_if<std::uint32_t>() > 99999999u)
        return make_error(status::BadOutOfRange, "LBCD range is 0..99999999");
      return r;
    }
    default:
      return make_error(status::BadTypeMismatch, "unsupported target type");
  }
}

Value default_value(TagType t) {
  Value scalar;
  switch (t.scalar) {
    case DataType::Boolean: scalar = false; break;
    case DataType::Char: scalar = std::int8_t{0}; break;
    case DataType::Byte: scalar = std::uint8_t{0}; break;
    case DataType::Short: scalar = std::int16_t{0}; break;
    case DataType::Word:
    case DataType::BCD: scalar = std::uint16_t{0}; break;
    case DataType::Long: scalar = std::int32_t{0}; break;
    case DataType::DWord:
    case DataType::LBCD: scalar = std::uint32_t{0}; break;
    case DataType::LLong: scalar = std::int64_t{0}; break;
    case DataType::QWord: scalar = std::uint64_t{0}; break;
    case DataType::Float: scalar = 0.0f; break;
    case DataType::Double: scalar = 0.0; break;
    case DataType::String: scalar = std::string{}; break;
    case DataType::Date: scalar = DateTime{}; break;
    case DataType::Default: return {};
  }
  if (!t.is_array()) return scalar;
  return Value(Array(t.element_count(), scalar));
}

}  // namespace logict::sdk
