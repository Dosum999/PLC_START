#pragma once

#include "logict/sdk/value.h"
#include "model/project.h"

namespace logict::core {

// Kepware tag scaling. Linear: (SH-SL)/(RH-RL)*(raw-RL)+SL.
// Square root: sqrt((raw-RL)/(RH-RL))*(SH-SL)+SL. Then clamp, then negate.
sdk::Result<sdk::Value> scale(const sdk::Value& raw, const model::Scaling& s);

// Inverse used for writes; the result is converted to `raw_type`.
sdk::Result<sdk::Value> unscale(const sdk::Value& scaled, const model::Scaling& s, sdk::DataType raw_type);

}  // namespace logict::core
