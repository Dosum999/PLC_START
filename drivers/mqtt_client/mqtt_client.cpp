// MQTT Client driver: tags take their values from messages on MQTT topics and
// writes publish to the tag's topic (see mqtt_payload.h for addresses). Each
// device is one broker connection; its ID is the broker URL
// (tcp://host:1883, ssl://host:8883, ws://...). Paho C is linked privately.
//
// A read returns the newest message seen on the tag's topic, timestamped when
// it arrived; until the first message the tag is BadWaitingForInitialData.
// With a stale timeout, values older than that turn UncertainLastUsableValue.
//
// Sparkplug B tags (spb:...) make the device a Sparkplug host application:
// it tracks BIRTH/DATA/DEATH per edge node and device, resolves aliases,
// checks sequence numbers and asks a node to rebirth when it missed its BIRTH
// or a message. Writes are sent as NCMD/DCMD. Metrics of a dead node or
// device keep their last value as UncertainNoCommunicationLastUsableValue.
// With a host ID the device also publishes its STATE (retained, with a will).

#include <MQTTAsync.h>

#include <algorithm>
#include <condition_variable>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "logict/sdk/driver.h"
#include "mqtt_payload.h"
#include "sparkplug.h"

namespace {

using namespace logict;
using namespace logict::mqtt_client;
namespace status = sdk::status;
namespace spb = logict::sparkplug;

constexpr const char* kId = "mqtt_client";
constexpr auto kRebirthInterval = std::chrono::seconds(5);

struct Settings {
  std::string url, client_id, username, password, ca_file, cert_file, key_file, host_id;
  int qos = 1, keep_alive = 60, version = 4;
  bool retain = false, clean_session = true, verify_server = true;
  std::chrono::seconds stale{0};
};

std::uint64_t now_ms() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(sdk::Clock::now().time_since_epoch()).count());
}

class MqttDevice final : public sdk::DeviceRuntime {
 public:
  MqttDevice(Settings s, std::chrono::milliseconds request_timeout, sdk::HostContext& host)
      : s_(std::move(s)), request_timeout_(request_timeout), host_(host) {}

  ~MqttDevice() override {
    if (client_ == nullptr) return;
    disconnect();
    MQTTAsync_destroy(&client_);  // no callbacks after this
  }

  sdk::Result<std::shared_ptr<const sdk::Address>> parse_address(std::string_view text, sdk::TagType requested) override {
    auto a = mqtt_client::parse_address(text, requested);
    if (!a) return std::unexpected(a.error());
    bool now = false;
    {
      std::lock_guard lock(mu_);
      auto& set = (*a)->sparkplug ? spb_filters_ : filters_;
      now = set.insert((*a)->topic).second && connected_;
    }
    if (now) subscribe({(*a)->topic});
    return std::shared_ptr<const sdk::Address>(std::move(*a));
  }

