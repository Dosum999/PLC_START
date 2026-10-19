// Memory Based driver, modelled on the Kepware Memory Based driver: tags whose
// values live in server memory, written by clients (OPC UA, MQTT) and read back.
//
// Addresses (case-insensitive):
//   RAM                 each tag has its own value (Kepware)
//   RAM.<name>          tags with the same name share one value (extension)
//   either with " [rows]" or " [rows][cols]" for arrays, e.g. "RAM [10]"
// The default type is Double. Values start at zero / empty and are lost when
// the server stops, as in Kepware.

#include <cctype>
#include <charconv>
#include <map>

#include "logict/sdk/driver.h"

namespace {

using namespace logict::sdk;

struct MemAddress final : Address {
  std::string name;  // empty: the tag's own value
};

std::string upper(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

// Splits "RAM [r][c]" into "RAM" and dimensions.
bool split_array(std::string_view& s, std::uint32_t& rows, std::uint32_t& cols) {
  rows = cols = 0;
  auto pos = s.find('[');
  if (pos == std::string_view::npos) return true;
  std::string_view dims = s.substr(pos);
  s = trim(s.substr(0, pos));
  std::uint32_t* target = &rows;
  while (!(dims = trim(dims)).empty()) {
    auto close = dims.find(']');
    if (dims.front() != '[' || target == nullptr || close == std::string_view::npos) return false;
    std::string_view n = trim(dims.substr(1, close - 1));
    auto r = std::from_chars(n.data(), n.data() + n.size(), *target);
    if (r.ec != std::errc{} || r.ptr != n.data() + n.size() || *target < 1 || *target > 65535) return false;
    target = target == &rows ? &cols : nullptr;
    dims.remove_prefix(close + 1);
  }
  return true;
}

class MemoryDevice final : public DeviceRuntime {
 public:
  Result<std::shared_ptr<const Address>> parse_address(std::string_view text, TagType requested) override {
    auto a = std::make_shared<MemAddress>();
    a->text = std::string(text);
    std::string_view body = trim(text);
    std::uint32_t rows, cols;
    if (!split_array(body, rows, cols)) return make_error(status::BadInvalidArgument, "bad array dimensions");
    const std::string u = upper(body);
    if (u.starts_with("RAM.") && u.size() > 4) a->name = u.substr(4);
    else if (u != "RAM") return make_error(status::BadInvalidArgument, "expected RAM or RAM.<name>");
    a->type = TagType{requested.scalar == DataType::Default ? DataType::Double : requested.scalar, rows, cols};
    if (a->type.element_count() > 65535) return make_error(status::BadOutOfRange, "array too large");
    return a;
  }

  StatusCode connect(Deadline) override { return status::Good; }
  void disconnect() override {}
  bool connected() const override { return true; }

  StatusCode read(std::span<ReadItem*> items) override {
    const auto now = Clock::now();
    for (ReadItem* item : items) {
      const auto& a = static_cast<const MemAddress&>(*item->address);
      auto v = load(a);
      item->quality = v ? status::Good : v.error().code;
      if (v) item->value = std::move(*v);
      item->source_time = now;
    }
    return status::Good;
  }

  StatusCode write(const Address& address, const Value& value) override {
    const auto& a = static_cast<const MemAddress&>(address);
    auto v = convert(value, a.type.scalar);
    if (!v) return v.error().code;
    if (a.type.is_array()) {
      const auto* arr = v->get_if<Array>();
      if (!arr || arr->size() != a.type.element_count()) return status::BadTypeMismatch;
    } else if (v->is_array()) {
      return status::BadTypeMismatch;
    }
    if (a.name.empty()) own_[&a] = std::move(*v);
    else shared_[a.name] = std::move(*v);
    return status::Good;
  }

 private:
  Result<Value> load(const MemAddress& a) const {
    const Value* stored = nullptr;
    if (a.name.empty()) {
      if (auto it = own_.find(&a); it != own_.end()) stored = &it->second;
    } else if (auto it = shared_.find(a.name); it != shared_.end()) {
      stored = &it->second;
    }
    if (stored == nullptr) return default_value(a.type);
    // A shared value written through a tag of another type or shape.
    auto v = convert(*stored, a.type.scalar);
    if (!v) return v;
    if (a.type.is_array() != v->is_array() ||
        (a.type.is_array() && v->get_if<Array>()->size() != a.type.element_count()))
      return make_error(status::BadTypeMismatch, "stored value has another shape");
    return v;
  }

  // Keyed by the parsed address, which the host keeps for the tag's lifetime.
  std::map<const MemAddress*, Value> own_;
  std::map<std::string, Value> shared_;
};

class MemoryBasedDriver final : public Driver {
 public:
  DriverInfo info() const override { return {"memory_based", "Memory Based", "0.1.0", Medium::Virtual, {"Memory"}}; }

  nlohmann::json schema() const override {
    return {
        {"driver", "memory_based"},
        {"display_name", "Memory Based"},
        {"channel", nlohmann::json::array()},
        {"device", nlohmann::json::array()},
        {"address_help", "RAM (each tag its own value) or RAM.<name> (shared by name), optional [rows][cols]."},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig&, const DeviceConfig&, HostContext&) override {
    return std::make_unique<MemoryDevice>();
  }
};

}  // namespace

LOGICT_DECLARE_DRIVER(MemoryBasedDriver)
