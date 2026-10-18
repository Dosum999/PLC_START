#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "logict/sdk/log.h"
#include "logict/sdk/status.h"
#include "logict/sdk/types.h"
#include "logict/sdk/value.h"

namespace logict::sdk {

inline constexpr std::uint32_t kPluginAbiVersion = 1;

enum class Medium : std::uint8_t { Ethernet, Serial, Virtual };

struct DriverInfo {
  std::string name;          // stable id used in config, e.g. "modbus_tcp"
  std::string display_name;  // Kepware-style name, e.g. "Modbus TCP/IP Ethernet"
  std::string version;
  Medium medium = Medium::Ethernet;
  std::vector<std::string> models;
};

// Parsed, validated tag address. Drivers subclass this with their own fields
// (area, offset, bit, ...). Created once when a tag is added, then reused.
struct Address {
  virtual ~Address() = default;
  std::string text;  // address as configured
  TagType type;      // resolved type (Default replaced by the driver's choice)
};

// One tag in a read transaction. The driver fills value/quality/source_time.
// Values are raw device values; scaling is applied by the host.
struct ReadItem {
  const Address* address = nullptr;
  Value value;
  StatusCode quality = status::BadWaitingForInitialData;
  Timestamp source_time{};
};

struct ChannelConfig {
  std::string name;
  std::string driver;
  nlohmann::json properties = nlohmann::json::object();  // all channel properties
};

struct DeviceConfig {
  std::string name;
  std::string model;
  std::string id;  // servermain.DEVICE_ID_STRING (IP, station number, ...)
  std::chrono::milliseconds connect_timeout{3000};
  std::chrono::milliseconds request_timeout{1000};
  int attempts = 3;  // attempts before timeout
  std::chrono::milliseconds inter_request_delay{0};
  nlohmann::json properties = nlohmann::json::object();  // all device properties
};

// Optional observer for raw protocol frames (communication diagnostics).
class FrameObserver {
 public:
  virtual ~FrameObserver() = default;
  virtual void on_tx(std::span<const std::uint8_t> frame) = 0;
  virtual void on_rx(std::span<const std::uint8_t> frame) = 0;
};

// Services the host offers to a device runtime.
struct HostContext {
  Logger& log;
  FrameObserver* frames = nullptr;  // may be null
};

// Runtime for one device. All calls happen on the owning channel's thread, so
// implementations need no locking.
//
// read()/write() return the transaction-level status: Good if the device
// answered (per-item problems go into ReadItem::quality), or a communication
// error (BadTimeout, BadNotConnected, BadCommunicationError, ...) if it did not.
// The host uses that status for auto-demotion and reconnects.
class DeviceRuntime {
 public:
  virtual ~DeviceRuntime() = default;

  virtual Result<std::shared_ptr<const Address>> parse_address(std::string_view address,
                                                               TagType requested) = 0;
  virtual StatusCode connect(Deadline deadline) = 0;
  virtual void disconnect() = 0;
  virtual bool connected() const = 0;
  virtual StatusCode read(std::span<ReadItem*> items) = 0;
  virtual StatusCode write(const Address& address, const Value& value) = 0;
};

class Driver {
 public:
  virtual ~Driver() = default;
  virtual DriverInfo info() const = 0;
  // Property definitions for channel/device, served at /config/v1/doc/drivers/{name}.
  virtual nlohmann::json schema() const = 0;
  virtual Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig& channel,
                                                               const DeviceConfig& device,
                                                               HostContext& host) = 0;
};

// ---- plugin entry point ---------------------------------------------------

struct PluginDescriptor {
  std::uint32_t abi_version;
  Driver* (*create)();
  void (*destroy)(Driver*);
};

using PluginEntryFn = const PluginDescriptor* (*)();
inline constexpr const char* kPluginEntrySymbol = "logict_plugin_entry";

// Reads a property with a fallback, tolerating type mismatches.
template <typename T>
T property(const nlohmann::json& props, std::string_view key, T fallback) {
  auto it = props.find(key);
  if (it == props.end() || it->is_null()) return fallback;
  try {
    return it->template get<T>();
  } catch (const nlohmann::json::exception&) {
    return fallback;
  }
}

}  // namespace logict::sdk

// Exports the plugin entry point for a Driver subclass with a default constructor.
#define LOGICT_DECLARE_DRIVER(DriverClass)                                                    \
  extern "C" __attribute__((visibility("default"))) const ::logict::sdk::PluginDescriptor*    \
  logict_plugin_entry() {                                                                     \
    static const ::logict::sdk::PluginDescriptor descriptor{                                  \
        ::logict::sdk::kPluginAbiVersion,                                                     \
        []() -> ::logict::sdk::Driver* { return new DriverClass(); },                         \
        [](::logict::sdk::Driver* d) { delete d; }};                                          \
    return &descriptor;                                                                       \
  }
