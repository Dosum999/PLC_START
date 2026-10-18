#include "mqtt/mqtt_agent.h"

#include <MQTTAsync.h>

#include <algorithm>
#include <format>
#include <optional>
#include <set>

#include "util/log.h"

namespace logict::mqtt {

namespace status = sdk::status;
using core::TagId;
using Steady = std::chrono::steady_clock;
using nlohmann::json;

// State shared between the agent thread, Paho callback threads and channel
// threads completing writes. Nothing here calls into Paho.
struct MqttAgent::Shared {
  struct Outgoing {
    std::string topic, payload, correlation;
    int qos = 0;
  };
  struct ConnectResult {
    bool ok;
    std::string reason;
  };

  std::mutex mu;
  std::condition_variable cv;
  bool stopping = false;
  std::deque<Outgoing> outbox;                  // write responses
  std::optional<ConnectResult> connect_result;  // from Paho connect callbacks
  bool lost = false;                            // from Paho connectionLost

  void post(Outgoing o) {
    {
      std::lock_guard lock(mu);
      if (stopping) return;
      outbox.push_back(std::move(o));
    }
    cv.notify_all();
  }
};

namespace {

// ---- Paho C trampolines ---------------------------------------------------------

void cb_connected_v3(void* ctx, MQTTAsync_successData*) { static_cast<MqttAgent*>(ctx)->on_connected(true, ""); }
void cb_connected_v5(void* ctx, MQTTAsync_successData5*) { static_cast<MqttAgent*>(ctx)->on_connected(true, ""); }
void cb_failed_v3(void* ctx, MQTTAsync_failureData* d) {
  static_cast<MqttAgent*>(ctx)->on_connected(false, d && d->message ? d->message : std::format("code {}", d ? d->code : 0));
}
void cb_failed_v5(void* ctx, MQTTAsync_failureData5* d) {
  std::string why = d && d->message ? d->message : d ? MQTTReasonCode_toString(d->reasonCode) : "unknown";
  static_cast<MqttAgent*>(ctx)->on_connected(false, why);
}
void cb_lost(void* ctx, char* cause) { static_cast<MqttAgent*>(ctx)->on_connection_lost(cause ? cause : "connection lost"); }

int cb_message(void* ctx, char* topic, int topic_len, MQTTAsync_message* m) {
  std::string t = topic_len > 0 ? std::string(topic, static_cast<std::size_t>(topic_len)) : std::string(topic);
  std::string payload(static_cast<const char*>(m->payload), static_cast<std::size_t>(m->payloadlen));
  std::string response_topic, correlation;
  if (auto* p = MQTTProperties_getProperty(&m->properties, MQTTPROPERTY_CODE_RESPONSE_TOPIC))
    response_topic.assign(p->value.data.data, static_cast<std::size_t>(p->value.data.len));
  if (auto* p = MQTTProperties_getProperty(&m->properties, MQTTPROPERTY_CODE_CORRELATION_DATA))
    correlation.assign(p->value.data.data, static_cast<std::size_t>(p->value.data.len));
  MQTTAsync_freeMessage(&m);
  MQTTAsync_free(topic);
  static_cast<MqttAgent*>(ctx)->on_message(t, payload, response_topic, correlation);
  return 1;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
  for (std::size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size()) s.replace(pos, from.size(), to);
  return s;
}

std::string topic_safe(std::string s) {
  std::replace(s.begin(), s.end(), '+', '_');
  std::replace(s.begin(), s.end(), '#', '_');
  return s;
}

std::string client_id_safe(std::string s) {
  for (auto& c : s)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
  return s;
}

sdk::Result<sdk::Value> value_from_json(const json& j) {
  switch (j.type()) {
    case json::value_t::boolean: return sdk::Value(j.get<bool>());
    case json::value_t::number_integer: return sdk::Value(j.get<std::int64_t>());
    case json::value_t::number_unsigned: return sdk::Value(j.get<std::uint64_t>());
    case json::value_t::number_float: return sdk::Value(j.get<double>());
    case json::value_t::string: return sdk::Value(j.get<std::string>());
    case json::value_t::array: {
      sdk::Array arr;
      for (const auto& e : j) {
        if (e.is_array() || e.is_object()) return sdk::make_error(status::BadTypeMismatch, "nested arrays are not supported");
        auto v = value_from_json(e);
        if (!v) return v;
        arr.push_back(std::move(*v));
      }
      return sdk::Value(std::move(arr));
    }
    default: return sdk::make_error(status::BadTypeMismatch, "unsupported JSON value");
  }
}

}  // namespace

// ---- lifecycle --------------------------------------------------------------------

MqttAgent::MqttAgent(core::Runtime& runtime, AgentConfig config, std::string server_name)
    : rt_(runtime),
      cfg_(std::move(config)),
      server_name_(std::move(server_name)),
      template_(*(cfg_.advanced ? Template::advanced(cfg_.advanced_template)
                                : Template::standard(cfg_.standard_template, cfg_.expansion))),
      log_(core::make_logger("mqtt:" + cfg_.name)),
      shared_(std::make_shared<Shared>()) {
  uri_ = cfg_.url;
  if (uri_.starts_with("mqtt://")) uri_ = "tcp://" + uri_.substr(7);
  if (uri_.starts_with("mqtts://")) uri_ = "ssl://" + uri_.substr(8);
  client_id_ = cfg_.client_id.empty() ? client_id_safe(std::format("logict-{}-{}", server_name_, cfg_.name)) : cfg_.client_id;
}

MqttAgent::~MqttAgent() { stop(); }

std::vector<std::string> MqttAgent::prepare() {
  std::vector<std::string> warnings;
  auto& db = rt_.tags();
  for (const auto& ic : cfg_.items) {
    if (!ic.enabled) continue;
    TagId id = db.find(ic.server_tag);
    if (id == core::kInvalidTag) {
      warnings.push_back(std::format("MQTT agent '{}': IoT item '{}' refers to unknown tag '{}'", cfg_.name, ic.name, ic.server_tag));
      continue;
    }
    const auto& info = db.info(id);
    Item item;
    item.cfg = ic;
    item.tag = id;
    const double deadband = ic.deadband_absolute > 0 ? ic.deadband_absolute
                                                     : ChangeFilter::percent_threshold(info.type.scalar, ic.deadband_percent);
    item.filter = ChangeFilter(cfg_.send_initial, ic.every_scan, cfg_.ignore_quality_changes, deadband);
    std::string path = info.path;
    std::replace(path.begin(), path.end(), '.', '/');
    std::string t = cfg_.tag_topic;
    t = replace_all(t, "{topic}", cfg_.topic);
    t = replace_all(t, "{agent}", cfg_.name);
    t = replace_all(t, "{path}", path);
    t = replace_all(t, "{channel}", info.channel);
    t = replace_all(t, "{device}", info.device);
    t = replace_all(t, "{tag}", info.config.name);
    item.topic = topic_safe(std::move(t));
    item_by_tag_[info.path] = items_.size();
    items_.push_back(std::move(item));
  }

  auto add = [&](const std::string& name, sdk::DataType type, sdk::Value v) {
    core::TagInfo info;
    info.path = std::format("_IoT_Gateway.{}.{}", cfg_.name, name);
    info.kind = core::TagKind::System;
    info.raw_type = info.type = sdk::TagType{type};
    info.config.name = name;
    info.config.type = info.type;
    info.config.access = sdk::Access::ReadOnly;
    TagId id = db.add(std::move(info));
    auto now = sdk::Clock::now();
    db.update(id, sdk::Vqt{std::move(v), status::Good, now, now});
    return id;
  };
  t_publishes_ = add("_PublishesSent", sdk::DataType::DWord, 0u);
  t_dropped_ = add("_DroppedEvents", sdk::DataType::DWord, 0u);
  t_buffered_ = add("_BufferedEvents", sdk::DataType::DWord, 0u);
  t_connected_ = add("_Connected", sdk::DataType::Boolean, false);
  return warnings;
}

sdk::Result<void> MqttAgent::start() {
  if (thread_.joinable()) return {};
  MQTTAsync_createOptions create = MQTTAsync_createOptions_initializer;
  if (cfg_.protocol == 5) create.MQTTVersion = MQTTVERSION_5;
  MQTTAsync client = nullptr;
  int rc = MQTTAsync_createWithOptions(&client, uri_.c_str(), client_id_.c_str(), MQTTCLIENT_PERSISTENCE_NONE, nullptr,
                                       &create);
  if (rc != MQTTASYNC_SUCCESS)
    return sdk::make_error(status::BadConfigurationError, std::format("cannot create MQTT client for {}: {}", uri_, MQTTAsync_strerror(rc)));
  MQTTAsync_setCallbacks(client, this, cb_lost, cb_message, nullptr);
  client_ = client;

  auto& db = rt_.tags();
  sub_ = db.add_subscriber([](TagId, const sdk::Vqt&, bool) {});
  const auto now = Steady::now();
  for (auto& item : items_) {
    db.subscribe(item.tag, sub_, item.cfg.scan_rate_ms);
    item.next_due = now;
  }
  {
    std::lock_guard lock(shared_->mu);
    shared_->stopping = false;
  }
  next_connect_ = now;
  thread_ = std::thread([this] { run(); });
  log_->info("MQTT agent started: {} ({} item(s), client id {})", uri_, items_.size(), client_id_);
  return {};
}

void MqttAgent::stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard lock(shared_->mu);
    shared_->stopping = true;  // also makes late write completions no-ops
  }
  shared_->cv.notify_all();
  thread_.join();
  rt_.tags().remove_subscriber(sub_);

  if (connected_ && cfg_.lwt_enabled) {
    // Kepware announces a clean shutdown on the LWT topic (the broker does not
    // publish the will after a normal disconnect).
    MQTTAsync_message msg = MQTTAsync_message_initializer;
    std::string text = "Server is shutting down.";
    msg.payload = text.data();
    msg.payloadlen = static_cast<int>(text.size());
    msg.qos = 1;
    msg.retained = cfg_.lwt_retain;
    MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
    if (MQTTAsync_sendMessage(client_, cfg_.lwt_topic.c_str(), &msg, &opts) == MQTTASYNC_SUCCESS)
      MQTTAsync_waitForCompletion(client_, opts.token, 2000);
  }
  if (MQTTAsync_isConnected(client_)) {
    MQTTAsync_disconnectOptions d = MQTTAsync_disconnectOptions_initializer;
    d.timeout = 2000;
    MQTTAsync_disconnect(client_, &d);
    for (int i = 0; i < 40 && MQTTAsync_isConnected(client_); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  MQTTAsync client = client_;
  MQTTAsync_destroy(&client);
  client_ = nullptr;
  connected_ = false;
  update_system_tags();
  log_->info("MQTT agent stopped ({} event(s) left in buffer)", buffer_.size());
}

// ---- connection -------------------------------------------------------------------

void MqttAgent::connect(MQTTAsync_handle client) {
  connecting_ = true;
  MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
  if (cfg_.protocol == 5) {
    MQTTAsync_connectOptions v5 = MQTTAsync_connectOptions_initializer5;
    opts = v5;
    opts.onSuccess5 = cb_connected_v5;
    opts.onFailure5 = cb_failed_v5;
  } else {
    opts.onSuccess = cb_connected_v3;
    opts.onFailure = cb_failed_v3;
  }
  opts.context = this;
  opts.keepAliveInterval = 60;
  opts.connectTimeout = static_cast<int>(cfg_.timeout_s);
  opts.automaticReconnect = 0;  // reconnects are driven by run()
  if (!cfg_.username.empty()) opts.username = cfg_.username.c_str();
  if (!cfg_.password.empty()) opts.password = cfg_.password.c_str();

  MQTTAsync_willOptions will = MQTTAsync_willOptions_initializer;
  if (cfg_.lwt_enabled) {
    will.topicName = cfg_.lwt_topic.c_str();
    will.message = cfg_.lwt_message.c_str();
    will.qos = 1;
    will.retained = cfg_.lwt_retain;
    opts.will = &will;
  }
  MQTTAsync_SSLOptions ssl = MQTTAsync_SSLOptions_initializer;
  if (uri_.starts_with("ssl://")) {
    if (!cfg_.ca_file.empty()) ssl.trustStore = cfg_.ca_file.c_str();
    if (!cfg_.cert_file.empty()) ssl.keyStore = cfg_.cert_file.c_str();
    if (!cfg_.key_file.empty()) ssl.privateKey = cfg_.key_file.c_str();
    if (!cfg_.key_password.empty()) ssl.privateKeyPassword = cfg_.key_password.c_str();
    ssl.enableServerCertAuth = cfg_.verify_server;
    ssl.verify = cfg_.verify_server;
    ssl.sslVersion = cfg_.tls_version == 1 ? MQTT_SSL_VERSION_TLS_1_2 : MQTT_SSL_VERSION_DEFAULT;
    opts.ssl = &ssl;
  }
  int rc = MQTTAsync_connect(client, &opts);
  if (rc != MQTTASYNC_SUCCESS) handle_connect_result(false, MQTTAsync_strerror(rc));
}

void MqttAgent::on_connected(bool ok, const std::string& reason) {
  {
    std::lock_guard lock(shared_->mu);
    shared_->connect_result = Shared::ConnectResult{ok, reason};
  }
  shared_->cv.notify_all();
}

void MqttAgent::on_connection_lost(const std::string& cause) {
  connected_ = false;  // stop publishing right away
  log_->warn("connection to {} lost: {}", uri_, cause);
  {
    std::lock_guard lock(shared_->mu);
    shared_->lost = true;
  }
  shared_->cv.notify_all();
}

void MqttAgent::handle_connect_result(bool ok, const std::string& reason) {
  connecting_ = false;
  if (!ok) {
    log_->warn("connect to {} failed: {} (retry in {} s)", uri_, reason, backoff_.count());
    next_connect_ = Steady::now() + backoff_;
    backoff_ = std::min(backoff_ * 2, std::chrono::seconds(30));
    return;
  }
  log_->info("connected to {}", uri_);
  backoff_ = std::chrono::seconds(1);
  if (cfg_.write_enabled) {
    MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
    int rc = MQTTAsync_subscribe(client_, cfg_.write_topic.c_str(), cfg_.write_qos, &opts);
    if (rc != MQTTASYNC_SUCCESS) log_->error("cannot subscribe to {}: {}", cfg_.write_topic, MQTTAsync_strerror(rc));
  }
  connected_ = true;
  if (cfg_.lwt_enabled) send(cfg_.lwt_topic, "Server is online.", 1, cfg_.lwt_retain);
}

void MqttAgent::flush_outbox() {
  std::deque<Shared::Outgoing> out;
  {
    std::lock_guard lock(shared_->mu);
    out.swap(shared_->outbox);
  }
  for (auto& o : out) {
    if (!connected_) {
      log_->warn("write response to {} dropped: not connected", o.topic);
      continue;
    }
    MQTTAsync_message msg = MQTTAsync_message_initializer;
    msg.payload = o.payload.data();
    msg.payloadlen = static_cast<int>(o.payload.size());
    msg.qos = o.qos;
    MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
    if (cfg_.protocol == 5 && !o.correlation.empty()) {
      MQTTProperty p;
      p.identifier = MQTTPROPERTY_CODE_CORRELATION_DATA;
      p.value.data.data = o.correlation.data();
      p.value.data.len = static_cast<int>(o.correlation.size());
      MQTTProperties_add(&msg.properties, &p);
    }
    int rc = MQTTAsync_sendMessage(client_, o.topic.c_str(), &msg, &opts);
    MQTTProperties_free(&msg.properties);
    if (rc != MQTTASYNC_SUCCESS) log_->warn("write response to {} failed: {}", o.topic, MQTTAsync_strerror(rc));
  }
}

// ---- main loop ----------------------------------------------------------------------

void MqttAgent::run() {
  auto next_publish = Steady::now() + std::chrono::milliseconds(cfg_.rate_ms);
  while (true) {
    std::optional<Shared::ConnectResult> result;
    bool lost;
    {
      std::lock_guard lock(shared_->mu);
      if (shared_->stopping) break;
      result.swap(shared_->connect_result);
      lost = std::exchange(shared_->lost, false);
    }
    if (result) handle_connect_result(result->ok, result->reason);
    if (lost && !connecting_) next_connect_ = Steady::now() + std::chrono::seconds(1);

    auto now = Steady::now();
    if (!connected_ && !connecting_ && now >= next_connect_) connect(client_);
    flush_outbox();

    for (auto& item : items_)
      if (now >= item.next_due) {
        sample(item);
        item.next_due += std::chrono::milliseconds(item.cfg.scan_rate_ms);
        if (item.next_due <= now) item.next_due = now + std::chrono::milliseconds(item.cfg.scan_rate_ms);
      }

    if (cfg_.publish_type == AgentConfig::PublishType::OnDataChange) {
      if (!buffer_.empty()) publish_narrow();
    } else if (now >= next_publish) {
      if (cfg_.format == AgentConfig::Format::Wide) publish_wide();
      else publish_narrow();
      next_publish += std::chrono::milliseconds(cfg_.rate_ms);
      if (next_publish <= now) next_publish = now + std::chrono::milliseconds(cfg_.rate_ms);
    }
    update_system_tags();

    auto wake = now + std::chrono::seconds(1);
    for (const auto& item : items_) wake = std::min(wake, item.next_due);
    if (cfg_.publish_type == AgentConfig::PublishType::Interval) wake = std::min(wake, next_publish);
    if (!connected_ && !connecting_) wake = std::min(wake, next_connect_);
    std::unique_lock lock(shared_->mu);
    shared_->cv.wait_until(lock, wake, [this] {
      return shared_->stopping || shared_->connect_result || shared_->lost || !shared_->outbox.empty();
    });
  }
}

void MqttAgent::sample(Item& item) {
  sdk::Vqt v = rt_.tags().read(item.tag);
  if (v.quality == status::BadWaitingForInitialData) return;
  item.latest = v;
  item.has_latest = true;
  if (cfg_.format == AgentConfig::Format::Wide) return;
  if (!item.filter.accept(v)) return;
  if (buffer_.size() >= cfg_.buffer_size) {
    ++dropped_;  // Kepware: keep the oldest data, drop new events
    return;
  }
  buffer_.push_back(Event{static_cast<std::size_t>(&item - items_.data()), std::move(v)});
}

std::string MqttAgent::render(const std::vector<const Item*>& items, const std::vector<const sdk::Vqt*>& values) const {
  const auto& db = rt_.tags();
  std::vector<EventView> views;
  views.reserve(items.size());
  for (std::size_t i = 0; i < items.size(); ++i) {
    const auto& info = db.info(items[i]->tag);
    views.push_back(EventView{info.path, info.channel, info.device, info.config.name, info.config.address,
                              info.config.description, info.config.access == sdk::Access::ReadWrite, values[i],
                              &info.type});
  }
  return template_.render(server_name_, sdk::Clock::now(), views);
}

void MqttAgent::publish_narrow() {
  if (!connected_) return;
  const bool per_tag = cfg_.topic_mode == AgentConfig::TopicMode::PerTag;
  while (!buffer_.empty()) {
    const std::size_t n = per_tag ? 1 : std::min<std::size_t>(cfg_.max_events, buffer_.size());
    std::vector<const Item*> items;
    std::vector<const sdk::Vqt*> values;
    for (std::size_t i = 0; i < n; ++i) {
      items.push_back(&items_[buffer_[i].item]);
      values.push_back(&buffer_[i].vqt);
    }
    const std::string& topic = per_tag ? items.front()->topic : cfg_.topic;
    if (!send(topic, render(items, values), cfg_.qos, cfg_.retain)) return;  // keep buffered; retry later
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(n));
  }
}

