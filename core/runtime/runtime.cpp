#include "runtime/runtime.h"

#include <format>

#include "runtime/scaling.h"

namespace logict::core {

namespace status = sdk::status;


Runtime::Runtime(DriverRegistry& drivers) : drivers_(drivers), log_(make_logger("runtime")) {}

Runtime::~Runtime() { stop(); }

TagId Runtime::add_server_tag(const std::string& name, sdk::DataType type, sdk::Value v) {
  TagInfo info;
  info.path = "_System." + name;
  info.kind = TagKind::System;
  info.raw_type = info.type = sdk::TagType{type};
  info.config.name = name;
  info.config.type = info.type;
  info.config.access = sdk::Access::ReadOnly;
  TagId id = db_.add(std::move(info));
  auto now = sdk::Clock::now();
  db_.update(id, sdk::Vqt{std::move(v), status::Good, now, now});
  return id;
}

std::vector<std::string> Runtime::load(model::Project project) {
  std::vector<std::string> errors;
  project_ = std::move(project);

  using sdk::DataType;
  add_server_tag("_ProductName", DataType::String, std::string(kProductName));
  add_server_tag("_ProductVersion", DataType::String, std::string(kProductVersion));
  add_server_tag("_ProjectTitle", DataType::String, project_.title);
  active_tag_count_ = add_server_tag("_ActiveTagCount", DataType::DWord, 0u);
  client_count_ = add_server_tag("_ClientCount", DataType::DWord, 0u);
  date_time_ = add_server_tag("_DateTime", DataType::Date, sdk::DateTime{sdk::Clock::now()});

  for (const auto& ch : project_.channels) {
    sdk::Driver* driver = drivers_.find(ch.driver);
    if (!driver) {
      errors.push_back(std::format("channel '{}': driver '{}' is not installed", ch.name, ch.driver));
      continue;
    }
    auto worker = std::make_unique<ChannelWorker>(ch, *driver, db_);
    worker->build();
    workers_.push_back(std::move(worker));
  }
  log_->info("project loaded: {} channel(s), {} tag(s)", workers_.size(), db_.size());
  return errors;
}

void Runtime::start() {
  for (auto& w : workers_) w->start();
  {
    std::lock_guard lock(mu_);
    stopping_ = false;
  }
  housekeeping_ = std::thread([this] { housekeeping(); });
}

void Runtime::stop() {
  {
    std::lock_guard lock(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (housekeeping_.joinable()) housekeeping_.join();
  for (auto& w : workers_) w->stop();
}

void Runtime::housekeeping() {
  std::unique_lock lock(mu_);
  while (!stopping_) {
    lock.unlock();
    auto now = sdk::Clock::now();
    db_.update(active_tag_count_, sdk::Vqt{static_cast<std::uint32_t>(db_.active_tag_count()), status::Good, now, now});
    int clients = 0;
    {
      std::lock_guard clock(clients_mu_);
      for (auto& [name, n] : clients_) clients += n;
    }
    db_.update(client_count_, sdk::Vqt{static_cast<std::uint32_t>(std::max(0, clients)), status::Good, now, now});
    db_.update(date_time_, sdk::Vqt{sdk::DateTime{now}, status::Good, now, now});
    lock.lock();
    cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_; });
  }
}

void Runtime::set_client_count(const std::string& interface_name, int count) {
  std::lock_guard lock(clients_mu_);
  clients_[interface_name] = count;
}

void Runtime::write_async(TagId id, const sdk::Value& value, WriteCallback done) {
  if (id >= db_.size()) return done(status::BadNodeIdUnknown);
  const TagInfo& info = db_.info(id);
  auto* worker = dynamic_cast<ChannelWorker*>(info.owner);
  if (info.kind != TagKind::Device || !worker || info.config.access == sdk::Access::ReadOnly)
    return done(status::BadNotWritable);

  if (info.type.is_array()) {
    const auto* arr = value.get_if<sdk::Array>();
    if (!arr || arr->size() != info.type.element_count()) return done(status::BadTypeMismatch);
  } else if (value.is_array()) {
    return done(status::BadTypeMismatch);
  }

  auto typed = sdk::convert(value, info.type.scalar);
  if (!typed) return done(typed.error().code);
  auto raw = unscale(*typed, info.config.scaling, info.raw_type.scalar);
  if (!raw) return done(raw.error().code);
  worker->write(id, std::move(*raw), std::move(done));
}

std::future<sdk::StatusCode> Runtime::write(TagId id, const sdk::Value& value) {
  auto promise = std::make_shared<std::promise<sdk::StatusCode>>();
  auto fut = promise->get_future();
  write_async(id, value, [promise](sdk::StatusCode st) { promise->set_value(st); });
  return fut;
}

}  // namespace logict::core
