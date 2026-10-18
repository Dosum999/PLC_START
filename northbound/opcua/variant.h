#pragma once

#include <open62541/types.h>

#include "logict/sdk/types.h"
#include "logict/sdk/value.h"

namespace logict::opcua {

// OPC UA DataType for a Kepware scalar type (BCD -> UInt16, LBCD -> UInt32).
const UA_DataType* ua_type(sdk::DataType t) noexcept;

UA_DateTime to_ua_time(sdk::Timestamp t) noexcept;

// Fills `out` (which must be empty) with a copy of the value. 2D arrays get
// ArrayDimensions from `shape`.
UA_StatusCode to_variant(const sdk::Value& v, const sdk::TagType& shape, UA_Variant* out);

// Converts a client-written variant to a Value. Arrays become sdk::Array.
sdk::Result<sdk::Value> from_variant(const UA_Variant& v);

}  // namespace logict::opcua