void MqttAgent::publish_wide() {
  if (!connected_) return;  // Kepware: wide format is not buffered
  std::vector<const Item*> items;
  std::vector<const sdk::Vqt*> values;
  for (const auto& item : items_)
    if (item.has_latest) {
      items.push_back(&item);
      values.push_back(&item.latest);
    }
  if (items.empty()) return;
  if (cfg_.topic_mode == AgentConfig::TopicMode::PerTag) {
    for (std::size_t i = 0; i < items.size(); ++i)
      if (!send(items[i]->topic, render({items[i]}, {values[i]}), cfg_.qos, cfg_.retain)) return;
  } else {
    send(cfg_.topic, render(items, values), cfg_.qos, cfg_.retain);
  }
}

bool MqttAgent::send(const std::string& topic, const std::string& payload, int qos, bool retain) {
  if (!client_) return false;
  MQTTAsync_message msg = MQTTAsync_message_initializer;
  msg.payload = const_cast<char*>(payload.data());
  msg.payloadlen = static_cast<int>(payload.size());
  msg.qos = qos;
  msg.retained = retain;
  MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
  int rc = MQTTAsync_sendMessage(client_, topic.c_str(), &msg, &opts);
  if (rc != MQTTASYNC_SUCCESS) {
    log_->debug("publish to {} failed: {}", topic, MQTTAsync_strerror(rc));
    return false;
  }
  ++publishes_;
  return true;
}

