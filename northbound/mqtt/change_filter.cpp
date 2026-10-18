#include "mqtt/change_filter.h"

#include <cfloat>
#include <cmath>

namespace logict::mqtt {

namespace {

bool value_changed(const sdk::Value& a, const sdk::Value& b, double deadband) {
  if (deadband > 0 && !a.is_array() && !b.is_array()) {
    auto x = sdk::to_double(a), y = sdk::to_double(b);
    if (x && y && !a.get_if<std::string>() && !b.get_if<std::string>()) return std::fabs(*x - *y) > deadband;
  }
  return !(a == b);
}

}  // namespace

bool ChangeFilter::accept(const sdk::Vqt& v) {
  if (!has_last_) {
    has_last_ = true;
    remember(v);
    bad_sent_ = sdk::is_bad(v.quality) && send_initial_;
    return send_initial_;
  }
  if (every_scan_) {
    if (sdk::is_bad(v.quality)) {
      if (bad_sent_) return false;
      bad_sent_ = true;
    } else {
      bad_sent_ = false;
    }
    remember(v);
    return true;
  }
  const bool quality_changed = v.quality != last_quality_;
  const bool changed = value_changed(last_, v.value, deadband_) || (quality_changed && !ignore_quality_);
  if (changed) {
    remember(v);
    return true;
  }
  last_quality_ = v.quality;  // track silently so a later value change compares correctly
  return false;
}

double ChangeFilter::percent_threshold(sdk::DataType t, double percent) {
  using sdk::DataType;
  double range = 0;
  switch (t) {
    case DataType::Char:
    case DataType::Byte: range = 255; break;
    case DataType::Short:
    case DataType::Word:
    case DataType::BCD: range = 65535; break;
    case DataType::Long:
    case DataType::DWord:
    case DataType::LBCD: range = 4294967295.0; break;
    case DataType::LLong:
    case DataType::QWord: range = 18446744073709551615.0; break;
    case DataType::Float: range = 2.0 * FLT_MAX; break;
    case DataType::Double: range = DBL_MAX; break;  // 2*DBL_MAX overflows
    default: return 0;
  }
  return percent / 100.0 * range;
}

}  // namespace logict::mqtt
