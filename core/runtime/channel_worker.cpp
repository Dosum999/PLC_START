#include "runtime/channel_worker.h"

#include <cmath>
#include <format>

#include "runtime/scaling.h"

namespace logict::core {

namespace status = sdk::status;
using sdk::StatusCode;

namespace {

bool is_comm_error(StatusCode st) {
  return st == status::BadTimeout || st == status::BadNotConnected || st == status::BadCommunicationError ||
         st == status::BadNoCommunication || st == status::BadDeviceFailure;
}

const char* scan_mode_name(model::ScanMode m) {
  switch (m) {
    case model::ScanMode::RespectClientRate: return "Respect client specified scan rate";
    case model::ScanMode::NoFasterThanScanRate: return "Request data no faster than scan rate";
    case model::ScanMode::AllDataAtScanRate: return "Request all data at scan rate";
    case model::ScanMode::DemandPollOnly: return "Do not scan, demand poll only";
    case model::ScanMode::RespectTagRate: return "Respect tag specified scan rate";
  }
  return "";
}

// Replaces NaN/Inf with 0 (Kepware "Non-Normalized Float Handling: Replace with Zero").
void replace_non_normalized(sdk::Value& v) {
  if (auto* f = std::get_if<float>(&v.data); f && !std::isnormal(*f) && *f != 0.0f) *f = 0.0f;
  else if (auto* d = std::get_if<double>(&v.data); d && !std::isnormal(*d) && *d != 0.0) *d = 0.0;
  else if (auto* a = std::get_if<sdk::Array>(&v.data))
    for (auto& e : *a) replace_non_normalized(e);
}

}  // namespace

// Counts bytes on the wire for the _Statistics tags. Runs on the channel thread.
class ChannelWorker::Frames final : public sdk::FrameObserver {
 public:
  explicit Frames(Statistics& s, bool& dirty) : stats_(s), dirty_(dirty) {}
  void on_tx(std::span<const std::uint8_t> f) override { stats_.tx += f.size(); dirty_ = true; }
  void on_rx(std::span<const std::uint8_t> f) override { stats_.rx += f.size(); dirty_ = true; }