void MqttAgent::update_system_tags() {
  auto& db = rt_.tags();
  auto now = sdk::Clock::now();
  auto set = [&](TagId id, sdk::Value v) {
    if (id != core::kInvalidTag && !(db.read(id).value == v)) db.update(id, sdk::Vqt{std::move(v), status::Good, now, now});
  };
  set(t_publishes_, publishes_);
  set(t_dropped_, dropped_);
  set(t_buffered_, static_cast<std::uint32_t>(buffer_.size()));
  set(t_connected_, connected_.load());
}

// ---- writes -------------------------------------------------------------------------

void MqttAgent::on_message(const std::string& topic, const std::string& payload, const std::string& response_topic,
                           const std::string& correlation) {
  if (!cfg_.write_enabled || topic != cfg_.write_topic) return;
  handle_write(payload, response_topic.empty() ? cfg_.write_response_topic : response_topic, correlation);
}

void MqttAgent::handle_write(const std::string& payload, std::string response_topic, std::string correlation) {
  json req = json::parse(payload, nullptr, false);
  const bool single = req.is_object();
  if (single) req = json::array({req});
  if (!req.is_array() || req.empty()) {
    log_->warn("failed to process write request on topic {}: invalid write format", cfg_.write_topic);
    if (!response_topic.empty())
      shared_->post({response_topic, R"({"error":"invalid write format"})", correlation, cfg_.write_qos});
    return;
  }

  // Collects per-tag results; the last completion publishes the response.
  struct Batch {
    std::mutex mu;
    json results;
    std::size_t remaining;
  };
  auto batch = std::make_shared<Batch>();
  batch->results = json::array();
  batch->remaining = req.size();
  for (std::size_t i = 0; i < req.size(); ++i) batch->results.push_back(json::object());

  auto finish = [batch, shared = shared_, log = log_, response_topic, correlation, single, qos = cfg_.write_qos](
                    std::size_t index, const std::string& id, sdk::StatusCode st) {
    if (!sdk::is_good(st)) log->warn("write to '{}' failed: {}", id, sdk::status_name(st));
    std::string body;
    {
      std::lock_guard lock(batch->mu);
      batch->results[index] = {{"id", id}, {"result", std::string(sdk::status_name(st))}, {"code", st}};
      if (--batch->remaining > 0) return;
      body = (single ? batch->results[0] : batch->results).dump();
    }
    if (!response_topic.empty()) shared->post({response_topic, std::move(body), correlation, qos});
  };

  for (std::size_t i = 0; i < req.size(); ++i) {
    const json& e = req[i];
    const std::string id = e.is_object() ? e.value("id", "") : "";
    if (id.empty() || !e.contains("v")) {
      finish(i, id, status::BadInvalidArgument);
      continue;
    }
    auto it = item_by_tag_.find(id);
    if (it == item_by_tag_.end()) {
      // Kepware: only tags configured on this agent may be written.
      finish(i, id, status::BadNodeIdUnknown);
      continue;
    }
    auto value = value_from_json(e.at("v"));
    if (!value) {
      finish(i, id, value.error().code);
      continue;
    }
    rt_.write_async(items_[it->second].tag, *value, [finish, i, id](sdk::StatusCode st) { finish(i, id, st); });
  }
}

