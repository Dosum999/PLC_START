#include "model/project.h"

#include <format>
#include <set>

#include "model/keys.h"

namespace logict::model {

using sdk::make_error;
using sdk::Result;
namespace status = sdk::status;

namespace {

template <typename T>
T get_or(const json& j, const char* key, T fallback) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return fallback;
  try {
    return it->get<T>();
  } catch (const json::exception&) {
    return fallback;
  }
}

Result<std::string> get_name(const json& j, std::string_view what) {
  if (!j.is_object()) return make_error(status::BadConfigurationError, std::format("{} must be an object", what));
  auto name = get_or<std::string>(j, keys::kName, "");
  if (!is_valid_name(name))
    return make_error(status::BadConfigurationError, std::format("{} has invalid name '{}'", what, name));
  return name;
}

// Accepts the Kepware integer code or a type name ("Word", "Word Array").
std::optional<TagType> parse_data_type(const json& v) {
  if (v.is_number_integer()) return sdk::from_kepware_code(v.get<int>());
  if (v.is_string()) {
    std::string s = v.get<std::string>();
    bool array = false;
    if (s.size() > 6 && s.ends_with(" Array")) {
      array = true;
      s.resize(s.size() - 6);
    }
    if (auto t = sdk::data_type_from_string(s)) return TagType{*t, array ? 1u : 0u, 0};
  }
  return std::nullopt;
}

template <typename T, typename F>
Result<std::vector<T>> parse_children(const json& j, const char* key, std::string_view what, F parse) {
  std::vector<T> out;
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return out;
  if (!it->is_array()) return make_error(status::BadConfigurationError, std::format("'{}' must be an array", key));
  std::set<std::string> names;
  for (const auto& child : *it) {
    auto r = parse(child);
    if (!r) return std::unexpected(r.error());
    if (!names.insert(r->name).second)
      return make_error(status::BadConfigurationError, std::format("duplicate {} name '{}'", what, r->name));
    out.push_back(std::move(*r));
  }
  return out;
}

void check_tag_group_names(const std::vector<Tag>& tags, const std::vector<TagGroup>& groups,
                           std::string& error) {
  std::set<std::string> names;
  for (const auto& t : tags) names.insert(t.name);
  for (const auto& g : groups)
    if (names.contains(g.name)) error = std::format("tag and tag group share the name '{}'", g.name);
}

}  // namespace

bool is_valid_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > 256 || name.front() == '_' || name.front() == ' ') return false;
  for (char c : name)
    if (c == '.' || c == '"' || c == '/' || c == '\\' || static_cast<unsigned char>(c) < 0x20) return false;
  return true;
}

// ---- Tag ----------------------------------------------------------------

Result<Tag> tag_from_json(const json& j) {
  auto name = get_name(j, "tag");
  if (!name) return std::unexpected(name.error());
  Tag t;
  t.name = *name;
  t.props = j;
  t.description = get_or<std::string>(j, keys::kDescription, "");
  t.address = get_or<std::string>(j, keys::kTagAddress, "");
  if (t.address.empty())
    return make_error(status::BadConfigurationError, std::format("tag '{}' has no address", t.name));
  if (auto it = j.find(keys::kTagDataType); it != j.end()) {
    auto type = parse_data_type(*it);
    if (!type) return make_error(status::BadConfigurationError, std::format("tag '{}' has invalid data type", t.name));
    t.type = *type;
  }
  t.access = get_or<int>(j, keys::kTagAccess, 1) == 0 ? Access::ReadOnly : Access::ReadWrite;
  t.scan_rate_ms = std::max<std::uint32_t>(10, get_or<std::uint32_t>(j, keys::kTagScanRate, 100));

  Scaling& s = t.scaling;
  int scaling_type = get_or<int>(j, keys::kTagScalingType, 0);
  if (scaling_type < 0 || scaling_type > 2)
    return make_error(status::BadConfigurationError, std::format("tag '{}' has invalid scaling type", t.name));
  s.type = static_cast<Scaling::Type>(scaling_type);
  s.raw_low = get_or<double>(j, keys::kTagScalingRawLow, 0);
  s.raw_high = get_or<double>(j, keys::kTagScalingRawHigh, 1000);
  s.scaled_low = get_or<double>(j, keys::kTagScalingScaledLow, 0);
  s.scaled_high = get_or<double>(j, keys::kTagScalingScaledHigh, 1000);
  if (auto it = j.find(keys::kTagScalingScaledType); it != j.end()) {
    auto st = parse_data_type(*it);
    if (st && !st->is_array()) s.scaled_type = st->scalar;
  }
  s.clamp_low = get_or<bool>(j, keys::kTagScalingClampLow, false);
  s.clamp_high = get_or<bool>(j, keys::kTagScalingClampHigh, false);
  s.negate = get_or<bool>(j, keys::kTagScalingNegate, false);
  s.units = get_or<std::string>(j, keys::kTagScalingUnits, "");
  if (s.type != Scaling::Type::None && s.raw_high == s.raw_low)
    return make_error(status::BadConfigurationError, std::format("tag '{}': raw high equals raw low", t.name));
  return t;
}

