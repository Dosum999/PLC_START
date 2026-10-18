// Simulator driver, modelled on the Kepware Simulator.
//
// Addresses (case-insensitive):
//   R<n> / K<n>  numeric register, read/write, default type Word
//   B<n>         boolean, read/write
//   S<n>         string, read/write
//   any of the above with " [rows]" or " [rows][cols]" for arrays, e.g. "R0010 [8]"
//   RAMP(rate_ms, low, high, increment)          read-only, default Long
//   RANDOM(rate_ms, low, high)                   read-only, default Long
//   SINE(rate_ms, low, high, frequency_hz, phase_deg)  read-only, default Double
//   USER(rate_ms, v1, v2, ...)                   read-only, cycles values, default String
//
// A device whose ID is "offline" refuses to connect (handy for testing error
// handling and auto-demotion).

#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <numbers>
#include <random>

#include "logict/sdk/driver.h"

namespace {

using namespace logict::sdk;

enum class Kind { Register, Bool, String, Ramp, Random, Sine, User };

struct SimAddress final : Address {
  Kind kind = Kind::Register;
  std::uint32_t index = 0;
  std::vector<double> args;
  std::vector<std::string> user_values;
  bool writable() const { return kind == Kind::Register || kind == Kind::Bool || kind == Kind::String; }
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

std::optional<double> to_num(std::string_view s) {
  s = trim(s);
  double v;
  auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
  return v;
}

// Splits "a [r][c]" into "a" and dimensions.
bool split_array(std::string_view& s, std::uint32_t& rows, std::uint32_t& cols) {
  rows = cols = 0;
  auto pos = s.find('[');
  if (pos == std::string_view::npos) return true;
  std::string_view dims = s.substr(pos);
  s = trim(s.substr(0, pos));
  std::uint32_t* target = &rows;
  while (!dims.empty()) {
    dims = trim(dims);
    if (dims.empty()) break;
    if (dims.front() != '[' || target == nullptr) return false;
    auto close = dims.find(']');
    if (close == std::string_view::npos) return false;
    auto n = to_num(dims.substr(1, close - 1));
    if (!n || *n < 1 || *n > 10000) return false;
    *target = static_cast<std::uint32_t>(*n);
    target = target == &rows ? &cols : nullptr;
    dims.remove_prefix(close + 1);
  }
  return true;
}

class SimDevice final : public DeviceRuntime {
 public:
  SimDevice(const DeviceConfig& cfg, HostContext& host) : offline_(upper(cfg.id) == "OFFLINE"), host_(host) {}