// ---- gateway --------------------------------------------------------------------------

MqttGateway::MqttGateway(core::Runtime& runtime, const nlohmann::json& iot_gateway, std::string server_name) {
  auto it = iot_gateway.find("mqtt_clients");
  if (it == iot_gateway.end() || !it->is_array()) return;
  std::set<std::string> names;
  for (const auto& aj : *it) {
    auto cfg = agent_from_json(aj);
    if (!cfg) {
      errors_.push_back(cfg.error().message);
      continue;
    }
    if (!model::is_valid_name(cfg->name) || !names.insert(cfg->name).second) {
      errors_.push_back(std::format("MQTT agent '{}': invalid or duplicate name", cfg->name));
      continue;
    }
    if (!cfg->enabled) continue;
    agents_.push_back(std::make_unique<MqttAgent>(runtime, std::move(*cfg), server_name));
  }
}

std::vector<std::string> MqttGateway::prepare() {
  std::vector<std::string> out = errors_;
  for (auto& a : agents_) {
    auto w = a->prepare();
    out.insert(out.end(), w.begin(), w.end());
  }
  return out;
}

void MqttGateway::start() {
  for (auto& a : agents_)
    if (auto r = a->start(); !r) core::make_logger("mqtt")->error("{}", r.error().message);
}

void MqttGateway::stop() {
  for (auto& a : agents_) a->stop();
}

}  // namespace logict::mqtt