json tag_to_json(const Tag& t) {
  json j = t.props.is_object() ? t.props : json::object();
  j[keys::kName] = t.name;
  j[keys::kDescription] = t.description;
  j[keys::kTagAddress] = t.address;
  j[keys::kTagDataType] = sdk::to_kepware_code(t.type);
  j[keys::kTagAccess] = t.access == Access::ReadOnly ? 0 : 1;
  j[keys::kTagScanRate] = t.scan_rate_ms;
  j[keys::kTagScalingType] = static_cast<int>(t.scaling.type);
  if (t.scaling.type != Scaling::Type::None) {
    j[keys::kTagScalingRawLow] = t.scaling.raw_low;
    j[keys::kTagScalingRawHigh] = t.scaling.raw_high;
    j[keys::kTagScalingScaledType] = sdk::to_kepware_code(TagType{t.scaling.scaled_type});
    j[keys::kTagScalingScaledLow] = t.scaling.scaled_low;
    j[keys::kTagScalingScaledHigh] = t.scaling.scaled_high;
    j[keys::kTagScalingClampLow] = t.scaling.clamp_low;
    j[keys::kTagScalingClampHigh] = t.scaling.clamp_high;
    j[keys::kTagScalingNegate] = t.scaling.negate;
    j[keys::kTagScalingUnits] = t.scaling.units;
  }
  return j;
}

// ---- TagGroup -----------------------------------------------------------

Result<TagGroup> tag_group_from_json(const json& j) {
  auto name = get_name(j, "tag group");
  if (!name) return std::unexpected(name.error());
  TagGroup g;
  g.name = *name;
  g.description = get_or<std::string>(j, keys::kDescription, "");
  auto tags = parse_children<Tag>(j, "tags", "tag", tag_from_json);
  if (!tags) return std::unexpected(tags.error());
  auto groups = parse_children<TagGroup>(j, "tag_groups", "tag group", tag_group_from_json);
  if (!groups) return std::unexpected(groups.error());
  g.tags = std::move(*tags);
  g.groups = std::move(*groups);
  std::string err;
  check_tag_group_names(g.tags, g.groups, err);
  if (!err.empty()) return make_error(status::BadConfigurationError, err);
  g.props = j;
  g.props.erase("tags");
  g.props.erase("tag_groups");
  return g;
}

json tag_group_to_json(const TagGroup& g) {
  json j = g.props.is_object() ? g.props : json::object();
  j[keys::kName] = g.name;
  j[keys::kDescription] = g.description;
  j["tags"] = json::array();
  for (const auto& t : g.tags) j["tags"].push_back(tag_to_json(t));
  j["tag_groups"] = json::array();
  for (const auto& c : g.groups) j["tag_groups"].push_back(tag_group_to_json(c));
  return j;
}

