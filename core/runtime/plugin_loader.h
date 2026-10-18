#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "logict/sdk/driver.h"

namespace logict::core {

// Owns the loaded driver plugins (.so) and resolves a channel's driver by id or
// Kepware display name.
class DriverRegistry {
 public:
  DriverRegistry() = default;
  ~DriverRegistry();
  DriverRegistry(const DriverRegistry&) = delete;
  DriverRegistry& operator=(const DriverRegistry&) = delete;

  // Loads every *.so in `dir`. Returns human-readable errors for plugins that
  // failed; successfully loaded drivers stay registered.
  std::vector<std::string> load_directory(const std::filesystem::path& dir);
  sdk::Result<sdk::Driver*> load_file(const std::filesystem::path& file);

  // Registers an in-process driver (tests, built-ins). Takes ownership.
  void add(std::unique_ptr<sdk::Driver> driver);

  sdk::Driver* find(std::string_view name_or_display) const;
  std::vector<sdk::Driver*> drivers() const;

 private:
  struct Entry {
    void* handle = nullptr;  // dlopen handle, null for in-process drivers
    sdk::Driver* driver = nullptr;
    void (*destroy)(sdk::Driver*) = nullptr;
    std::unique_ptr<sdk::Driver> owned;
  };
  std::vector<Entry> entries_;
};

}  // namespace logict::core
