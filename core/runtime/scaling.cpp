#include "runtime/scaling.h"

#include <algorithm>
#include <cmath>

namespace logict::core {

using sdk::make_error;
using sdk::Result;
using sdk::Value;
namespace status = sdk::status;

namespace {

bool is_integral(sdk::DataType t) {
  using enum sdk::DataType;
  return t != Float && t != Double && t != String && t != Date && t != Default && t != Boolean;
}

Result<double> scale_one(double raw, const model::Scaling& s) {
  double ratio = (raw - s.raw_low) / (s.raw_high - s.raw_low);
  double v;
  if (s.type == model::Scaling::Type::SquareRoot) {
    v = std::sqrt(std::max(0.0, ratio)) * (s.scaled_high - s.scaled_low) + s.scaled_low;
  } else {
    v = ratio * (s.scaled_high - s.scaled_low) + s.scaled_low;
  }
  const double lo = std::min(s.scaled_low, s.scaled_high);
  const double hi = std::max(s.scaled_low, s.scaled_high);
  if (s.clamp_low && v < lo) v = lo;
  if (s.clamp_high && v > hi) v = hi;
  if (s.negate) v = -v;
  return v;
}

double unscale_one(double v, const model::Scaling& s) {
  if (s.negate) v = -v;
  double ratio = (v - s.scaled_low) / (s.scaled_high - s.scaled_low);
  if (s.type == model::Scaling::Type::SquareRoot) ratio = ratio * ratio;
  return ratio * (s.raw_high - s.raw_low) + s.raw_low;
}

template <typename F>
Result<Value> map_numeric(const Value& in, sdk::DataType out_type, F f) {
  if (const auto* arr = in.get_if<sdk::Array>()) {
    sdk::Array out;
    out.reserve(arr->size());
    for (const auto& e : *arr) {
      auto r = map_numeric(e, out_type, f);
      if (!r) return r;
      out.push_back(std::move(*r));
    }
    return Value(std::move(out));
  }
  auto d = sdk::to_double(in);
  if (!d) return make_error(status::BadTypeMismatch, "scaling requires a numeric value");
  double v = f(*d);
  if (is_integral(out_type)) v = std::round(v);
  return sdk::convert(Value(v), out_type);
}

}  // namespace

Result<Value> scale(const Value& raw, const model::Scaling& s) {
  if (s.type == model::Scaling::Type::None) return raw;
  return map_numeric(raw, s.scaled_type, [&](double r) { return *scale_one(r, s); });
}

Result<Value> unscale(const Value& scaled, const model::Scaling& s, sdk::DataType raw_type) {
  if (s.type == model::Scaling::Type::None) return sdk::convert(scaled, raw_type);
  return map_numeric(scaled, raw_type, [&](double v) { return unscale_one(v, s); });
}

}  // namespace logict::core