// ---- Device -------------------------------------------------------------

Result<Device> device_from_json(const json& j) {
  auto name = get_name(j, "device");
  if (!name) return std::unexpected(name.error());
  Device d;
  d.name = *name;
  d.description = get_or<std::string>(j, keys::kDescription, "");
  d.model = get_or<std::string>(j, keys::kDeviceModel, "");
  d.id = get_or<std::string>(j, keys::kDeviceId, "");
  d.enabled = get_or<bool>(j, keys::kDeviceEnabled, true);
  d.simulated = get_or<bool>(j, keys::kDeviceSimulated, false);
  int mode = get_or<int>(j, keys::kDeviceScanMode, 0);
  if (mode < 0 || mode > 4)
    return make_error(status::BadConfigurationError, std::format("device '{}' has invalid scan mode", d.name));
  d.scan_mode = static_cast<ScanMode>(mode);
  d.scan_rate_ms = std::max<std::uint32_t>(10, get_or<std::uint32_t>(j, keys::kDeviceScanRate, 1000));
  d.initial_updates_from_cache = get_or<bool>(j, keys::kDeviceInitialFromCache, false);
  d.connect_timeout_s = std::clamp<std::uint32_t>(get_or<std::uint32_t>(j, keys::kDeviceConnectTimeout, 3), 1, 30);
  d.request_timeout_ms = std::max<std::uint32_t>(50, get_or<std::uint32_t>(j, keys::kDeviceRequestTimeout, 1000));
  d.attempts = std::clamp<std::uint32_t>(get_or<std::uint32_t>(j, keys::kDeviceAttempts, 3), 1, 10);
  d.inter_request_delay_ms = std::min<std::uint32_t>(get_or<std::uint32_t>(j, keys::kDeviceInterRequestDelay, 0), 300000);
  d.demotion_enabled = get_or<bool>(j, keys::kDeviceDemotionEnable, false);
  d.demote_after_timeouts = std::clamp<std::uint32_t>(get_or<std::uint32_t>(j, keys::kDeviceDemoteAfter, 3), 1, 30);
  d.demotion_period_ms = std::clamp<std::uint32_t>(get_or<std::uint32_t>(j, keys::kDeviceDemotionPeriod, 10000), 100, 3600000);
  d.demotion_discard_writes = get_or<bool>(j, keys::kDeviceDemotionDiscardWrites, false);

  auto tags = parse_children<Tag>(j, "tags", "tag", tag_from_json);
  if (!tags) return std::unexpected(tags.error());
  auto groups = parse_children<TagGroup>(j, "tag_groups", "tag group", tag_group_from_json);
  if (!groups) return std::unexpected(groups.error());
  d.tags = std::move(*tags);
  d.groups = std::move(*groups);
  std::string err;
  check_tag_group_names(d.tags, d.groups, err);
  if (!err.empty()) return make_error(status::BadConfigurationError, err);
  d.props = j;
  d.props.erase("tags");
  d.props.erase("tag_groups");
  return d;
}

json device_to_json(const Device& d) {
  json j = d.props.is_object() ? d.props : json::object();
  j[keys::kName] = d.name;
  j[keys::kDescription] = d.description;
  j[keys::kDeviceModel] = d.model;
  j[keys::kDeviceId] = d.id;
  j[keys::kDeviceEnabled] = d.enabled;
  j[keys::kDeviceSimulated] = d.simulated;
  j[keys::kDeviceScanMode] = static_cast<int>(d.scan_mode);
  j[keys::kDeviceScanRate] = d.scan_rate_ms;
  j[keys::kDeviceInitialFromCache] = d.initial_updates_from_cache;
  j[keys::kDeviceConnectTimeout] = d.connect_timeout_s;
  j[keys::kDeviceRequestTimeout] = d.request_timeout_ms;
  j[keys::kDeviceAttempts] = d.attempts;
  j[keys::kDeviceInterRequestDelay] = d.inter_request_delay_ms;
  j[keys::kDeviceDemotionEnable] = d.demotion_enabled;
  j[keys::kDeviceDemoteAfter] = d.demote_after_timeouts;
  j[keys::kDeviceDemotionPeriod] = d.demotion_period_ms;
  j[keys::kDeviceDemotionDiscardWrites] = d.demotion_discard_writes;
  j["tags"] = json::array();
  for (const auto& t : d.tags) j["tags"].push_back(tag_to_json(t));
  j["tag_groups"] = json::array();
  for (const auto& g : d.groups) j["tag_groups"].push_back(tag_group_to_json(g));
  return j;
}

