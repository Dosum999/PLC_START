#pragma once

#include <open62541/server.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/logger.h>

#include "runtime/runtime.h"

namespace logict::opcua {

// Kepware-compatible security mode encoding per policy:
// 0 = disabled, 1 = Sign, 2 = SignAndEncrypt, 3 = Sign + SignAndEncrypt.
struct Settings {
  bool enabled = true;
  std::uint16_t port = 49320;
  std::vector<std::string> bind_addresses;  // empty = all interfaces
  bool security_none = false;
  int basic256sha256 = 2;
  int aes128_sha256_rsaoaep = 2;
  int aes256_sha256_rsapss = 2;
  bool allow_anonymous = false;
  std::uint32_t max_sessions = 128;
  std::uint32_t max_session_timeout_s = 60;
  std::uint32_t tag_cache_timeout_s = 5;
  std::uint32_t max_data_queue_size = 2;
  std::uint32_t max_retransmit_queue_size = 10;
  std::uint32_t max_notifications_per_publish = 65536;
  double min_sampling_interval_ms = 10;
  double min_publishing_interval_ms = 50;
  std::filesystem::path pki_dir = "/var/lib/logict/pki";
  std::string hostname;  // empty = gethostname()
};

Settings settings_from_json(const nlohmann::json& j);

struct UserAccount {
  std::string name;
  std::string password_hash;  // see core/util/password.h
};

// OPC UA server exposing every tag as ns=2;s=Channel.Device[.Group].Tag, the
// same NodeIds Kepware uses.
class OpcUaServer {
 public:
  OpcUaServer(core::Runtime& runtime, Settings settings, std::vector<UserAccount> users);
  ~OpcUaServer();
  OpcUaServer(const OpcUaServer&) = delete;
  OpcUaServer& operator=(const OpcUaServer&) = delete;

  // Configures the stack, builds the address space and starts the server thread.
  sdk::Result<void> start();
  void stop();

  std::uint16_t namespace_index() const noexcept { return ns_; }

  // Checks a UserName token against the configured accounts.
  bool authenticate(std::string_view user, std::string_view password) const;

 private:
  struct CacheEntry {
    std::chrono::steady_clock::time_point last_access;
  };
  struct InlineWrite;

  sdk::Result<void> configure(UA_ServerConfig& config);
  sdk::Result<std::pair<UA_ByteString, UA_ByteString>> load_or_create_certificate(const std::string& app_uri);
  void build_address_space();
  void run();
  void expire_cache();

  // open62541 callbacks (C ABI).
  static UA_StatusCode on_read(UA_Server* server, const UA_NodeId* session_id, void* session_ctx,
                               const UA_NodeId* node_id, void* node_ctx, UA_Boolean include_source_ts,
                               const UA_NumericRange* range, UA_DataValue* value);
  static UA_StatusCode on_write(UA_Server* server, const UA_NodeId* session_id, void* session_ctx,
                                const UA_NodeId* node_id, void* node_ctx, const UA_NumericRange* range,
                                const UA_DataValue* value);
  static void on_monitored_item(UA_Server* server, const UA_NodeId* session_id, void* session_ctx,
                                const UA_NodeId* node_id, void* node_ctx, UA_UInt32 attribute_id,
                                UA_Boolean removed);
  static void on_async_cancel(UA_Server* server, const void* out);

  void touch_cache(core::TagId id);
  UA_StatusCode begin_write(core::TagId id, const sdk::Value& value, const UA_DataValue* key);
  void complete_write(const UA_DataValue* key, sdk::StatusCode st);

  core::Runtime& rt_;
  Settings settings_;
  std::vector<UserAccount> users_;
  std::shared_ptr<spdlog::logger> log_;
  UA_Logger ua_logger_{};

  UA_Server* server_ = nullptr;
  std::uint16_t ns_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<bool> building_{false};
  std::thread thread_;

  // Readers of unsubscribed tags start a temporary subscription (Kepware's
  // "tag cache"), dropped after tag_cache_timeout of inactivity.
  core::SubscriberId cache_sub_ = 0;
  std::mutex cache_mu_;
  std::unordered_map<core::TagId, CacheEntry> cache_;

  // Tags with at least one OPC UA MonitoredItem.
  core::SubscriberId monitor_sub_ = 0;
  std::mutex monitor_mu_;
  std::unordered_map<core::TagId, int> monitored_;

  // Async writes awaiting the device. Guarded by pending_mu_; never held while
  // calling into open62541 (the cancel callback runs under the server lock).
  std::mutex pending_mu_;
  std::set<const UA_DataValue*> pending_writes_;
  // Shared while completing an async op, exclusive while deleting the server.
  std::shared_mutex server_alive_mu_;
};

}  // namespace logict::opcua
