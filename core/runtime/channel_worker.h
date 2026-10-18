#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/logger.h>

#include "logict/sdk/driver.h"
#include "model/project.h"
#include "tagdb/tagdb.h"
#include "util/log.h"

namespace logict::core {

// One channel = one thread, like Kepware: devices on a channel are polled
// sequentially; channels run in parallel.
class ChannelWorker final : public SubscriptionListener {
 public:
  ChannelWorker(model::Channel config, sdk::Driver& driver, TagDB& db);
  ~ChannelWorker() override;
  ChannelWorker(const ChannelWorker&) = delete;
  ChannelWorker& operator=(const ChannelWorker&) = delete;

  // Creates device runtimes and registers all tags (including _System and
  // _Statistics) in the TagDB. Call once, before start().
  void build();
  void start();
  void stop();

  const std::string& name() const noexcept { return config_.name; }

  using WriteCallback = std::function<void(sdk::StatusCode)>;

  // Queues a write of a raw (unscaled, device-typed) value. `done` runs on the
  // channel thread once the device acknowledged or the write failed.
  void write(TagId id, sdk::Value raw, WriteCallback done);

  void on_subscription_changed(TagId id) override;

 private:
  struct ScanGroup {
    std::uint32_t rate_ms = 0;
    std::vector<TagId> tags;
    sdk::Deadline next_due{};
  };

  struct SystemTags {
    TagId error = kInvalidTag, no_error = kInvalidTag, scan_mode = kInvalidTag, scan_rate = kInvalidTag,
          device_id = kInvalidTag, enabled = kInvalidTag, simulated = kInvalidTag, demoted = kInvalidTag;
  };

  struct DeviceSlot {
    model::Device config;
    std::string path;  // Channel.Device
    std::unique_ptr<sdk::DeviceRuntime> runtime;
    std::vector<TagId> tags;  // polled device tags
    std::vector<ScanGroup> groups;
    bool groups_dirty = true;
    std::set<TagId> needs_initial;  // newly subscribed tags to read immediately
    bool in_error = false;
    std::uint32_t consecutive_failures = 0;
    sdk::Deadline demoted_until{};
    bool demoted = false;
    SystemTags sys;
  };

  struct WriteRequest {
    TagId tag;
    sdk::Value value;
    std::vector<WriteCallback> waiters;
  };

  struct Statistics {
    TagId successful_reads = kInvalidTag, failed_reads = kInvalidTag, successful_writes = kInvalidTag,
          failed_writes = kInvalidTag, tx_bytes = kInvalidTag, rx_bytes = kInvalidTag;
    std::uint32_t ok_reads = 0, bad_reads = 0, ok_writes = 0, bad_writes = 0;
    std::uint64_t tx = 0, rx = 0;
  };

  class Frames;

  void run();
  void register_tags(DeviceSlot& slot, std::uint32_t slot_index, const std::vector<model::Tag>& tags,
                     const std::vector<model::TagGroup>& groups, const std::string& prefix);
  void register_system_tags(DeviceSlot& slot);
  void register_statistics();
  TagId add_internal_tag(const std::string& path, sdk::TagType type, TagKind kind, sdk::Value initial);

  void rebuild_groups(DeviceSlot& slot, sdk::Deadline now);
  std::optional<std::uint32_t> effective_rate(const DeviceSlot& slot, TagId id) const;
  void poll(DeviceSlot& slot, const std::vector<TagId>& tags);
  bool ensure_connected(DeviceSlot& slot);
  void handle_comm_result(DeviceSlot& slot, sdk::StatusCode st);
  void set_device_error(DeviceSlot& slot, bool error);
  void mark_tags_bad(DeviceSlot& slot, sdk::StatusCode quality);
  void process_write(WriteRequest& req);
  void publish_statistics();
  void set_internal(TagId id, sdk::Value v);

  model::Channel config_;
  sdk::Driver& driver_;
  TagDB& db_;
  std::shared_ptr<spdlog::logger> log_;
  std::unique_ptr<SpdLogger> driver_log_;
  std::unique_ptr<Frames> frames_;
  std::unique_ptr<sdk::HostContext> host_;

  std::vector<DeviceSlot> devices_;
  Statistics stats_;
  bool stats_dirty_ = false;

  std::mutex mu_;  // guards everything below
  std::condition_variable cv_;
  std::deque<WriteRequest> writes_;
  std::set<std::uint32_t> dirty_slots_;
  std::set<TagId> newly_subscribed_;
  bool stopping_ = false;

  std::thread thread_;
};

}  // namespace logict::core
