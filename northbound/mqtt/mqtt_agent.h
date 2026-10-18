#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/logger.h>

#include "mqtt/agent_config.h"
#include "mqtt/change_filter.h"
#include "mqtt/template.h"
#include "runtime/runtime.h"

namespace logict::mqtt {

using MQTTAsync_handle = void*;  // MQTTAsync, without pulling Paho into this header

// One Kepware-style MQTT Client agent: samples its IoT items from the tag
// database, publishes them with a template, and optionally accepts writes.
// Runs on its own thread; the Paho client runs its own threads for I/O.
class MqttAgent {
 public:
  MqttAgent(core::Runtime& runtime, AgentConfig config, std::string server_name);
  ~MqttAgent();
  MqttAgent(const MqttAgent&) = delete;
  MqttAgent& operator=(const MqttAgent&) = delete;

  // Resolves items and registers _IoT_Gateway.<agent>.* tags. Must run before
  // the runtime starts (tag registration is not thread-safe). Returns warnings.
  std::vector<std::string> prepare();
  sdk::Result<void> start();
  void stop();

  const std::string& name() const noexcept { return cfg_.name; }

  // Paho callbacks (public for the C trampolines). They only queue work for
  // the agent thread, which makes every Paho API call itself.
  void on_connected(bool ok, const std::string& reason);
  void on_connection_lost(const std::string& cause);
  void on_message(const std::string& topic, const std::string& payload, const std::string& response_topic,
                  const std::string& correlation);

 private:
  struct Item {
    ItemConfig cfg;
    core::TagId tag = core::kInvalidTag;
    std::string topic;
    ChangeFilter filter{true, false, false, 0};
    sdk::Vqt latest;
    bool has_latest = false;
    std::chrono::steady_clock::time_point next_due{};
  };
  struct Event {
    std::size_t item;
    sdk::Vqt vqt;
  };
  struct Shared;  // queue + wakeup shared with Paho and write-completion callbacks

  void run();
  void connect(MQTTAsync_handle client);
  void handle_connect_result(bool ok, const std::string& reason);
  void flush_outbox();
  void sample(Item& item);
  void publish_narrow();
  void publish_wide();
  bool send(const std::string& topic, const std::string& payload, int qos, bool retain);
  std::string render(const std::vector<const Item*>& items, const std::vector<const sdk::Vqt*>& values) const;
  void update_system_tags();
  void handle_write(const std::string& payload, std::string response_topic, std::string correlation);

  core::Runtime& rt_;
  AgentConfig cfg_;
  std::string server_name_;
  std::string uri_, client_id_;
  Template template_;
  std::shared_ptr<spdlog::logger> log_;

  std::vector<Item> items_;
  std::unordered_map<std::string, std::size_t> item_by_tag_;
  std::deque<Event> buffer_;
  core::SubscriberId sub_ = 0;

  std::shared_ptr<Shared> shared_;
  MQTTAsync_handle client_ = nullptr;
  std::atomic<bool> connected_{false};
  bool connecting_ = false;  // agent thread only
  std::chrono::steady_clock::time_point next_connect_{};
  std::chrono::seconds backoff_{1};

  std::uint32_t publishes_ = 0, dropped_ = 0;
  core::TagId t_publishes_ = core::kInvalidTag, t_dropped_ = core::kInvalidTag, t_connected_ = core::kInvalidTag,
              t_buffered_ = core::kInvalidTag;

  std::thread thread_;
};

// All MQTT agents of a project (project._iot_gateway.mqtt_clients).
class MqttGateway {
 public:
  MqttGateway(core::Runtime& runtime, const nlohmann::json& iot_gateway, std::string server_name);
  // Errors for agents that could not be configured; other agents still run.
  std::vector<std::string> prepare();
  void start();
  void stop();
  std::size_t size() const noexcept { return agents_.size(); }

 private:
  std::vector<std::unique_ptr<MqttAgent>> agents_;
  std::vector<std::string> errors_;
};

}  // namespace logict::mqtt
