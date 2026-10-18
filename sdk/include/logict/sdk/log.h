#pragma once

#include <format>
#include <string_view>

namespace logict::sdk {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

// Logging sink handed to drivers by the host. Drivers do not link the host's
// logging library so the plugin boundary stays small.
class Logger {
 public:
  virtual ~Logger() = default;
  virtual void log(LogLevel level, std::string_view message) = 0;
  virtual bool enabled(LogLevel level) const = 0;

  template <typename... Args>
  void trace(std::format_string<Args...> f, Args&&... a) { emit(LogLevel::Trace, f, std::forward<Args>(a)...); }
  template <typename... Args>
  void debug(std::format_string<Args...> f, Args&&... a) { emit(LogLevel::Debug, f, std::forward<Args>(a)...); }
  template <typename... Args>
  void info(std::format_string<Args...> f, Args&&... a) { emit(LogLevel::Info, f, std::forward<Args>(a)...); }
  template <typename... Args>
  void warn(std::format_string<Args...> f, Args&&... a) { emit(LogLevel::Warn, f, std::forward<Args>(a)...); }
  template <typename... Args>
  void error(std::format_string<Args...> f, Args&&... a) { emit(LogLevel::Error, f, std::forward<Args>(a)...); }

 private:
  template <typename... Args>
  void emit(LogLevel level, std::format_string<Args...> f, Args&&... a) {
    if (enabled(level)) log(level, std::format(f, std::forward<Args>(a)...));
  }
};

}  // namespace logict::sdk
