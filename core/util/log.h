#pragma once

#include <memory>
#include <string>

#include <spdlog/spdlog.h>

#include "logict/sdk/log.h"

namespace logict::core {

// Creates (or returns) a named spdlog logger sharing the default sinks.
std::shared_ptr<spdlog::logger> make_logger(const std::string& name);

// Adapts spdlog to the SDK Logger interface given to drivers.
class SpdLogger final : public sdk::Logger {
 public:
  explicit SpdLogger(std::shared_ptr<spdlog::logger> l) : logger_(std::move(l)) {}
  void log(sdk::LogLevel level, std::string_view message) override;
  bool enabled(sdk::LogLevel level) const override;

 private:
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace logict::core