 private:
  Statistics& stats_;
  bool& dirty_;
};

ChannelWorker::ChannelWorker(model::Channel config, sdk::Driver& driver, TagDB& db)
    : config_(std::move(config)), driver_(driver), db_(db), log_(make_logger("ch:" + config_.name)) {
  driver_log_ = std::make_unique<SpdLogger>(log_);
  frames_ = std::make_unique<Frames>(stats_, stats_dirty_);
  host_ = std::make_unique<sdk::HostContext>(sdk::HostContext{*driver_log_, frames_.get()});
}

ChannelWorker::~ChannelWorker() { stop(); }

// ---- setup ----------------------------------------------------------------

void ChannelWorker::build() {
  sdk::ChannelConfig cc{config_.name, config_.driver, model::channel_to_json(config_)};
  cc.properties.erase("devices");

  devices_.reserve(config_.devices.size());
  for (const auto& dev : config_.devices) {
    DeviceSlot& slot = devices_.emplace_back();
    slot.config = dev;
    slot.path = config_.name + "." + dev.name;

    sdk::DeviceConfig dc;
    dc.name = dev.name;
    dc.model = dev.model;
    dc.id = dev.id;
    dc.connect_timeout = std::chrono::seconds(dev.connect_timeout_s);
    dc.request_timeout = std::chrono::milliseconds(dev.request_timeout_ms);
    dc.attempts = static_cast<int>(dev.attempts);
    dc.inter_request_delay = std::chrono::milliseconds(dev.inter_request_delay_ms);
    dc.properties = dev.props;

    try {
      if (auto rt = driver_.create_device(cc, dc, *host_)) {
        slot.runtime = std::move(*rt);
      } else {
        log_->error("device '{}': {}", dev.name, rt.error().message);
      }
    } catch (const std::exception& e) {
      log_->error("device '{}': driver threw: {}", dev.name, e.what());
    }

    register_system_tags(slot);
    register_tags(slot, static_cast<std::uint32_t>(devices_.size() - 1), dev.tags, dev.groups, slot.path);
    if (!slot.runtime) mark_tags_bad(slot, status::BadConfigurationError);
  }
  register_statistics();
}

TagId ChannelWorker::add_internal_tag(const std::string& path, sdk::TagType type, TagKind kind, sdk::Value initial) {
  TagInfo info;
  info.path = path;
  info.channel = config_.name;
  info.kind = kind;
  info.raw_type = info.type = type;
  info.config.name = path.substr(path.rfind('.') + 1);
  info.config.type = type;
  info.config.access = sdk::Access::ReadOnly;
  TagId id = db_.add(std::move(info));
  db_.update(id, sdk::Vqt{std::move(initial), status::Good, sdk::Clock::now(), sdk::Clock::now()});
  return id;
}

void ChannelWorker::register_system_tags(DeviceSlot& slot) {
  const std::string p = slot.path + "._System.";
  const auto& d = slot.config;
  using sdk::DataType;
  slot.sys.error = add_internal_tag(p + "_Error", {DataType::Boolean}, TagKind::System, false);
  slot.sys.no_error = add_internal_tag(p + "_NoError", {DataType::Boolean}, TagKind::System, true);
  slot.sys.enabled = add_internal_tag(p + "_Enabled", {DataType::Boolean}, TagKind::System, d.enabled);
  slot.sys.simulated = add_internal_tag(p + "_Simulated", {DataType::Boolean}, TagKind::System, d.simulated);
  slot.sys.scan_mode = add_internal_tag(p + "_ScanMode", {DataType::String}, TagKind::System,
                                        std::string(scan_mode_name(d.scan_mode)));
  slot.sys.scan_rate = add_internal_tag(p + "_ScanRateMs", {DataType::DWord}, TagKind::System, d.scan_rate_ms);
  slot.sys.device_id = add_internal_tag(p + "_DeviceId", {DataType::String}, TagKind::System, d.id);
  slot.sys.demoted = add_internal_tag(p + "_AutoDemoted", {DataType::Boolean}, TagKind::System, false);
}

void ChannelWorker::register_statistics() {
  const std::string p = config_.name + "._Statistics.";
  using sdk::DataType;
  stats_.successful_reads = add_internal_tag(p + "_SuccessfulReads", {DataType::DWord}, TagKind::Statistics, 0u);
  stats_.failed_reads = add_internal_tag(p + "_FailedReads", {DataType::DWord}, TagKind::Statistics, 0u);
  stats_.successful_writes = add_internal_tag(p + "_SuccessfulWrites", {DataType::DWord}, TagKind::Statistics, 0u);
  stats_.failed_writes = add_internal_tag(p + "_FailedWrites", {DataType::DWord}, TagKind::Statistics, 0u);
  stats_.tx_bytes = add_internal_tag(p + "_TxBytes", {DataType::QWord}, TagKind::Statistics, std::uint64_t{0});
  stats_.rx_bytes = add_internal_tag(p + "_RxBytes", {DataType::QWord}, TagKind::Statistics, std::uint64_t{0});
}

void ChannelWorker::register_tags(DeviceSlot& slot, std::uint32_t slot_index, const std::vector<model::Tag>& tags,
                                  const std::vector<model::TagGroup>& groups, const std::string& prefix) {
  for (const auto& t : tags) {
    TagInfo info;
    info.path = prefix + "." + t.name;
    info.channel = config_.name;
    info.device = slot.config.name;
    info.kind = TagKind::Device;
    info.config = t;
    info.raw_type = t.type;

    bool ok = false;
    if (slot.runtime) {
      try {
        auto addr = slot.runtime->parse_address(t.address, t.type);
        if (addr) {
          info.address = *addr;
          info.raw_type = (*addr)->type;
          ok = true;
        } else {
          log_->warn("tag '{}': invalid address '{}': {}", info.path, t.address, addr.error().message);
        }
      } catch (const std::exception& e) {
        log_->warn("tag '{}': driver threw while parsing address: {}", info.path, e.what());
      }
    }
    info.type = info.raw_type;
    if (t.scaling.type != model::Scaling::Type::None) info.type.scalar = t.scaling.scaled_type;
    if (ok) {
      info.owner = this;
      info.owner_index = slot_index;
    }
    TagId id = db_.add(std::move(info));
    if (ok) slot.tags.push_back(id);
    else db_.set_quality(id, status::BadConfigurationError, sdk::Clock::now());
  }
  for (const auto& g : groups) register_tags(slot, slot_index, g.tags, g.groups, prefix + "." + g.name);
}

// ---- lifecycle --------------------------------------------------------------

void ChannelWorker::start() {
  if (thread_.joinable()) return;
  {
    std::lock_guard lock(mu_);
    stopping_ = false;
  }
  thread_ = std::thread([this] { run(); });
}

void ChannelWorker::stop() {
  {
    std::lock_guard lock(mu_);
    if (!thread_.joinable()) return;
    stopping_ = true;
  }
  cv_.notify_all();
  thread_.join();
  std::deque<WriteRequest> pending;
  {
    std::lock_guard lock(mu_);
    pending.swap(writes_);
  }
  for (auto& w : pending)
    for (auto& done : w.waiters) done(status::BadOutOfService);
  for (auto& slot : devices_)
    if (slot.runtime && slot.runtime->connected()) slot.runtime->disconnect();
}

void ChannelWorker::write(TagId id, sdk::Value raw, WriteCallback done) {
  const bool is_bool = db_.info(id).raw_type.scalar == sdk::DataType::Boolean;
  const bool coalesce = config_.write_method == model::WriteOptimization::LatestValueAll ||
                        (config_.write_method == model::WriteOptimization::LatestValueNonBoolean && !is_bool);
  {
    std::lock_guard lock(mu_);
    if (!stopping_ && thread_.joinable()) {
      WriteRequest* existing = nullptr;
      if (coalesce)
        for (auto& w : writes_)
          if (w.tag == id) existing = &w;
      if (existing) {
        existing->value = std::move(raw);
        existing->waiters.push_back(std::move(done));
      } else {
        WriteRequest req{id, std::move(raw), {}};
        req.waiters.push_back(std::move(done));
        writes_.push_back(std::move(req));
      }
      done = nullptr;
    }
  }
  if (done) {
    done(status::BadOutOfService);
    return;
  }
  cv_.notify_all();
}

void ChannelWorker::on_subscription_changed(TagId id) {
  {
    std::lock_guard lock(mu_);
    dirty_slots_.insert(db_.info(id).owner_index);
    newly_subscribed_.insert(id);
  }
  cv_.notify_all();
}

// ---- thread -----------------------------------------------------------------

void ChannelWorker::run() {
  log_->info("channel started ({} device(s), driver {})", devices_.size(), driver_.info().name);
  auto next_wake = sdk::SteadyClock::now();

  while (true) {
    std::deque<WriteRequest> writes;
    std::set<std::uint32_t> dirty;
    std::set<TagId> fresh;
    {
      std::unique_lock lock(mu_);
      cv_.wait_until(lock, next_wake,
                     [this] { return stopping_ || !writes_.empty() || !dirty_slots_.empty(); });
      if (stopping_) break;
      // Kepware duty cycle: at most N writes per read pass so polling never starves.
      for (std::uint32_t i = 0; i < config_.duty_cycle && !writes_.empty(); ++i) {
        writes.push_back(std::move(writes_.front()));
        writes_.pop_front();
      }
      dirty.swap(dirty_slots_);
      fresh.swap(newly_subscribed_);
    }

    for (auto idx : dirty) devices_.at(idx).groups_dirty = true;
    for (TagId id : fresh)
      if (db_.requested_rate(id)) devices_.at(db_.info(id).owner_index).needs_initial.insert(id);

    for (auto& w : writes) process_write(w);

    auto now = sdk::SteadyClock::now();
    next_wake = now + std::chrono::seconds(1);
    for (auto& slot : devices_) {
      if (slot.groups_dirty) rebuild_groups(slot, now);
      if (!slot.config.enabled) continue;
      if (slot.demoted) {
        if (now < slot.demoted_until) {
          next_wake = std::min(next_wake, slot.demoted_until);
          continue;
        }
        slot.demoted = false;
        set_internal(slot.sys.demoted, false);
        log_->info("device '{}' promoted back to scan", slot.config.name);
      }
      if (!slot.needs_initial.empty()) {
        std::vector<TagId> initial(slot.needs_initial.begin(), slot.needs_initial.end());
        slot.needs_initial.clear();
        poll(slot, initial);
      }
      for (auto& g : slot.groups) {
        now = sdk::SteadyClock::now();
        if (now >= g.next_due && !slot.demoted) {
          poll(slot, g.tags);
          g.next_due += std::chrono::milliseconds(g.rate_ms);
          if (g.next_due <= now) g.next_due = now + std::chrono::milliseconds(g.rate_ms);
        }
        next_wake = std::min(next_wake, g.next_due);
      }
      if (slot.demoted) next_wake = std::min(next_wake, slot.demoted_until);
    }
    publish_statistics();
    {
      std::lock_guard lock(mu_);
      if (!writes_.empty()) next_wake = sdk::SteadyClock::now();
    }
  }
  log_->info("channel stopped");
}

std::optional<std::uint32_t> ChannelWorker::effective_rate(const DeviceSlot& slot, TagId id) const {
  auto requested = db_.requested_rate(id);
  if (!requested) return std::nullopt;
  const auto& d = slot.config;
  switch (d.scan_mode) {
    case model::ScanMode::RespectClientRate: return std::max<std::uint32_t>(10, *requested);
    case model::ScanMode::NoFasterThanScanRate: return std::max(*requested, d.scan_rate_ms);
    case model::ScanMode::AllDataAtScanRate: return d.scan_rate_ms;
    case model::ScanMode::DemandPollOnly: return std::nullopt;
    case model::ScanMode::RespectTagRate: return db_.info(id).config.scan_rate_ms;
  }
  return std::nullopt;
}

void ChannelWorker::rebuild_groups(DeviceSlot& slot, sdk::Deadline now) {
  std::map<std::uint32_t, std::vector<TagId>> by_rate;
  for (TagId id : slot.tags)
    if (auto rate = effective_rate(slot, id)) by_rate[*rate].push_back(id);

  std::vector<ScanGroup> groups;
  for (auto& [rate, tags] : by_rate) {
    ScanGroup g{rate, std::move(tags), now + std::chrono::milliseconds(rate)};
    for (const auto& old : slot.groups)
      if (old.rate_ms == rate) g.next_due = old.next_due;
    groups.push_back(std::move(g));
  }
  slot.groups = std::move(groups);
  slot.groups_dirty = false;
}

// ---- I/O --------------------------------------------------------------------

bool ChannelWorker::ensure_connected(DeviceSlot& slot) {
  if (!slot.runtime) return false;
  if (slot.runtime->connected()) return true;
  StatusCode st;
  try {
    st = slot.runtime->connect(sdk::SteadyClock::now() + std::chrono::seconds(slot.config.connect_timeout_s));
  } catch (const std::exception& e) {
    log_->error("device '{}': connect threw: {}", slot.config.name, e.what());
    st = status::BadInternalError;
  }
  if (sdk::is_good(st)) {
    log_->info("device '{}' connected", slot.config.name);
    return true;
  }
  if (!slot.in_error) log_->warn("device '{}' connect failed: {}", slot.config.name, sdk::status_name(st));
  handle_comm_result(slot, st);
  mark_tags_bad(slot, status::BadNotConnected);
  return false;
}

void ChannelWorker::poll(DeviceSlot& slot, const std::vector<TagId>& tags) {
  if (tags.empty()) return;
  const auto sys_now = sdk::Clock::now();

  if (slot.config.simulated) {
    // Kepware "Simulated": no communication, tags hold their last written value.
    for (TagId id : tags) {
      auto v = db_.read(id);
      if (v.quality == status::BadWaitingForInitialData) v.quality = status::Good;
      v.source_time = v.server_time = sys_now;
      db_.update(id, std::move(v));
    }
    return;
  }
  if (!ensure_connected(slot)) return;

  std::vector<sdk::ReadItem> items(tags.size());
  std::vector<sdk::ReadItem*> ptrs(tags.size());
  for (std::size_t i = 0; i < tags.size(); ++i) {
    items[i].address = db_.info(tags[i]).address.get();
    ptrs[i] = &items[i];
  }

  StatusCode st;
  try {
    st = slot.runtime->read(ptrs);
  } catch (const std::exception& e) {
    log_->error("device '{}': read threw: {}", slot.config.name, e.what());
    st = status::BadInternalError;
  }
  handle_comm_result(slot, st);
  stats_dirty_ = true;
  if (!sdk::is_good(st)) {
    ++stats_.bad_reads;
    for (TagId id : tags) db_.set_quality(id, st, sys_now);
    return;
  }
  ++stats_.ok_reads;

  for (std::size_t i = 0; i < tags.size(); ++i) {
    const TagInfo& info = db_.info(tags[i]);
    auto& item = items[i];
    sdk::Vqt v;
    v.server_time = sys_now;
    v.source_time = item.source_time == sdk::Timestamp{} ? sys_now : item.source_time;
    v.quality = item.quality;
    if (sdk::is_good(item.quality) || sdk::is_uncertain(item.quality)) {
      if (config_.replace_non_normalized_floats) replace_non_normalized(item.value);
      auto scaled = scale(item.value, info.config.scaling);
      if (scaled) {
        v.value = std::move(*scaled);
      } else {
        v.quality = status::BadTypeMismatch;
        v.value = db_.read(tags[i]).value;
      }
    } else {
      v.value = db_.read(tags[i]).value;
    }
    db_.update(tags[i], std::move(v));
  }

  if (slot.config.inter_request_delay_ms > 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(slot.config.inter_request_delay_ms));
}

void ChannelWorker::handle_comm_result(DeviceSlot& slot, StatusCode st) {
  if (sdk::is_good(st)) {
    slot.consecutive_failures = 0;
    set_device_error(slot, false);
    return;
  }
  if (!is_comm_error(st) && st != status::BadInternalError) return;  // per-request error, device is fine
  set_device_error(slot, true);
  if (slot.runtime && slot.runtime->connected()) slot.runtime->disconnect();
  ++slot.consecutive_failures;
  if (slot.config.demotion_enabled && slot.consecutive_failures >= slot.config.demote_after_timeouts) {
    slot.demoted = true;
    slot.demoted_until = sdk::SteadyClock::now() + std::chrono::milliseconds(slot.config.demotion_period_ms);
    slot.consecutive_failures = 0;
    set_internal(slot.sys.demoted, true);
    log_->warn("device '{}' auto-demoted for {} ms", slot.config.name, slot.config.demotion_period_ms);
  }
}

void ChannelWorker::set_device_error(DeviceSlot& slot, bool error) {
  if (slot.in_error == error) return;
  slot.in_error = error;
  set_internal(slot.sys.error, error);
  set_internal(slot.sys.no_error, !error);
  if (!error) log_->info("device '{}' communication restored", slot.config.name);
}

void ChannelWorker::mark_tags_bad(DeviceSlot& slot, StatusCode quality) {
  auto now = sdk::Clock::now();
  for (TagId id : slot.tags) db_.set_quality(id, quality, now);
}

void ChannelWorker::process_write(WriteRequest& req) {
  const TagInfo& info = db_.info(req.tag);
  DeviceSlot& slot = devices_.at(info.owner_index);
  StatusCode st;

  if (!slot.config.enabled || (slot.demoted && slot.config.demotion_discard_writes)) {
    st = status::BadOutOfService;
  } else if (slot.config.simulated) {
    auto scaled = scale(req.value, info.config.scaling);
    st = scaled ? status::Good : scaled.error().code;
    if (scaled) {
      auto now = sdk::Clock::now();
      db_.update(req.tag, sdk::Vqt{std::move(*scaled), status::Good, now, now});
    }
  } else if (!ensure_connected(slot)) {
    st = status::BadNotConnected;
  } else {
    try {
      st = slot.runtime->write(*info.address, req.value);
    } catch (const std::exception& e) {
      log_->error("device '{}': write threw: {}", slot.config.name, e.what());
      st = status::BadInternalError;
    }
    handle_comm_result(slot, st);
    // Read back on the next pass so subscribers see the new value promptly.
    if (sdk::is_good(st) && db_.requested_rate(req.tag)) slot.needs_initial.insert(req.tag);
  }

  if (sdk::is_good(st)) ++stats_.ok_writes;
  else {
    ++stats_.bad_writes;
    log_->warn("write to '{}' failed: {}", info.path, sdk::status_name(st));
  }
  stats_dirty_ = true;
  for (auto& done : req.waiters) done(st);
}

void ChannelWorker::set_internal(TagId id, sdk::Value v) {
  if (id == kInvalidTag) return;
  auto now = sdk::Clock::now();
  db_.update(id, sdk::Vqt{std::move(v), status::Good, now, now});
}

void ChannelWorker::publish_statistics() {
  if (!stats_dirty_) return;
  stats_dirty_ = false;
  set_internal(stats_.successful_reads, stats_.ok_reads);
  set_internal(stats_.failed_reads, stats_.bad_reads);
  set_internal(stats_.successful_writes, stats_.ok_writes);
  set_internal(stats_.failed_writes, stats_.bad_writes);
  set_internal(stats_.tx_bytes, stats_.tx);
  set_internal(stats_.rx_bytes, stats_.rx);
}

}  // namespace logict::core