// ---- Channel ------------------------------------------------------------

Result<Channel> channel_from_json(const json& j) {
  auto name = get_name(j, "channel");
  if (!name) return std::unexpected(name.error());
  Channel c;
  c.name = *name;
  c.description = get_or<std::string>(j, keys::kDescription, "");
  c.driver = get_or<std::string>(j, keys::kChannelDriver, "");
  if (c.driver.empty())
    return make_error(status::BadConfigurationError, std::format("channel '{}' has no driver", c.name));
  int method = get_or<int>(j, keys::kChannelWriteMethod, 2);
  if (method < 0 || method > 2)
    return make_error(status::BadConfigurationError, std::format("channel '{}' has invalid write method", c.name));
  c.write_method = static_cast<WriteOptimization>(method);
  c.duty_cycle = std::clamp<std::uint32_t>(get_or<std::uint32_t>(j, keys::kChannelDutyCycle, 10), 1, 10);
  c.replace_non_normalized_floats = get_or<int>(j, keys::kChannelFloatHandling, 0) == 0;
  c.diagnostics = get_or<bool>(j, keys::kChannelDiagnostics, false);
  auto devices = parse_children<Device>(j, "devices", "device", device_from_json);
  if (!devices) return std::unexpected(devices.error());
  c.devices = std::move(*devices);
  c.props = j;
  c.props.erase("devices");
  return c;
}

json channel_to_json(const Channel& c) {
  json j = c.props.is_object() ? c.props : json::object();
  j[keys::kName] = c.name;
  j[keys::kDescription] = c.description;
  j[keys::kChannelDriver] = c.driver;
  j[keys::kChannelWriteMethod] = static_cast<int>(c.write_method);
  j[keys::kChannelDutyCycle] = c.duty_cycle;
  j[keys::kChannelFloatHandling] = c.replace_non_normalized_floats ? 0 : 1;
  j[keys::kChannelDiagnostics] = c.diagnostics;
  j["devices"] = json::array();
  for (const auto& d : c.devices) j["devices"].push_back(device_to_json(d));
  return j;
}

// ---- Project ------------------------------------------------------------

Result<Project> project_from_json(const json& root) {
  const json& j = root.contains("project") ? root.at("project") : root;
  if (!j.is_object()) return make_error(status::BadConfigurationError, "project must be an object");
  Project p;
  p.title = get_or<std::string>(j, keys::kProjectTitle, "");
  p.description = get_or<std::string>(j, keys::kDescription, "");
  auto channels = parse_children<Channel>(j, "channels", "channel", channel_from_json);
  if (!channels) return std::unexpected(channels.error());
  p.channels = std::move(*channels);
  if (auto it = j.find("_iot_gateway"); it != j.end() && it->is_object()) p.iot_gateway = *it;
  p.props = j;
  p.props.erase("channels");
  p.props.erase("_iot_gateway");
  return p;
}

json project_to_json(const Project& p) {
  json j = p.props.is_object() ? p.props : json::object();
  j[keys::kProjectTitle] = p.title;
  j[keys::kDescription] = p.description;
  j["channels"] = json::array();
  for (const auto& c : p.channels) j["channels"].push_back(channel_to_json(c));
  if (!p.iot_gateway.empty()) j["_iot_gateway"] = p.iot_gateway;
  return json{{"project", std::move(j)}};
}

}  // namespace logict::model
