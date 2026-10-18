#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "logict/sdk/driver.h"
#include "logict/sdk/value.h"
#include "model/project.h"

namespace logict::core {

using TagId = std::uint32_t;
using SubscriberId = std::uint64_t;
inline constexpr TagId kInvalidTag = ~TagId{0};

enum class TagKind : std::uint8_t { Device, System, Statistics };

// Notified when a tag's subscription set changes (the owning channel uses it to
// rebuild scan groups). Called with no TagDB locks held.
class SubscriptionListener {
 public:
  virtual ~SubscriptionListener() = default;
  virtual void on_subscription_changed(TagId id) = 0;
};

// Static description of a tag; immutable once registered.
struct TagInfo {
  std::string path;  // Channel.Device[.Group...].Tag
  std::string channel, device;
  TagKind kind = TagKind::Device;
  model::Tag config;                                   // as configured
  sdk::TagType raw_type;                               // device type (driver-resolved)
  sdk::TagType type;                                   // exposed type (scaled type if scaling)
  std::shared_ptr<const sdk::Address> address;         // null for system tags
  SubscriptionListener* owner = nullptr;               // channel worker, null for system tags
  std::uint32_t owner_index = 0;                       // owner's device slot index
};

// Called on every update of a subscribed tag; `changed` is false when value and
// quality are unchanged (MQTT "every scan" still wants those). Must be fast.
using UpdateCallback = std::function<void(TagId, const sdk::Vqt&, bool changed)>;

// Value cache and subscription registry for all tags.
class TagDB {
 public:
  TagDB() = default;
  TagDB(const TagDB&) = delete;
  TagDB& operator=(const TagDB&) = delete;

  // Registration happens before runtime start (single-threaded).
  TagId add(TagInfo info);
  std::size_t size() const noexcept { return entries_.size(); }

  TagId find(std::string_view path) const;
  const TagInfo& info(TagId id) const { return entries_.at(id)->info; }
  sdk::Vqt read(TagId id) const;
  std::vector<TagId> all() const;

  // Stores a new VQT and notifies subscribers. Returns true if value or quality changed.
  bool update(TagId id, sdk::Vqt vqt);
  // Sets quality only (keeps last value), e.g. on communication loss.
  void set_quality(TagId id, sdk::StatusCode quality, sdk::Timestamp when);

  SubscriberId add_subscriber(UpdateCallback cb);
  void remove_subscriber(SubscriberId sub);
  // Adds or updates the subscriber's requested rate for a tag.
  void subscribe(TagId id, SubscriberId sub, std::uint32_t rate_ms);
  void unsubscribe(TagId id, SubscriberId sub);

  // Fastest requested rate across subscribers, nullopt if not subscribed.
  std::optional<std::uint32_t> requested_rate(TagId id) const;
  std::size_t active_tag_count() const noexcept { return active_tags_.load(); }

 private:
  struct Entry {
    TagInfo info;
    mutable std::mutex mu;
    sdk::Vqt vqt;
    std::map<SubscriberId, std::uint32_t> subs;
  };

  void notify(TagId id, const sdk::Vqt& vqt, bool changed, const std::vector<SubscriberId>& subs);
  void changed_subscription(TagId id);

  std::vector<std::unique_ptr<Entry>> entries_;
  std::unordered_map<std::string, TagId> by_path_;

  mutable std::shared_mutex subs_mu_;
  std::unordered_map<SubscriberId, std::shared_ptr<UpdateCallback>> subscribers_;
  std::atomic<SubscriberId> next_sub_{1};
  std::atomic<std::size_t> active_tags_{0};
};

}  // namespace logict::core
