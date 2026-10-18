#pragma once

#include <atomic>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "model/project.h"
#include "runtime/channel_worker.h"
#include "runtime/plugin_loader.h"
#include "tagdb/tagdb.h"

namespace logict::core {

inline constexpr const char* kProductName = "logict";
inline constexpr const char* kProductVersion = "0.1.0";

// Owns the tag database and channel workers for one loaded project, and is the
// API the northbound interfaces (OPC UA, MQTT, REST) talk to.
class Runtime {
 public:
  explicit Runtime(DriverRegistry& drivers);
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  // Builds channels and tags. Channels whose driver is missing are skipped with
  // an error (the rest of the project still runs). Must be called before start().
  std::vector<std::string> load(model::Project project);
  void start();
  void stop();

  TagDB& tags() noexcept { return db_; }
  const TagDB& tags() const noexcept { return db_; }
  const model::Project& project() const noexcept { return project_; }

  using WriteCallback = ChannelWorker::WriteCallback;

  // Writes a client value: checks access, converts to the tag's type, applies
  // inverse scaling and queues it on the owning channel. `done` runs exactly
  // once, either inline (validation failure) or on the channel thread.
  void write_async(TagId id, const sdk::Value& value, WriteCallback done);
  std::future<sdk::StatusCode> write(TagId id, const sdk::Value& value);

  // Connected northbound clients per interface, summed into _System._ClientCount.
  void set_client_count(const std::string& interface_name, int count);

 private:
  void housekeeping();
  TagId add_server_tag(const std::string& name, sdk::DataType type, sdk::Value v);

  DriverRegistry& drivers_;
  TagDB db_;
  model::Project project_;
  std::vector<std::unique_ptr<ChannelWorker>> workers_;
  std::shared_ptr<spdlog::logger> log_;

  TagId active_tag_count_ = kInvalidTag, client_count_ = kInvalidTag, date_time_ = kInvalidTag;
  std::mutex clients_mu_;
  std::map<std::string, int> clients_;

  std::mutex mu_;
  std::condition_variable cv_;
  bool stopping_ = false;
  std::thread housekeeping_;
};

}  // namespace logict::core
