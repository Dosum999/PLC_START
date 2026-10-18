#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "logict/sdk/status.h"
#include "logict/sdk/types.h"

namespace logict::model {

using nlohmann::json;
using sdk::Access;
using sdk::DataType;
using sdk::TagType;

struct Scaling {
  enum class Type : std::uint8_t { None = 0, Linear = 1, SquareRoot = 2 };
  Type type = Type::None;
  double raw_low = 0, raw_high = 1000;
  double scaled_low = 0, scaled_high = 1000;
  DataType scaled_type = DataType::Double;
  bool clamp_low = false, clamp_high = false;
  bool negate = false;
  std::string units;
};

struct Tag {
  std::string name, description;
  std::string address;
  TagType type;
  Access access = Access::ReadWrite;
  std::uint32_t scan_rate_ms = 100;
  Scaling scaling;
  json props = json::object();  // all properties as loaded (unknown keys preserved)
};

struct TagGroup {
  std::string name, description;
  std::vector<Tag> tags;
  std::vector<TagGroup> groups;
  json props = json::object();
};

enum class ScanMode : std::uint8_t {
  RespectClientRate = 0,
  NoFasterThanScanRate = 1,
  AllDataAtScanRate = 2,
  DemandPollOnly = 3,
  RespectTagRate = 4,
};

struct Device {
  std::string name, description;
  std::string model, id;
  bool enabled = true;
  bool simulated = false;
  ScanMode scan_mode = ScanMode::RespectClientRate;
  std::uint32_t scan_rate_ms = 1000;
  bool initial_updates_from_cache = false;
  std::uint32_t connect_timeout_s = 3;
  std::uint32_t request_timeout_ms = 1000;
  std::uint32_t attempts = 3;
  std::uint32_t inter_request_delay_ms = 0;
  bool demotion_enabled = false;
  std::uint32_t demote_after_timeouts = 3;
  std::uint32_t demotion_period_ms = 10000;
  bool demotion_discard_writes = false;
  std::vector<Tag> tags;
  std::vector<TagGroup> groups;
  json props = json::object();
};

enum class WriteOptimization : std::uint8_t {
  WriteAllValues = 0,
  LatestValueNonBoolean = 1,
  LatestValueAll = 2,
};

struct Channel {
  std::string name, description;
  std::string driver;
  WriteOptimization write_method = WriteOptimization::LatestValueAll;
  std::uint32_t duty_cycle = 10;
  bool replace_non_normalized_floats = true;
  bool diagnostics = false;
  std::vector<Device> devices;
  json props = json::object();
};

struct Project {
  std::string title, description;
  std::vector<Channel> channels;
  json iot_gateway = json::object();  // parsed by the MQTT module
  json props = json::object();
};

// Kepware-style project JSON: {"project": {..., "channels": [{..., "devices": [{..., "tags": [],
// "tag_groups": []}]}], "_iot_gateway": {...}}}. Also accepts the inner object directly.
sdk::Result<Project> project_from_json(const json& j);
json project_to_json(const Project& p);

// Per-object parsing, reused by the REST API.
sdk::Result<Tag> tag_from_json(const json& j);
json tag_to_json(const Tag& t);
sdk::Result<TagGroup> tag_group_from_json(const json& j);
json tag_group_to_json(const TagGroup& g);
sdk::Result<Device> device_from_json(const json& j);
json device_to_json(const Device& d);
sdk::Result<Channel> channel_from_json(const json& j);
json channel_to_json(const Channel& c);

// Kepware naming rules: non-empty, no leading underscore, no '.', '"', '/', '\'.
bool is_valid_name(std::string_view name) noexcept;

}  // namespace logict::model
