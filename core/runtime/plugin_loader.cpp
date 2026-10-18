#include "runtime/plugin_loader.h"

#include <dlfcn.h>

#include <cctype>

#include <algorithm>
#include <format>

namespace logict::core {

using sdk::make_error;
namespace status = sdk::status;

namespace {
bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(x) == std::tolower(y); });
}
}  // namespace

DriverRegistry::~DriverRegistry() {
  // Destroy drivers before unloading the code that implements them.
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
    if (it->handle) {
      it->destroy(it->driver);
      dlclose(it->handle);
    }
  }
}

sdk::Result<sdk::Driver*> DriverRegistry::load_file(const std::filesystem::path& file) {
  void* handle = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) return make_error(status::BadConfigurationError, std::format("dlopen failed: {}", dlerror()));
  auto entry = reinterpret_cast<sdk::PluginEntryFn>(dlsym(handle, sdk::kPluginEntrySymbol));
  if (!entry) {
    dlclose(handle);
    return make_error(status::BadConfigurationError, std::format("{}: missing {}", file.string(), sdk::kPluginEntrySymbol));
  }
  const sdk::PluginDescriptor* desc = entry();
  if (!desc || desc->abi_version != sdk::kPluginAbiVersion) {
    dlclose(handle);
    return make_error(status::BadConfigurationError,
                      std::format("{}: plugin ABI {} does not match host ABI {}", file.string(),
                                  desc ? desc->abi_version : 0, sdk::kPluginAbiVersion));
  }
  sdk::Driver* driver = desc->create();
  if (find(driver->info().name)) {
    std::string name = driver->info().name;
    desc->destroy(driver);
    dlclose(handle);
    return make_error(status::BadConfigurationError, std::format("{}: driver '{}' already loaded", file.string(), name));
  }
  entries_.push_back(Entry{handle, driver, desc->destroy, nullptr});
  return driver;
}

std::vector<std::string> DriverRegistry::load_directory(const std::filesystem::path& dir) {
  std::vector<std::string> errors;
  std::error_code ec;
  std::vector<std::filesystem::path> files;
  for (const auto& de : std::filesystem::directory_iterator(dir, ec))
    if (de.is_regular_file() && de.path().extension() == ".so") files.push_back(de.path());
  if (ec) errors.push_back(std::format("cannot read driver directory {}: {}", dir.string(), ec.message()));
  std::sort(files.begin(), files.end());
  for (const auto& f : files)
    if (auto r = load_file(f); !r) errors.push_back(r.error().message);
  return errors;
}

void DriverRegistry::add(std::unique_ptr<sdk::Driver> driver) {
  sdk::Driver* raw = driver.get();
  entries_.push_back(Entry{nullptr, raw, nullptr, std::move(driver)});
}

sdk::Driver* DriverRegistry::find(std::string_view name) const {
  for (const auto& e : entries_) {
    auto info = e.driver->info();
    if (iequals(info.name, name) || iequals(info.display_name, name)) return e.driver;
  }
  return nullptr;
}

std::vector<sdk::Driver*> DriverRegistry::drivers() const {
  std::vector<sdk::Driver*> out;
  for (const auto& e : entries_) out.push_back(e.driver);
  return out;
}

}  // namespace logict::core
