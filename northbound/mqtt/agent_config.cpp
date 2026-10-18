#include "mqtt/agent_config.h"

#include <algorithm>
#include <format>
#include <set>

#include "logict/sdk/driver.h"
#include "mqtt/template.h"

namespace logict::mqtt {

namespace status = sdk::status;
using sdk::property;

namespace {

sdk::Unexpected bad(const std::string& agent, std::string msg) {
  return sdk::make_error(status::BadConfigurationError, std::format("MQTT agent '{}': {}", agent, msg));
}

bool valid_topic(std::string_view t, bool allow_placeholders) {
  if (t.empty() || t.find_first_of("+#") != std::string_view::npos) return false;
  if (!allow_placeholders && t.find('{') != std::string_view::npos) return false;
  return true;
}

}  // namespace

sdk::Result<AgentConfig> agent_from_json(const nlohmann::json& j) {
  using namespace keys;
  if (!j.is_object()) return sdk::make_error(status::BadConfigurationError, "MQTT agent must be an object");
  AgentConfig a;
  a.name = property<std::string>(j, kName, "");
  if (a.name.empty()) return sdk::make_error(status::BadConfigurationError, "MQTT agent has no name");
  a.enabled = property(j, kEnabled, a.enabled);
  a.url = property(j, kUrl, a.url);
  if (!a.url.starts_with("tcp://") && !a.url.starts_with("ssl://") && !a.url.starts_with("mqtt://") &&
      !a.url.starts_with("mqtts://"))
    return bad(a.name, "URL must start with tcp://, ssl://, mqtt:// or mqtts://");
  if (a.url.ends_with("/")) return bad(a.name, "URL must not end with '/'");
  a.topic = property(j, kTopic, a.topic);
  if (!valid_topic(a.topic, false)) return bad(a.name, "invalid topic");
  a.qos = property(j, kQos, a.qos);
  if (a.qos < 0 || a.qos > 2) return bad(a.name, "QoS must be 0, 1 or 2");
  a.publish_type = property(j, kPublishType, 0) == 1 ? AgentConfig::PublishType::OnDataChange
                                                     : AgentConfig::PublishType::Interval;
  a.rate_ms = std::clamp<std::uint32_t>(property(j, kRate, a.rate_ms), 10, 99999990);
  a.format = property(j, kFormat, 0) == 1 ? AgentConfig::Format::Wide : AgentConfig::Format::Narrow;
  if (a.publish_type == AgentConfig::PublishType::OnDataChange) a.format = AgentConfig::Format::Narrow;
  a.max_events = std::clamp<std::uint32_t>(property(j, kMaxEvents, a.max_events), 1, 100000);
  a.timeout_s = std::clamp<std::uint32_t>(property(j, kTimeout, a.timeout_s), 1, 60);
  a.send_initial = property(j, kSendInitial, a.send_initial);
  a.ignore_quality_changes = property(j, kIgnoreQuality, a.ignore_quality_changes);
  a.advanced = property(j, kMessageFormat, 0) == 1;
  a.standard_template = property<std::string>(j, kTemplate, kDefaultStandardTemplate);
  a.expansion = property<std::string>(j, kExpansion, kDefaultExpansion);
  a.advanced_template = property<std::string>(j, kAdvancedTemplate, "");
  if (a.advanced && a.advanced_template.empty()) return bad(a.name, "advanced template is empty");
  a.client_id = property<std::string>(j, kClientId, "");
  a.username = property<std::string>(j, kUsername, "");
  a.password = property<std::string>(j, kPassword, "");
  a.tls_version = property(j, kTlsVersion, 0);
  a.ca_file = property<std::string>(j, kCaFile, "");
  a.cert_file = property<std::string>(j, kCertFile, "");
  a.key_file = property<std::string>(j, kKeyFile, "");
  a.key_password = property<std::string>(j, kKeyPassword, "");
  a.verify_server = property(j, kVerifyServer, a.verify_server);
  a.lwt_enabled = property(j, kLwtEnable, a.lwt_enabled);
  a.lwt_topic = property<std::string>(j, kLwtTopic, a.topic + "/status");
  a.lwt_message = property(j, kLwtMessage, a.lwt_message);
  a.lwt_retain = property(j, kLwtRetain, a.lwt_retain);
  if (a.lwt_enabled && !valid_topic(a.lwt_topic, false)) return bad(a.name, "invalid last will topic");
  a.write_enabled = property(j, kWriteEnable, a.write_enabled);
  a.write_topic = property(j, kWriteTopic, a.write_topic);
  a.write_qos = property(j, kWriteQos, a.write_qos);
  if (a.write_qos < 0 || a.write_qos > 2) return bad(a.name, "write QoS must be 0, 1 or 2");
  a.write_response_topic = property<std::string>(j, kWriteResponseTopic, "");
  if (a.write_enabled && !valid_topic(a.write_topic, false)) return bad(a.name, "invalid write topic");
  if (!a.write_response_topic.empty() && !valid_topic(a.write_response_topic, false))
    return bad(a.name, "invalid write response topic");
  a.protocol = property(j, kProtocol, a.protocol);
  if (a.protocol != 4 && a.protocol != 5) return bad(a.name, "protocol version must be 4 (3.1.1) or 5");
  a.topic_mode = property(j, kTopicMode, 0) == 1 ? AgentConfig::TopicMode::PerTag : AgentConfig::TopicMode::Agent;
  a.tag_topic = property(j, kTagTopic, a.tag_topic);
  if (!valid_topic(a.tag_topic, true)) return bad(a.name, "invalid tag topic template");
  a.retain = property(j, kRetain, a.retain);
  a.buffer_size = std::clamp<std::uint32_t>(property(j, kBufferSize, a.buffer_size), 1, 10000000);

  std::set<std::string> names;
  if (auto it = j.find("iot_items"); it != j.end() && it->is_array()) {
    for (const auto& ij : *it) {
      ItemConfig item;
      item.name = property<std::string>(ij, kName, "");
      item.server_tag = property<std::string>(ij, kItemTag, "");
      if (item.server_tag.empty()) return bad(a.name, std::format("IoT item '{}' has no server tag", item.name));
      if (item.name.empty()) {
        item.name = item.server_tag;
        std::replace(item.name.begin(), item.name.end(), '.', '_');
      }
      if (!names.insert(item.name).second) return bad(a.name, std::format("duplicate IoT item '{}'", item.name));
      item.enabled = property(ij, kItemEnabled, item.enabled);
      item.scan_rate_ms = std::clamp<std::uint32_t>(property(ij, kItemRate, item.scan_rate_ms), 10, 99999990);
      item.every_scan = property(ij, kItemEveryScan, item.every_scan);
      item.deadband_percent = std::clamp(property(ij, kItemDeadband, 0.0), 0.0, 100.0);
      item.deadband_absolute = std::max(0.0, property(ij, kItemDeadbandAbs, 0.0));
      a.items.push_back(std::move(item));
    }
  }

  auto tmpl = a.advanced ? Template::advanced(a.advanced_template) : Template::standard(a.standard_template, a.expansion);
  if (!tmpl) return bad(a.name, tmpl.error().message);
  return a;
}

}  // namespace logict::mqtt
