#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "logict/sdk/status.h"

namespace logict::mqtt {

// Property keys. iot_gateway.* follow Kepware's IoT Gateway Configuration API
// naming; logict.* are extensions Kepware does not have.
// TODO(verify): Kepware key names reconstructed from docs; check against a live
// /config/v1/doc/... response before claiming import compatibility.
namespace keys {
inline constexpr const char* kName = "common.ALLTYPES_NAME";
inline constexpr const char* kEnabled = "iot_gateway.AGENTTYPES_ENABLED";
inline constexpr const char* kUrl = "iot_gateway.MQTT_CLIENT_URL";
inline constexpr const char* kTopic = "iot_gateway.MQTT_CLIENT_TOPIC";
inline constexpr const char* kQos = "iot_gateway.MQTT_CLIENT_QOS";
inline constexpr const char* kPublishType = "iot_gateway.AGENTTYPES_PUBLISH_TYPE";  // 0 interval, 1 on data change
inline constexpr const char* kRate = "iot_gateway.AGENTTYPES_RATE_MS";
inline constexpr const char* kFormat = "iot_gateway.AGENTTYPES_PUBLISH_FORMAT";  // 0 narrow, 1 wide
inline constexpr const char* kMaxEvents = "iot_gateway.AGENTTYPES_MAX_EVENTS";
inline constexpr const char* kTimeout = "iot_gateway.AGENTTYPES_TIMEOUT_S";
inline constexpr const char* kSendInitial = "iot_gateway.AGENTTYPES_SEND_INITIAL_UPDATE";
inline constexpr const char* kIgnoreQuality = "iot_gateway.IGNORE_QUALITY_CHANGES";
inline constexpr const char* kMessageFormat = "iot_gateway.AGENTTYPES_MESSAGE_FORMAT";  // 0 standard, 1 advanced
inline constexpr const char* kTemplate = "iot_gateway.AGENTTYPES_STANDARD_TEMPLATE";
inline constexpr const char* kExpansion = "iot_gateway.AGENTTYPES_EXPANSION_OF_VALUES";
inline constexpr const char* kAdvancedTemplate = "iot_gateway.AGENTTYPES_ADVANCED_TEMPLATE";
inline constexpr const char* kClientId = "iot_gateway.MQTT_CLIENT_CLIENT_ID";
inline constexpr const char* kUsername = "iot_gateway.MQTT_CLIENT_USERNAME";
inline constexpr const char* kPassword = "iot_gateway.MQTT_CLIENT_PASSWORD";
inline constexpr const char* kTlsVersion = "iot_gateway.MQTT_TLS_VERSION";  // 0 default, 1 TLS 1.2, 2 TLS 1.3
inline constexpr const char* kLwtEnable = "iot_gateway.MQTT_CLIENT_ENABLE_LAST_WILL";
inline constexpr const char* kLwtTopic = "iot_gateway.MQTT_CLIENT_LAST_WILL_TOPIC";
inline constexpr const char* kLwtMessage = "iot_gateway.MQTT_CLIENT_LAST_WILL_MESSAGE";
inline constexpr const char* kWriteEnable = "iot_gateway.MQTT_CLIENT_ENABLE_WRITE_TOPIC";
inline constexpr const char* kWriteTopic = "iot_gateway.MQTT_CLIENT_WRITE_TOPIC";
// Extensions
inline constexpr const char* kProtocol = "logict.MQTT_PROTOCOL_VERSION";  // 4 = 3.1.1, 5 = 5.0
inline constexpr const char* kTopicMode = "logict.MQTT_TOPIC_MODE";       // 0 agent topic, 1 per tag
inline constexpr const char* kTagTopic = "logict.MQTT_TAG_TOPIC_TEMPLATE";
inline constexpr const char* kRetain = "logict.MQTT_RETAIN";
inline constexpr const char* kLwtRetain = "logict.MQTT_LAST_WILL_RETAIN";
inline constexpr const char* kWriteQos = "logict.MQTT_WRITE_QOS";
inline constexpr const char* kWriteResponseTopic = "logict.MQTT_WRITE_RESPONSE_TOPIC";
inline constexpr const char* kBufferSize = "logict.MQTT_BUFFER_SIZE";
inline constexpr const char* kCaFile = "logict.MQTT_TLS_CA_FILE";
inline constexpr const char* kCertFile = "logict.MQTT_TLS_CERT_FILE";
inline constexpr const char* kKeyFile = "logict.MQTT_TLS_KEY_FILE";
inline constexpr const char* kKeyPassword = "logict.MQTT_TLS_KEY_PASSWORD";
inline constexpr const char* kVerifyServer = "logict.MQTT_TLS_VERIFY_SERVER";
// IoT item
inline constexpr const char* kItemTag = "iot_gateway.IOT_ITEM_SERVER_TAG";
inline constexpr const char* kItemEnabled = "iot_gateway.IOT_ITEM_ENABLED";
inline constexpr const char* kItemRate = "iot_gateway.IOT_ITEM_SCAN_RATE_MS";
inline constexpr const char* kItemEveryScan = "iot_gateway.IOT_ITEM_SEND_EVERY_SCAN";
inline constexpr const char* kItemDeadband = "iot_gateway.IOT_ITEM_DEADBAND_PERCENT";
inline constexpr const char* kItemDeadbandAbs = "logict.IOT_ITEM_DEADBAND_ABSOLUTE";
}  // namespace keys

struct ItemConfig {
  std::string name;
  std::string server_tag;  // Channel.Device.Tag
  bool enabled = true;
  std::uint32_t scan_rate_ms = 1000;
  bool every_scan = false;
  double deadband_percent = 0;
  double deadband_absolute = 0;
};

struct AgentConfig {
  enum class PublishType { Interval, OnDataChange };
  enum class Format { Narrow, Wide };
  enum class TopicMode { Agent, PerTag };

  std::string name;
  bool enabled = true;
  std::string url = "tcp://localhost:1883";
  std::string topic = "iotgateway";
  int qos = 1;
  PublishType publish_type = PublishType::Interval;
  std::uint32_t rate_ms = 10000;
  Format format = Format::Narrow;
  std::uint32_t max_events = 1000;
  std::uint32_t timeout_s = 5;
  bool send_initial = true;
  bool ignore_quality_changes = false;
  bool advanced = false;
  std::string standard_template, expansion, advanced_template;
  std::string client_id, username, password;
  int tls_version = 0;
  std::string ca_file, cert_file, key_file, key_password;
  bool verify_server = true;
  bool lwt_enabled = false;
  std::string lwt_topic, lwt_message = "offline";
  bool lwt_retain = false;
  bool write_enabled = false;
  std::string write_topic = "iotgateway/write";
  int write_qos = 0;
  std::string write_response_topic;  // empty = no response (Kepware behaviour)
  int protocol = 4;
  TopicMode topic_mode = TopicMode::Agent;
  std::string tag_topic = "{topic}/{path}";
  bool retain = false;
  std::uint32_t buffer_size = 100000;
  std::vector<ItemConfig> items;
};

// Parses one entry of project._iot_gateway.mqtt_clients.
sdk::Result<AgentConfig> agent_from_json(const nlohmann::json& j);

}  // namespace logict::mqtt
