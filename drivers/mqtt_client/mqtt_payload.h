#pragma once

// Tag addresses and payload handling of the MQTT Client driver. No I/O here.
//
// Address: "<topic>" or "<topic>|<json path>".
//   plant/line1/temp              whole payload is the value
//   plant/line1|temp              member "temp" of a JSON payload
//   plant/line1|$.data.values[2]  nested members and array elements ("$." optional)
//   plant/+/temp                  wildcards (+, #) subscribe; such tags are read-only
//   plant/arr [10]                arrays: dimensions after a space, " [rows]" or " [rows][cols]"
//
// Sparkplug B metrics: "spb:<group>/<edge node>[/<device>]|<metric name>",
// e.g. spb:Plant1/Edge1/Press3|Motor/Speed. The type may stay Default: the
// metric's BIRTH data type is used.

#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "logict/sdk/driver.h"

namespace logict::mqtt_client {

// One step of a JSON path: member name or array index.
using PathStep = std::variant<std::string, std::size_t>;

struct MqttAddress final : sdk::Address {
  std::string topic;           // topic filter to subscribe
  std::vector<PathStep> path;  // empty: whole payload
  bool has_path = false;
  bool wildcard = false;       // + or # in the topic

  bool sparkplug = false;      // topic is then spBv1.0/<group>/+/<edge>/#
  std::string group, edge, device, metric;
  std::string entity() const { return group + "/" + edge + (device.empty() ? "" : "/" + device); }
};

// Default type: String for a whole payload, Double for a JSON member.
sdk::Result<std::shared_ptr<MqttAddress>> parse_address(std::string_view text, sdk::TagType requested);

// MQTT topic filter matching (+ one level, # the rest; $-topics only match explicitly).
bool topic_matches(std::string_view filter, std::string_view topic);

// Value of a tag from a received payload.
sdk::Result<sdk::Value> decode(const MqttAddress& a, std::string_view payload);

// Payload published when the tag is written: plain text for a whole-payload
// tag (strings as is, numbers and booleans as JSON, arrays as JSON arrays),
// a JSON object for a member path ({"a":{"b":value}}; index steps are not
// writable).
sdk::Result<std::string> encode(const MqttAddress& a, const sdk::Value& v);

}  // namespace logict::mqtt_client