  Result<std::shared_ptr<const Address>> parse_address(std::string_view text, TagType requested) override {
    auto a = std::make_shared<SimAddress>();
    a->text = std::string(text);
    std::string s = upper(trim(text));

    if (auto open = s.find('('); open != std::string::npos) {
      if (s.back() != ')') return make_error(status::BadInvalidArgument, "missing ')'");
      std::string fn = std::string(trim(std::string_view(s).substr(0, open)));
      std::vector<std::string> args;
      std::string_view rest = std::string_view(text).substr(text.find('(') + 1);
      rest = rest.substr(0, rest.rfind(')'));
      std::size_t start = 0;
      while (start <= rest.size()) {
        auto comma = rest.find(',', start);
        args.emplace_back(trim(rest.substr(start, comma - start)));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
      }
      if (fn == "USER") {
        if (args.size() < 2) return make_error(status::BadInvalidArgument, "USER(rate, v1, ...)");
        a->kind = Kind::User;
        auto rate = to_num(args[0]);
        if (!rate || *rate < 10) return make_error(status::BadInvalidArgument, "rate must be >= 10 ms");
        a->args = {*rate};
        a->user_values.assign(args.begin() + 1, args.end());
        a->type = TagType{requested.scalar == DataType::Default ? DataType::String : requested.scalar};
        return a;
      }
      std::size_t expected;
      if (fn == "RAMP") a->kind = Kind::Ramp, expected = 4;
      else if (fn == "RANDOM") a->kind = Kind::Random, expected = 3;
      else if (fn == "SINE") a->kind = Kind::Sine, expected = 5;
      else return make_error(status::BadInvalidArgument, std::format("unknown function '{}'", fn));
      if (args.size() != expected)
        return make_error(status::BadInvalidArgument, std::format("{} expects {} arguments", fn, expected));
      for (auto& arg : args) {
        auto n = to_num(arg);
        if (!n) return make_error(status::BadInvalidArgument, std::format("bad argument '{}'", arg));
        a->args.push_back(*n);
      }
      if (a->args[0] < 10) return make_error(status::BadInvalidArgument, "rate must be >= 10 ms");
      if (a->args[2] < a->args[1]) return make_error(status::BadInvalidArgument, "high must be >= low");
      DataType def = a->kind == Kind::Sine ? DataType::Double : DataType::Long;
      a->type = TagType{requested.scalar == DataType::Default ? def : requested.scalar};
      if (a->type.scalar == DataType::String || a->type.scalar == DataType::Date)
        return make_error(status::BadTypeMismatch, "function requires a numeric type");
      return a;
    }

    std::string_view body = s;
    std::uint32_t rows, cols;
    if (!split_array(body, rows, cols)) return make_error(status::BadInvalidArgument, "bad array dimensions");
    if (body.size() < 2) return make_error(status::BadInvalidArgument, "expected R, K, B or S followed by a number");
    auto idx = to_num(body.substr(1));
    if (!idx || *idx < 0 || *idx > 9999 || std::floor(*idx) != *idx)
      return make_error(status::BadInvalidArgument, "index must be 0..9999");
    a->index = static_cast<std::uint32_t>(*idx);
    DataType def;
    switch (body.front()) {
      case 'R':
      case 'K': a->kind = Kind::Register, def = DataType::Word; break;
      case 'B': a->kind = Kind::Bool, def = DataType::Boolean; break;
      case 'S': a->kind = Kind::String, def = DataType::String; break;
      default: return make_error(status::BadInvalidArgument, "expected R, K, B or S");
    }
    DataType t = requested.scalar == DataType::Default ? def : requested.scalar;
    if (a->kind == Kind::Bool && t != DataType::Boolean) return make_error(status::BadTypeMismatch, "B requires Boolean");
    if (a->kind == Kind::String && t != DataType::String) return make_error(status::BadTypeMismatch, "S requires String");
    if (a->kind == Kind::Register && (t == DataType::String || t == DataType::Boolean))
      return make_error(status::BadTypeMismatch, "R requires a numeric type");
    a->type = TagType{t, rows, cols};
    if (a->index + a->type.element_count() > 10000) return make_error(status::BadOutOfRange, "array exceeds R9999");
    return a;
  }

  StatusCode connect(Deadline) override {
    if (offline_) return status::BadNotConnected;
    connected_ = true;
    return status::Good;
  }
  void disconnect() override { connected_ = false; }
  bool connected() const override { return connected_; }

  StatusCode read(std::span<ReadItem*> items) override {
    if (!connected_) return status::BadNotConnected;
    auto now = Clock::now();
    for (ReadItem* item : items) {
      const auto& a = static_cast<const SimAddress&>(*item->address);
      auto v = evaluate(a, now);
      if (!v) {
        item->quality = v.error().code;
        continue;
      }
      item->value = std::move(*v);
      item->quality = status::Good;
      item->source_time = now;
    }
    return status::Good;
  }

  StatusCode write(const Address& address, const Value& value) override {
    if (!connected_) return status::BadNotConnected;
    const auto& a = static_cast<const SimAddress&>(address);
    if (!a.writable()) return status::BadNotWritable;
    if (a.type.is_array()) {
      const auto* arr = value.get_if<Array>();
      if (!arr || arr->size() != a.type.element_count()) return status::BadTypeMismatch;
      for (std::uint32_t i = 0; i < arr->size(); ++i) memory_[key(a, i)] = (*arr)[i];
    } else {
      memory_[key(a, 0)] = value;
    }
    return status::Good;
  }

