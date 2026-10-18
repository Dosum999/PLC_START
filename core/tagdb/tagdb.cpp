#include "tagdb/tagdb.h"

namespace logict::core {

TagId TagDB::add(TagInfo info) {
  auto id = static_cast<TagId>(entries_.size());
  auto e = std::make_unique<Entry>();
  by_path_.emplace(info.path, id);
  e->vqt.value = sdk::default_value(info.type);
  e->info = std::move(info);
  entries_.push_back(std::move(e));
  return id;
}

TagId TagDB::find(std::string_view path) const {
  auto it = by_path_.find(std::string(path));
  return it == by_path_.end() ? kInvalidTag : it->second;
}

sdk::Vqt TagDB::read(TagId id) const {
  const Entry& e = *entries_.at(id);
  std::lock_guard lock(e.mu);
  return e.vqt;
}

std::vector<TagId> TagDB::all() const {
  std::vector<TagId> ids(entries_.size());
  for (TagId i = 0; i < ids.size(); ++i) ids[i] = i;
  return ids;
}

bool TagDB::update(TagId id, sdk::Vqt vqt) {
  Entry& e = *entries_.at(id);
  bool changed;
  std::vector<SubscriberId> subs;
  {
    std::lock_guard lock(e.mu);
    changed = e.vqt.quality != vqt.quality || !(e.vqt.value == vqt.value);
    e.vqt = vqt;
    subs.reserve(e.subs.size());
    for (auto& [sub, rate] : e.subs) subs.push_back(sub);
  }
  if (!subs.empty()) notify(id, vqt, changed, subs);
  return changed;
}

void TagDB::set_quality(TagId id, sdk::StatusCode quality, sdk::Timestamp when) {
  sdk::Vqt v;
  {
    Entry& e = *entries_.at(id);
    std::lock_guard lock(e.mu);
    if (e.vqt.quality == quality) return;
    v = e.vqt;
  }
  v.quality = quality;
  v.server_time = when;
  update(id, std::move(v));
}

void TagDB::notify(TagId id, const sdk::Vqt& vqt, bool changed, const std::vector<SubscriberId>& subs) {
  std::vector<std::shared_ptr<UpdateCallback>> cbs;
  {
    std::shared_lock lock(subs_mu_);
    for (auto sub : subs)
      if (auto it = subscribers_.find(sub); it != subscribers_.end()) cbs.push_back(it->second);
  }
  for (auto& cb : cbs) (*cb)(id, vqt, changed);
}

SubscriberId TagDB::add_subscriber(UpdateCallback cb) {
  SubscriberId id = next_sub_++;
  std::unique_lock lock(subs_mu_);
  subscribers_.emplace(id, std::make_shared<UpdateCallback>(std::move(cb)));
  return id;
}

void TagDB::remove_subscriber(SubscriberId sub) {
  {
    std::unique_lock lock(subs_mu_);
    subscribers_.erase(sub);
  }
  for (TagId id = 0; id < entries_.size(); ++id) unsubscribe(id, sub);
}

void TagDB::subscribe(TagId id, SubscriberId sub, std::uint32_t rate_ms) {
  Entry& e = *entries_.at(id);
  bool notify_owner;
  {
    std::lock_guard lock(e.mu);
    auto [it, inserted] = e.subs.try_emplace(sub, rate_ms);
    if (inserted && e.subs.size() == 1) ++active_tags_;
    notify_owner = inserted || it->second != rate_ms;
    it->second = rate_ms;
  }
  if (notify_owner) changed_subscription(id);
}

void TagDB::unsubscribe(TagId id, SubscriberId sub) {
  Entry& e = *entries_.at(id);
  {
    std::lock_guard lock(e.mu);
    if (e.subs.erase(sub) == 0) return;
    if (e.subs.empty()) --active_tags_;
  }
  changed_subscription(id);
}

std::optional<std::uint32_t> TagDB::requested_rate(TagId id) const {
  const Entry& e = *entries_.at(id);
  std::lock_guard lock(e.mu);
  if (e.subs.empty()) return std::nullopt;
  std::uint32_t rate = ~0u;
  for (auto& [sub, r] : e.subs) rate = std::min(rate, r);
  return rate;
}

void TagDB::changed_subscription(TagId id) {
  if (auto* owner = entries_.at(id)->info.owner) owner->on_subscription_changed(id);
}

}  // namespace logict::core