  sdk::StatusCode connect(sdk::Deadline deadline) override {
    if (client_ == nullptr) {
      MQTTAsync_createOptions create = MQTTAsync_createOptions_initializer;
      if (s_.version == 5) create.MQTTVersion = MQTTVERSION_5;
      int rc = MQTTAsync_createWithOptions(&client_, s_.url.c_str(), s_.client_id.c_str(), MQTTCLIENT_PERSISTENCE_NONE,
                                           nullptr, &create);
      if (rc != MQTTASYNC_SUCCESS) {
        host_.log.error("cannot create MQTT client for {}: {}", s_.url, MQTTAsync_strerror(rc));
        client_ = nullptr;
        return status::BadConfigurationError;
      }
      MQTTAsync_setCallbacks(client_, this, on_lost, on_message, nullptr);
    }

    MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
    if (s_.version == 5) {
      MQTTAsync_connectOptions v5 = MQTTAsync_connectOptions_initializer5;
      opts = v5;
      opts.cleanstart = s_.clean_session;
      opts.onSuccess5 = [](void* ctx, MQTTAsync_successData5*) { static_cast<MqttDevice*>(ctx)->done(true, ""); };
      opts.onFailure5 = [](void* ctx, MQTTAsync_failureData5* d) {
        static_cast<MqttDevice*>(ctx)->done(false, d && d->message ? d->message : d ? MQTTReasonCode_toString(d->reasonCode) : "");
      };
    } else {
      opts.cleansession = s_.clean_session;
      opts.onSuccess = [](void* ctx, MQTTAsync_successData*) { static_cast<MqttDevice*>(ctx)->done(true, ""); };
      opts.onFailure = [](void* ctx, MQTTAsync_failureData* d) {
        static_cast<MqttDevice*>(ctx)->done(false, d && d->message ? d->message : std::format("code {}", d ? d->code : 0));
      };
    }
    opts.context = this;
    opts.keepAliveInterval = s_.keep_alive;
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(deadline - sdk::SteadyClock::now());
    opts.connectTimeout = static_cast<int>(std::max<std::int64_t>(1, left.count()));
    opts.automaticReconnect = 0;  // the host reconnects
    if (!s_.username.empty()) opts.username = s_.username.c_str();
    if (!s_.password.empty()) opts.password = s_.password.c_str();
    MQTTAsync_SSLOptions ssl = MQTTAsync_SSLOptions_initializer;
    if (s_.url.starts_with("ssl://") || s_.url.starts_with("wss://")) {
      if (!s_.ca_file.empty()) ssl.trustStore = s_.ca_file.c_str();
      if (!s_.cert_file.empty()) ssl.keyStore = s_.cert_file.c_str();
      if (!s_.key_file.empty()) ssl.privateKey = s_.key_file.c_str();
      ssl.enableServerCertAuth = s_.verify_server;
      ssl.verify = s_.verify_server;
      opts.ssl = &ssl;
    }
    // Sparkplug host STATE: the will says offline, published retained after connecting says online.
    MQTTAsync_willOptions will = MQTTAsync_willOptions_initializer;
    const std::string state_topic = "spBv1.0/STATE/" + s_.host_id;
    const std::uint64_t state_time = now_ms();
    const std::string offline = std::format(R"({{"online":false,"timestamp":{}}})", state_time);
    if (!s_.host_id.empty()) {
      will.topicName = state_topic.c_str();
      will.message = offline.c_str();
      will.qos = 1;
      will.retained = 1;
      opts.will = &will;
    }

    // Paho is never called with mu_ held: its thread takes mu_ in the callbacks.
    {
      std::lock_guard lock(mu_);
      pending_ = true;
    }
    int rc = MQTTAsync_connect(client_, &opts);
    if (rc != MQTTASYNC_SUCCESS) {
      host_.log.warn("MQTT connect to {} failed: {}", s_.url, MQTTAsync_strerror(rc));
      return status::BadNotConnected;
    }
    std::unique_lock lock(mu_);
    // Paho reports the outcome itself after connectTimeout, so wait a little longer.
    if (!cv_.wait_until(lock, deadline + std::chrono::seconds(2), [&] { return !pending_; })) {
      host_.log.warn("MQTT connect to {} timed out", s_.url);
      return status::BadTimeout;
    }
    if (!result_ok_) {
      host_.log.warn("MQTT connect to {} failed: {}", s_.url, result_text_);
      return status::BadNotConnected;
    }
    connected_ = true;
    std::vector<std::string> topics(filters_.begin(), filters_.end());
    topics.insert(topics.end(), spb_filters_.begin(), spb_filters_.end());
    // What was born before this session is unknown now: wait for (or ask for) new BIRTHs.
    for (auto& [_, e] : entities_) e.born = e.online = false;
    seq_.clear();
    lock.unlock();
    host_.log.info("connected to {} as {}", s_.url, s_.client_id);
    subscribe(std::move(topics));
    // Sparkplug: a host subscribes before it announces itself, so BIRTHs sent in reply are not missed.
    if (!s_.host_id.empty()) publish(state_topic, std::format(R"({{"online":true,"timestamp":{}}})", state_time), 1, true);
    return status::Good;
  }

