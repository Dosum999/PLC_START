#pragma once

#include "logict/sdk/value.h"

namespace logict::mqtt {

// Decides whether a sampled value becomes an event (Kepware IoT item rules):
// - first sample: sent if "send initial update"
// - "every scan": every good sample; a bad quality only once until good again
// - otherwise: value change beyond the deadband (compared with the last *sent*
//   value), or a quality change unless quality changes are ignored.
class ChangeFilter {
 public:
  ChangeFilter(bool send_initial, bool every_scan, bool ignore_quality, double deadband)
      : send_initial_(send_initial), every_scan_(every_scan), ignore_quality_(ignore_quality), deadband_(deadband) {}

  bool accept(const sdk::Vqt& v);

  // Absolute threshold for a Kepware percent deadband: percent of the data
  // type's full range (0 for types without a range).
  static double percent_threshold(sdk::DataType t, double percent);

 private:
  void remember(const sdk::Vqt& v) {
    last_ = v.value;
    last_quality_ = v.quality;
  }

  bool send_initial_, every_scan_, ignore_quality_;
  double deadband_;
  bool has_last_ = false;
  bool bad_sent_ = false;
  sdk::Value last_;
  sdk::StatusCode last_quality_ = sdk::status::Good;
};

}  // namespace logict::mqtt
