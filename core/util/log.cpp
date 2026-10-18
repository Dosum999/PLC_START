#include "util/log.h"

namespace logict::core {

namespace {
spdlog::level::level_enum to_spd(sdk::LogLevel l) {
  switch (l) {
    case sdk::LogLevel::Trace: return spdlog::level::trace;
    case sdk::LogLevel::Debug: return spdlog::level::debug;
    case sdk::LogLevel::Info: return spdlog::level::info;
    case sdk::LogLevel::Warn: return spdlog::level::warn;
    case sdk::LogLevel::Error: return spdlog::level::err;
  }
  return spdlog::level::info;
}
}  // namespace

std::shared_ptr<spdlog::logger> make_logger(const std::string& name) {
  if (auto existing = spdlog::get(name)) return existing;
  auto def = spdlog::default_logger();
  auto l = std::make_shared<spdlog::logger>(name, def->sinks().begin(), def->sinks().end());
  l->set_level(def->level());
  spdlog::register_logger(l);
  return l;
}

void SpdLogger::log(sdk::LogLevel level, std::string_view message) { logger_->log(to_spd(level), message); }

bool SpdLogger::enabled(sdk::LogLevel level) const { return logger_->should_log(to_spd(level)); }

}  // namespace logict::core