 private:
  static std::uint64_t key(const SimAddress& a, std::uint32_t offset) {
    return (static_cast<std::uint64_t>(a.kind) << 32) | (a.index + offset);
  }

  Result<Value> evaluate(const SimAddress& a, Timestamp now) {
    const double ms = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    switch (a.kind) {
      case Kind::Register:
      case Kind::Bool:
      case Kind::String: {
        TagType scalar{a.type.scalar};
        if (!a.type.is_array()) {
          auto it = memory_.find(key(a, 0));
          return it == memory_.end() ? default_value(scalar) : convert(it->second, a.type.scalar);
        }
        Array arr;
        for (std::uint32_t i = 0; i < a.type.element_count(); ++i) {
          auto it = memory_.find(key(a, i));
          auto v = it == memory_.end() ? Result<Value>(default_value(scalar)) : convert(it->second, a.type.scalar);
          if (!v) return v;
          arr.push_back(std::move(*v));
        }
        return Value(std::move(arr));
      }
      case Kind::Ramp: {
        const double rate = a.args[0], lo = a.args[1], hi = a.args[2], inc = a.args[3];
        const double steps = std::floor(ms / rate);
        const double span = hi - lo + (inc > 0 ? inc : 1);
        double v = lo + std::fmod(steps * inc, span > 0 ? span : 1);
        if (v > hi) v = lo;
        return convert(Value(v), a.type.scalar);
      }
      case Kind::Random: {
        const double rate = a.args[0], lo = a.args[1], hi = a.args[2];
        // Deterministic per time slot so all readers within a slot agree.
        std::mt19937_64 rng(static_cast<std::uint64_t>(std::floor(ms / rate)) ^ (std::uint64_t{a.index} << 48));
        std::uniform_real_distribution<double> dist(lo, hi);
        double v = dist(rng);
        if (a.type.scalar != DataType::Float && a.type.scalar != DataType::Double) v = std::round(v);
        return convert(Value(v), a.type.scalar);
      }
      case Kind::Sine: {
        const double rate = a.args[0], lo = a.args[1], hi = a.args[2], freq = a.args[3], phase = a.args[4];
        const double t = std::floor(ms / rate) * rate / 1000.0;
        const double s = std::sin(2 * std::numbers::pi * freq * t + phase * std::numbers::pi / 180.0);
        return convert(Value(lo + (s + 1) / 2 * (hi - lo)), a.type.scalar);
      }
      case Kind::User: {
        const auto n = a.user_values.size();
        const auto i = static_cast<std::size_t>(std::floor(ms / a.args[0])) % n;
        return convert(Value(a.user_values[i]), a.type.scalar);
      }
    }
    return make_error(status::BadInternalError, "unreachable");
  }

  bool offline_;
  bool connected_ = false;
  [[maybe_unused]] HostContext& host_;
  std::map<std::uint64_t, Value> memory_;
};

class SimulatorDriver final : public Driver {
 public:
  DriverInfo info() const override {
    return {"simulator", "Simulator", "0.1.0", Medium::Virtual, {"16 Bit Device"}};
  }

  nlohmann::json schema() const override {
    return {
        {"driver", "simulator"},
        {"display_name", "Simulator"},
        {"channel", nlohmann::json::array()},
        {"device", nlohmann::json::array({
                       {{"key", "servermain.DEVICE_ID_STRING"},
                        {"type", "string"},
                        {"default", "1"},
                        {"description", "Any value; 'offline' simulates an unreachable device"}},
                   })},
        {"address_help",
         "R<n>/K<n> register, B<n> bool, S<n> string (n = 0..9999, optional [rows][cols]); "
         "RAMP(rate,low,high,inc), RANDOM(rate,low,high), SINE(rate,low,high,freq,phase), USER(rate,v1,...)"},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig&, const DeviceConfig& device,
                                                       HostContext& host) override {
    return std::make_unique<SimDevice>(device, host);
  }
};

}  // namespace

LOGICT_DECLARE_DRIVER(SimulatorDriver)