  void disconnect() override {
    {
      std::lock_guard lock(mu_);
      connected_ = false;
    }
    if (client_ != nullptr && MQTTAsync_isConnected(client_)) {
      if (!s_.host_id.empty())
        publish("spBv1.0/STATE/" + s_.host_id, std::format(R"({{"online":false,"timestamp":{}}})", now_ms()), 1, true);
      MQTTAsync_disconnectOptions d = MQTTAsync_disconnectOptions_initializer;
      d.timeout = 1000;
      MQTTAsync_disconnect(client_, &d);
      for (int i = 0; i < 30 && MQTTAsync_isConnected(client_); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  bool connected() const override {
    std::lock_guard lock(mu_);
    return connected_;
  }

  sdk::StatusCode read(std::span<sdk::ReadItem*> items) override {
    std::lock_guard lock(mu_);
    if (!connected_) return status::BadNotConnected;
    const auto now = sdk::Clock::now();
    for (auto* item : items) {
      const auto& a = static_cast<const MqttAddress&>(*item->address);
      if (a.sparkplug) {
        read_metric(a, *item, now);
        continue;
      }
      auto it = latest_.find(a.topic);
      if (it == latest_.end()) {
        item->quality = status::BadWaitingForInitialData;
        continue;
      }
      auto v = decode(a, it->second.payload);
      item->source_time = it->second.time;
      if (!v) {
        item->quality = v.error().code;
        continue;
      }
      item->value = std::move(*v);
      item->quality = stale(it->second.time, now) ? status::UncertainLastUsableValue : status::Good;
    }
    return status::Good;
  }

  sdk::StatusCode write(const sdk::Address& address, const sdk::Value& value) override {
    const auto& a = static_cast<const MqttAddress&>(address);
    if (a.sparkplug) return write_metric(a, value);
    auto payload = encode(a, value);
    if (!payload) return payload.error().code;
    if (!connected()) return status::BadNotConnected;
    return publish(a.topic, *payload, s_.qos, s_.retain);
  }

 private:
  struct Message {
    std::string payload;
    sdk::Timestamp time;
  };
  struct MetricState {
    std::uint32_t datatype = 0;
    sdk::Result<sdk::Value> value = sdk::make_error(status::BadWaitingForInitialData, "no value yet");
    sdk::Timestamp time{};
  };
  // An edge node ("group/edge") or device ("group/edge/device").
  struct Entity {
    bool born = false, online = false;
    std::map<std::string, MetricState> metrics;
    std::map<std::uint64_t, std::string> aliases;
  };

  bool stale(sdk::Timestamp t, sdk::Timestamp now) const { return s_.stale.count() > 0 && now - t > s_.stale; }

  sdk::StatusCode publish(const std::string& topic, const std::string& payload, int qos, bool retain) {
    MQTTAsync_message msg = MQTTAsync_message_initializer;
    msg.payload = const_cast<char*>(payload.data());
    msg.payloadlen = static_cast<int>(payload.size());
    msg.qos = qos;
    msg.retained = retain;
    MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
    int rc = MQTTAsync_sendMessage(client_, topic.c_str(), &msg, &opts);
    if (rc != MQTTASYNC_SUCCESS) {
      host_.log.warn("publish to {} failed: {}", topic, MQTTAsync_strerror(rc));
      return status::BadCommunicationError;
    }
    // QoS 1/2: the write completes when the broker acknowledges it.
    if (qos > 0 && MQTTAsync_waitForCompletion(client_, opts.token, static_cast<unsigned long>(request_timeout_.count())) != MQTTASYNC_SUCCESS)
      return status::BadTimeout;
    return status::Good;
  }

  void subscribe(std::vector<std::string> topics) {
    if (topics.empty()) return;
    std::vector<char*> names;
    std::vector<int> qos(topics.size(), s_.qos);
    for (auto& t : topics) names.push_back(t.data());
    MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
    int rc = MQTTAsync_subscribeMany(client_, static_cast<int>(names.size()), names.data(), qos.data(), &opts);
    if (rc != MQTTASYNC_SUCCESS) host_.log.error("cannot subscribe to {} topic(s): {}", names.size(), MQTTAsync_strerror(rc));
    else host_.log.info("subscribed to {} topic(s)", names.size());
  }

  // ---- Sparkplug B ----------------------------------------------------------------

  // Caller holds mu_.
  void read_metric(const MqttAddress& a, sdk::ReadItem& item, sdk::Timestamp now) {
    auto e = entities_.find(a.entity());
    if (e == entities_.end() || (!e->second.born && e->second.metrics.empty())) {
      item.quality = status::BadWaitingForInitialData;
      return;
    }
    auto m = e->second.metrics.find(a.metric);
    if (m == e->second.metrics.end()) {
      item.quality = e->second.born ? status::BadOutOfRange : status::BadWaitingForInitialData;  // not in the BIRTH
      return;
    }
    item.source_time = m->second.time;
    if (!m->second.value) {
      item.quality = m->second.value.error().code;
      return;
    }
    auto v = a.type.scalar == sdk::DataType::Default ? *m->second.value : sdk::convert(*m->second.value, a.type.scalar);
    if (!v) {
      item.quality = v.error().code;
      return;
    }
    item.value = std::move(*v);
    item.quality = !e->second.online           ? status::UncertainNoCommunicationLastUsableValue
                   : stale(m->second.time, now) ? status::UncertainLastUsableValue
                                                : status::Good;
  }

  sdk::StatusCode write_metric(const MqttAddress& a, const sdk::Value& value) {
    spb::Metric m;
    {
      std::lock_guard lock(mu_);
      if (!connected_) return status::BadNotConnected;
      auto e = entities_.find(a.entity());
      if (e == entities_.end() || !e->second.online) return status::BadNoCommunication;
      auto it = e->second.metrics.find(a.metric);
      if (it == e->second.metrics.end()) return status::BadOutOfRange;
      m.datatype = it->second.datatype;
    }
    m.name = a.metric;
    m.has_name = true;
    m.timestamp = now_ms();
    if (auto st = spb::set_value(m, m.datatype, value); !sdk::is_good(st)) return st;
    spb::Payload p;
    p.timestamp = m.timestamp;
    p.metrics.push_back(std::move(m));
    const auto bytes = spb::encode(p);
    const auto topic = spb::make_topic(a.group, a.device.empty() ? spb::MsgType::NCmd : spb::MsgType::DCmd, a.edge, a.device);
    return publish(topic, std::string(bytes.begin(), bytes.end()), 0, false);  // Sparkplug commands use QoS 0
  }

  // Applies the metrics of a BIRTH or DATA message; false if one is unknown.
  static bool apply(Entity& e, const spb::Payload& p, bool birth, sdk::Timestamp arrived) {
    bool known = true;
    for (const auto& m : p.metrics) {
      std::string name = m.name;
      if (!m.has_name) {
        auto alias = e.aliases.find(m.alias);
        if (!m.has_alias || alias == e.aliases.end()) {
          known = false;
          continue;
        }
        name = alias->second;
      }
      if (birth && m.has_alias) e.aliases[m.alias] = name;
      auto it = e.metrics.find(name);
      if (it == e.metrics.end()) {
        if (!birth) {
          known = false;
          continue;
        }
        it = e.metrics.emplace(name, MetricState{}).first;
      }
      auto& st = it->second;
      if (m.datatype != 0) st.datatype = m.datatype;
      const std::uint64_t ms = m.timestamp ? m.timestamp : p.timestamp;
      st.time = ms ? sdk::Timestamp(std::chrono::milliseconds(ms)) : arrived;
      if (m.is_null) st.value = sdk::make_error(status::Bad, "null value");
      else st.value = spb::value(m, st.datatype);
    }
    return known;
  }

  // Caller holds mu_. Returns the edge node to ask for a rebirth, if any.
  std::optional<spb::Topic> on_sparkplug(const spb::Topic& t, const std::string& payload, sdk::Timestamp arrived) {
    using spb::MsgType;
    if (t.type == MsgType::NCmd || t.type == MsgType::DCmd) return std::nullopt;
    const std::string node = t.group + "/" + t.edge;
    const std::string key = t.device.empty() ? node : node + "/" + t.device;
    auto p = spb::decode(std::span(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
    if (!p) {
      host_.log.warn("bad Sparkplug payload from {}: {}", key, p.error().message);
      return std::nullopt;
    }
    bool rebirth = false;
    // Sequence numbers run 0-255 over all messages of a node and its devices; DEATHs carry none.
    if (t.type == MsgType::NBirth) {
      seq_[node] = p->seq.value_or(0);
    } else if (t.type != MsgType::NDeath && p->seq) {
      auto s = seq_.find(node);
      if (s != seq_.end() && *p->seq != (s->second + 1) % 256) {
        host_.log.warn("Sparkplug node {}: sequence {} after {}", node, *p->seq, s->second);
        rebirth = true;
      }
      seq_[node] = *p->seq;
    }
    auto offline = [&](const std::string& prefix, bool with_devices) {
      for (auto& [k, e] : entities_)
        if (k == prefix || (with_devices && k.starts_with(prefix + "/"))) e.online = false;
    };
    switch (t.type) {
      case MsgType::NBirth: {
        offline(node, true);  // devices must be born again after their node
        auto& e = entities_[node];
        e.metrics.clear();
        e.aliases.clear();
        e.born = e.online = true;
        apply(e, *p, true, arrived);
        break;
      }
      case MsgType::DBirth: {
        auto n = entities_.find(node);
        if (n == entities_.end() || !n->second.online) rebirth = true;
        auto& e = entities_[key];
        e.metrics.clear();
        e.aliases.clear();
        e.born = e.online = true;
        apply(e, *p, true, arrived);
        break;
      }
      case MsgType::NData:
      case MsgType::DData: {
        auto e = entities_.find(key);
        if (e == entities_.end() || !e->second.online) {
          rebirth = true;
          break;
        }
        if (!apply(e->second, *p, false, arrived)) rebirth = true;
        break;
      }
      case MsgType::NDeath: offline(node, true); break;
      case MsgType::DDeath: offline(key, false); break;
      default: break;
    }
    if (!rebirth) return std::nullopt;
    auto& last = rebirth_sent_[node];
    const auto now = sdk::SteadyClock::now();
    if (now - last < kRebirthInterval) return std::nullopt;
    last = now;
    return t;
  }

  void request_rebirth(const spb::Topic& t) {
    spb::Metric m;
    m.name = "Node Control/Rebirth";
    m.has_name = true;
    m.timestamp = now_ms();
    spb::set_value(m, spb::Boolean, sdk::Value(true));
    spb::Payload p;
    p.timestamp = m.timestamp;
    p.metrics.push_back(std::move(m));
    const auto bytes = spb::encode(p);
    host_.log.info("asking Sparkplug node {}/{} to rebirth", t.group, t.edge);
    publish(spb::make_topic(t.group, spb::MsgType::NCmd, t.edge), std::string(bytes.begin(), bytes.end()), 0, false);
  }

  // ---- Paho callbacks ---------------------------------------------------------------

  void done(bool ok, std::string text) {
    {
      std::lock_guard lock(mu_);
      pending_ = false;
      result_ok_ = ok;
      result_text_ = std::move(text);
    }
    cv_.notify_all();
  }

  static void on_lost(void* ctx, char* cause) {
    auto* self = static_cast<MqttDevice*>(ctx);
    {
      std::lock_guard lock(self->mu_);
      self->connected_ = false;
    }
    self->host_.log.warn("MQTT connection lost: {}", cause ? cause : "unknown");
  }

  static int on_message(void* ctx, char* topic, int topic_len, MQTTAsync_message* m) {
    auto* self = static_cast<MqttDevice*>(ctx);
    std::string t = topic_len > 0 ? std::string(topic, static_cast<std::size_t>(topic_len)) : std::string(topic);
    Message msg{std::string(static_cast<const char*>(m->payload), static_cast<std::size_t>(m->payloadlen)), sdk::Clock::now()};
    MQTTAsync_freeMessage(&m);
    MQTTAsync_free(topic);
    std::optional<spb::Topic> rebirth;
    {
      std::lock_guard lock(self->mu_);
      for (const auto& f : self->filters_)
        if (topic_matches(f, t)) self->latest_[f] = msg;
      if (t.starts_with("spBv1.0/") && std::ranges::any_of(self->spb_filters_, [&](const auto& f) { return topic_matches(f, t); }))
        if (auto st = spb::parse_topic(t)) rebirth = self->on_sparkplug(*st, msg.payload, msg.time);
    }
    if (rebirth) self->request_rebirth(*rebirth);
    return 1;
  }

  Settings s_;
  std::chrono::milliseconds request_timeout_;
  sdk::HostContext& host_;
  MQTTAsync client_ = nullptr;

  mutable std::mutex mu_;  // guards everything below (Paho calls back on its own thread)
  std::condition_variable cv_;
  bool connected_ = false, pending_ = false, result_ok_ = false;
  std::string result_text_;
  std::set<std::string> filters_, spb_filters_;
  std::map<std::string, Message> latest_;  // by topic filter
  std::map<std::string, Entity> entities_;
  std::map<std::string, std::uint64_t> seq_;  // last sequence number per edge node
  std::map<std::string, sdk::SteadyClock::time_point> rebirth_sent_;
};

nlohmann::json prop(const char* key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", std::string(kId) + "." + key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

class MqttClientDriver final : public sdk::Driver {
 public:
  sdk::DriverInfo info() const override {
    return {kId, "MQTT Client", "0.1.0", sdk::Medium::Ethernet, {"MQTT 3.1.1", "MQTT 5", "Sparkplug B"}};
  }

  nlohmann::json schema() const override {
    using nlohmann::json;
    return {
        {"driver", kId},
        {"display_name", "MQTT Client"},
        {"channel", json::array()},
        {"device",
         json::array({
             {{"key", "servermain.DEVICE_ID_STRING"}, {"type", "string"}, {"default", "tcp://127.0.0.1:1883"},
              {"description", "Broker URL: tcp://, ssl://, ws:// or wss://"}},
             prop("DEVICE_CLIENT_ID", "string", "", "Client ID (empty: logict-<channel>-<device>)"),
             prop("DEVICE_USERNAME", "string", "", "User name"),
             prop("DEVICE_PASSWORD", "string", "", "Password"),
             prop("DEVICE_QOS", "integer", 1, "QoS for subscriptions and writes (0-2)"),
             prop("DEVICE_RETAIN", "boolean", false, "Publish writes as retained messages"),
             prop("DEVICE_KEEP_ALIVE_SECONDS", "integer", 60, "Keep-alive interval"),
             prop("DEVICE_CLEAN_SESSION", "boolean", true, "Clean session / clean start"),
             prop("DEVICE_PROTOCOL_VERSION", "enum", "3.1.1", "3.1.1 or 5"),
             prop("DEVICE_STALE_TIMEOUT_SECONDS", "integer", 0, "Mark values older than this Uncertain (0 = never)"),
             prop("DEVICE_SPARKPLUG_HOST_ID", "string", "", "Sparkplug host application ID; publishes spBv1.0/STATE/<id> when set"),
             prop("DEVICE_TLS_CA_FILE", "string", "", "CA certificates (PEM) to verify the broker"),
             prop("DEVICE_TLS_CERT_FILE", "string", "", "Client certificate (PEM)"),
             prop("DEVICE_TLS_KEY_FILE", "string", "", "Client private key (PEM)"),
             prop("DEVICE_TLS_VERIFY_SERVER", "boolean", true, "Verify the broker certificate and host name"),
         })},
        {"address_help",
         "<topic> (whole payload) or <topic>|<json path> (e.g. plant/line1|data.temp, plant/line1|$.values[2]); "
         "+ and # subscribe to many topics (read-only); arrays \"plant/arr [10]\". Sparkplug B: "
         "spb:<group>/<edge node>[/<device>]|<metric>. Writes publish to the topic (Sparkplug: NCMD/DCMD)."},
    };
  }

  sdk::Result<std::unique_ptr<sdk::DeviceRuntime>> create_device(const sdk::ChannelConfig& channel,
                                                                 const sdk::DeviceConfig& device,
                                                                 sdk::HostContext& host) override {
    const auto& p = device.properties;
    auto key = [](const char* k) { return std::string(kId) + "." + k; };
    Settings s;
    s.url = device.id;
    if (s.url.starts_with("mqtt://")) s.url = "tcp://" + s.url.substr(7);
    if (s.url.starts_with("mqtts://")) s.url = "ssl://" + s.url.substr(8);
    if (!(s.url.starts_with("tcp://") || s.url.starts_with("ssl://") || s.url.starts_with("ws://") || s.url.starts_with("wss://")))
      return sdk::make_error(status::BadConfigurationError, "device ID must be a broker URL such as tcp://host:1883");
    s.client_id = sdk::property<std::string>(p, key("DEVICE_CLIENT_ID"), "");
    if (s.client_id.empty()) s.client_id = "logict-" + channel.name + "-" + device.name;
    s.username = sdk::property<std::string>(p, key("DEVICE_USERNAME"), "");
    s.password = sdk::property<std::string>(p, key("DEVICE_PASSWORD"), "");
    s.qos = std::clamp(sdk::property(p, key("DEVICE_QOS"), 1), 0, 2);
    s.retain = sdk::property(p, key("DEVICE_RETAIN"), false);
    s.keep_alive = std::max(1, sdk::property(p, key("DEVICE_KEEP_ALIVE_SECONDS"), 60));
    s.clean_session = sdk::property(p, key("DEVICE_CLEAN_SESSION"), true);
    const std::string version = sdk::property<std::string>(p, key("DEVICE_PROTOCOL_VERSION"), "3.1.1");
    if (version != "3.1.1" && version != "5") return sdk::make_error(status::BadConfigurationError, "protocol version must be 3.1.1 or 5");
    s.version = version == "5" ? 5 : 4;
    s.stale = std::chrono::seconds(std::max(0, sdk::property(p, key("DEVICE_STALE_TIMEOUT_SECONDS"), 0)));
    s.host_id = sdk::property<std::string>(p, key("DEVICE_SPARKPLUG_HOST_ID"), "");
    if (s.host_id.find_first_of("/+#") != std::string::npos)
      return sdk::make_error(status::BadConfigurationError, "Sparkplug host ID must not contain '/', '+' or '#'");
    s.ca_file = sdk::property<std::string>(p, key("DEVICE_TLS_CA_FILE"), "");
    s.cert_file = sdk::property<std::string>(p, key("DEVICE_TLS_CERT_FILE"), "");
    s.key_file = sdk::property<std::string>(p, key("DEVICE_TLS_KEY_FILE"), "");
    s.verify_server = sdk::property(p, key("DEVICE_TLS_VERIFY_SERVER"), true);
    return std::make_unique<MqttDevice>(std::move(s), device.request_timeout, host);
  }
};

}  // namespace

LOGICT_DECLARE_DRIVER(MqttClientDriver)
