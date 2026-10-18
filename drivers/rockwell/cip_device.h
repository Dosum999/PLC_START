#pragma once

// Tag access over EtherNet/IP (CIP) with libplctag, shared by the
// Allen-Bradley ControlLogix and Omron NJ/NX drivers. libplctag batches
// concurrent requests (Multiple Service Packet) and keeps one connection per
// PLC.
//
// Device ID: "<ip>" or Kepware style "<ip>,<path>" e.g. "192.168.1.10,1,0".
// Addresses: symbolic "Tag", "Program:Main.Tag", "Arr[3]", "Udt.Member",
// "MyDint.5" (bit), element count "{10}"; PCCC "N7:0", "F8:3", "B3:0/5",
// "ST9:0", "L19:2". Data type Default resolves from the PLC's type.

#include <algorithm>
#include <charconv>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <thread>

#include "libplctag.h"
#include "logict/sdk/driver.h"

namespace logict::cip {

using namespace logict::sdk;

enum class Family { Logix, Micro800, Pccc, Omron };

struct AbAddress final : Address {
  std::string name;            // libplctag tag name
  std::uint32_t count = 1;     // elements
  bool bit = false;            // "Dint.5" / "B3:0/5"
};

inline std::optional<DataType> from_cip(std::uint16_t code) {
  switch (code) {
    case 0xC1: return DataType::Boolean;
    case 0xC2: return DataType::Char;
    case 0xC3: return DataType::Short;
    case 0xC4: return DataType::Long;
    case 0xC5: return DataType::LLong;
    case 0xC6: return DataType::Byte;
    case 0xC7: return DataType::Word;
    case 0xC8: return DataType::DWord;
    case 0xC9: return DataType::QWord;
    case 0xCA: return DataType::Float;
    case 0xCB: return DataType::Double;
    case 0xD0: return DataType::String;  // CIP STRING (Omron)
    case 0xD1: return DataType::Byte;    // BYTE (8-bit bit string)
    case 0xD2: return DataType::Word;    // WORD
    case 0xD3: return DataType::DWord;   // DWORD; Logix BOOL[] packed in 32-bit words
    case 0xD4: return DataType::QWord;   // LWORD
    default: return std::nullopt;
  }
}

inline std::optional<DataType> from_pccc(int file_type) {
  switch (file_type) {
    case 0x85: return DataType::Boolean;  // B: read as 16-bit words; bits use "/n"
    case 0x89: return DataType::Short;    // N
    case 0x8A: return DataType::Float;    // F
    case 0x8D: return DataType::String;   // ST
    case 0x91: return DataType::Long;     // L
    default: return std::nullopt;
  }
}

inline sdk::StatusCode map_error(int rc) {
  switch (rc) {
    case PLCTAG_STATUS_OK: return status::Good;
    case PLCTAG_ERR_TIMEOUT: return status::BadTimeout;
    case PLCTAG_ERR_NOT_FOUND:
    case PLCTAG_ERR_OUT_OF_BOUNDS:
    case PLCTAG_ERR_BAD_PARAM: return status::BadOutOfRange;
    case PLCTAG_ERR_NOT_ALLOWED: return status::BadNotWritable;
    case PLCTAG_ERR_UNSUPPORTED: return status::BadNotSupported;
    case PLCTAG_ERR_BAD_CONNECTION:
    case PLCTAG_ERR_BAD_GATEWAY:
    case PLCTAG_ERR_BAD_DEVICE:
    case PLCTAG_ERR_OPEN:
    case PLCTAG_ERR_WINSOCK:
    case PLCTAG_ERR_READ:
    case PLCTAG_ERR_WRITE: return status::BadNotConnected;
    default: return status::BadDeviceFailure;
  }
}

inline bool is_comm(sdk::StatusCode st) { return st == status::BadNotConnected || st == status::BadTimeout; }

class AbDevice final : public DeviceRuntime {
 public:
  // `prefix`: property key prefix ("ab", "omron_nj"). The Omron NJ/NX driver
  // passes Family::Omron; otherwise the family follows the device model.
  AbDevice(const DeviceConfig& cfg, HostContext& host, const std::string& prefix, std::optional<Family> family = {})
      : host_(host) {
    std::string id = cfg.id;
    path_ = property<std::string>(cfg.properties, prefix + ".DEVICE_ROUTING_PATH", "");
    if (auto comma = id.find(','); comma != std::string::npos) {
      if (path_.empty()) path_ = id.substr(comma + 1);
      id.resize(comma);
    }
    gateway_ = id;
    if (auto port = property<int>(cfg.properties, prefix + ".DEVICE_PORT", 0); port > 0) gateway_ += ":" + std::to_string(port);
    std::string model = cfg.model.empty() ? "ControlLogix" : cfg.model;
    for (auto& c : model) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (family == Family::Omron) family_ = Family::Omron, plc_ = "omron-njnx";
    else if (model.find("micro8") != std::string::npos) family_ = Family::Micro800, plc_ = "micro800";
    else if (model.find("plc") != std::string::npos && model.find("5") != std::string::npos) family_ = Family::Pccc, plc_ = "plc5";
    else if (model.find("slc") != std::string::npos) family_ = Family::Pccc, plc_ = "slc500";
    else if (model.find("micrologix") != std::string::npos) family_ = Family::Pccc, plc_ = "micrologix";
    else family_ = Family::Logix, plc_ = "controllogix";
    if (family_ == Family::Logix && path_.empty()) path_ = "1,0";
    // NJ/NX: port 18 (built-in EtherNet/IP) to the CPU's own address.
    if (family_ == Family::Omron && path_.empty()) path_ = "18," + id;
    timeout_ms_ = static_cast<int>(cfg.request_timeout.count()) * std::max(1, cfg.attempts);
  }

  ~AbDevice() override { destroy_all(); }

  Result<std::shared_ptr<const Address>> parse_address(std::string_view text, TagType requested) override {
    auto a = std::make_shared<AbAddress>();
    a->text = std::string(text);
    std::string s;
    for (char c : text)
      if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(c);
    if (auto brace = s.find('{'); brace != std::string::npos) {
      if (s.back() != '}') return make_error(status::BadInvalidArgument, "expected {count} at the end");
      std::uint32_t n = 0;
      auto r = std::from_chars(s.data() + brace + 1, s.data() + s.size() - 1, n);
      if (r.ec != std::errc{} || n == 0 || n > 10000) return make_error(status::BadInvalidArgument, "invalid element count");
      a->count = n;
      s.resize(brace);
    }
    if (s.empty()) return make_error(status::BadInvalidArgument, "empty address");
    if (family_ == Family::Pccc) {
      if (s.find(':') == std::string::npos) return make_error(status::BadInvalidArgument, "PCCC addresses look like N7:0, F8:3, B3:0/5");
      a->bit = s.find('/') != std::string::npos;
    } else {
      // "Tag.5": the last dotted part is a bit number.
      auto dot = s.rfind('.');
      a->bit = dot != std::string::npos && dot + 1 < s.size() &&
               std::all_of(s.begin() + static_cast<std::ptrdiff_t>(dot) + 1, s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    }
    if (a->bit && (a->count != 1 || (requested.scalar != DataType::Default && requested.scalar != DataType::Boolean)))
      return make_error(status::BadTypeMismatch, "bit addresses are a single Boolean");
    a->name = s;
    TagType t = requested;
    if (a->bit) t.scalar = DataType::Boolean;
    // "{n}" sets the element count; an array data type alone (Kepware type
    // codes carry no length) does not.
    if (a->count > 1) {
      t.rows = a->count;
      t.cols = 0;
    } else if (t.is_array()) {
      a->count = t.element_count();
    }
    if (t.scalar == DataType::BCD || t.scalar == DataType::LBCD || t.scalar == DataType::Date)
      return make_error(status::BadTypeMismatch, "type not available over CIP");
    a->type = t;
    return std::shared_ptr<const Address>(std::move(a));
  }

  StatusCode connect(Deadline) override {
    online_ = true;  // libplctag connects on first use and keeps the session
    return status::Good;
  }
  void disconnect() override {
    destroy_all();  // drop handles so the next use reconnects cleanly
    online_ = false;
  }
  bool connected() const override { return online_; }

  StatusCode read(std::span<ReadItem*> items) override {
    const auto now = Clock::now();
    const auto deadline = SteadyClock::now() + std::chrono::milliseconds(timeout_ms_);
    std::vector<std::pair<ReadItem*, std::int32_t>> pending;
    int comm_failures = 0;
    StatusCode last_comm = status::Good;
    for (ReadItem* item : items) {
      const auto& a = addr(*item);
      if (invalid_.contains(&a)) {
        item->quality = status::BadOutOfRange;
        continue;
      }
      std::int32_t h = handle(a);
      if (h < 0) {
        item->quality = map_error(h);
        continue;
      }
      if (int rc = plc_tag_read(h, 0); rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        item->quality = map_error(rc);
        continue;
      }
      pending.emplace_back(item, h);
    }
    // libplctag packs the outstanding reads into Multiple Service Packets.
    for (auto& [item, h] : pending) {
      int rc = wait(h, deadline);
      if (rc == PLCTAG_STATUS_PENDING) {
        plc_tag_abort(h);
        rc = PLCTAG_ERR_TIMEOUT;
      }
      if (rc != PLCTAG_STATUS_OK) {
        item->quality = map_error(rc);
        if (is_comm(item->quality)) {
          ++comm_failures;
          last_comm = item->quality;
        } else if (item->quality == status::BadOutOfRange) {
          invalid_.insert(&addr(*item));
          host_.log.warn("'{}': {}; tag deactivated", item->address->text, plc_tag_decode_error(rc));
        }
        continue;
      }
      decode(*item, h, now);
    }
    // Every request failing to communicate means the PLC is unreachable.
    if (!pending.empty() && comm_failures == static_cast<int>(pending.size())) return last_comm;
    return status::Good;
  }

  StatusCode write(const Address& address, const Value& value) override {
    const auto& a = static_cast<const AbAddress&>(address);
    std::int32_t h = handle(a);
    if (h < 0) return map_error(h);
    DataType t = a.type.scalar;
    if (t == DataType::Default) {
      if (!types_.contains(&a)) {  // learn the PLC type first
        int rc = plc_tag_read(h, timeout_ms_);
        if (rc != PLCTAG_STATUS_OK) return map_error(rc);
        learn_type(a, h);
      }
      if (!types_.contains(&a)) return status::BadTypeMismatch;
      t = types_[&a];
    }
    const int size = plc_tag_get_size(h);
    const int elem = a.count ? size / static_cast<int>(a.count) : size;
    auto put = [&](const Value& v, int offset) -> StatusCode {
      if (a.bit) {
        auto b = convert(v, DataType::Boolean);
        if (!b) return b.error().code;
        return map_error(plc_tag_set_bit(h, 0, *b->get_if<bool>() ? 1 : 0));
      }
      if (t == DataType::String) {
        auto s = convert(v, DataType::String);
        if (!s) return s.error().code;
        return map_error(plc_tag_set_string(h, offset, s->get_if<std::string>()->c_str()));
      }
      auto c = convert(v, t);
      if (!c) return c.error().code;
      const Value& x = *c;
      int rc;
      switch (t) {
        case DataType::Boolean: rc = plc_tag_set_uint8(h, offset, *x.get_if<bool>() ? 1 : 0); break;
        case DataType::Char: rc = plc_tag_set_int8(h, offset, *x.get_if<std::int8_t>()); break;
        case DataType::Byte: rc = plc_tag_set_uint8(h, offset, *x.get_if<std::uint8_t>()); break;
        case DataType::Short: rc = plc_tag_set_int16(h, offset, *x.get_if<std::int16_t>()); break;
        case DataType::Word: rc = plc_tag_set_uint16(h, offset, *x.get_if<std::uint16_t>()); break;
        case DataType::Long: rc = plc_tag_set_int32(h, offset, *x.get_if<std::int32_t>()); break;
        case DataType::DWord: rc = plc_tag_set_uint32(h, offset, *x.get_if<std::uint32_t>()); break;
        case DataType::LLong: rc = plc_tag_set_int64(h, offset, *x.get_if<std::int64_t>()); break;
        case DataType::QWord: rc = plc_tag_set_uint64(h, offset, *x.get_if<std::uint64_t>()); break;
        case DataType::Float: rc = plc_tag_set_float32(h, offset, *x.get_if<float>()); break;
        case DataType::Double: rc = plc_tag_set_float64(h, offset, *x.get_if<double>()); break;
        default: return status::BadTypeMismatch;
      }
      return map_error(rc);
    };
    if (const auto* arr = value.get_if<Array>()) {
      if (arr->size() != a.count) return status::BadTypeMismatch;
      for (std::size_t i = 0; i < arr->size(); ++i)
        if (StatusCode st = put((*arr)[i], static_cast<int>(i) * elem); !is_good(st)) return st;
    } else if (a.count != 1) {
      return status::BadTypeMismatch;
    } else if (StatusCode st = put(value, 0); !is_good(st)) {
      return st;
    }
    int rc = plc_tag_write(h, timeout_ms_);
    if (rc == PLCTAG_ERR_UNSUPPORTED && a.bit && family_ != Family::Pccc) return write_bit_rmw(a, value);
    if (rc != PLCTAG_STATUS_OK) host_.log.warn("write to '{}' failed: {}", a.text, plc_tag_decode_error(rc));
    return map_error(rc);
  }

 private:
  static const AbAddress& addr(const ReadItem& item) { return static_cast<const AbAddress&>(*item.address); }

  std::int32_t handle(const AbAddress& a) {
    if (auto it = handles_.find(&a); it != handles_.end()) return it->second;
    std::string attrs = std::format("protocol=ab-eip&gateway={}&plc={}&name={}&elem_count={}", gateway_, plc_, a.name, a.count);
    if (!path_.empty()) attrs += "&path=" + path_;
    std::int32_t h = plc_tag_create(attrs.c_str(), timeout_ms_);
    if (h < 0) {
      host_.log.debug("create '{}' failed: {}", a.text, plc_tag_decode_error(h));
      if (map_error(h) == status::BadOutOfRange) {
        invalid_.insert(&a);
        host_.log.warn("'{}': {}; tag deactivated", a.text, plc_tag_decode_error(h));
      }
      return h;
    }
    handles_[&a] = h;
    return h;
  }

  static int wait(std::int32_t h, std::chrono::steady_clock::time_point deadline) {
    int rc;
    while ((rc = plc_tag_status(h)) == PLCTAG_STATUS_PENDING && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return rc;
  }

  void learn_type(const AbAddress& a, std::int32_t h) {
    if (a.bit) {
      types_[&a] = DataType::Boolean;
      return;
    }
    std::optional<DataType> t;
    if (family_ == Family::Pccc) {
      t = from_pccc(plc_tag_get_int_attribute(h, "elem_type", 0));
    } else {
      std::uint8_t raw[16] = {};
      int n = plc_tag_get_byte_array_attribute(h, "raw_tag_type_bytes", raw, sizeof(raw));
      if (n >= 2) {
        const std::uint16_t code = static_cast<std::uint16_t>(raw[0] | (raw[1] << 8));
        t = from_cip(code);
        // STRING (structure) is recognised by its element size: 88 bytes on Logix.
        if (!t && plc_tag_get_size(h) / static_cast<int>(a.count) == 88) t = DataType::String;
      }
    }
    if (t) types_[&a] = *t;
  }

  void decode(ReadItem& item, std::int32_t h, Timestamp now) {
    const auto& a = addr(item);
    if (!types_.contains(&a)) learn_type(a, h);
    DataType t = a.type.scalar;
    if (t == DataType::Default) {
      auto it = types_.find(&a);
      if (it == types_.end()) {
        item.quality = status::BadTypeMismatch;
        host_.log.warn("'{}': unknown PLC data type; set the tag's data type", a.text);
        return;
      }
      t = it->second;
    }
    const int size = plc_tag_get_size(h);
    const int elem = size / static_cast<int>(a.count);
    auto one = [&](int offset) -> Value {
      if (a.bit) return Value(plc_tag_get_bit(h, 0) != 0);
      switch (t) {
        case DataType::Boolean: return Value(plc_tag_get_uint8(h, offset) != 0);
        case DataType::Char: return Value(plc_tag_get_int8(h, offset));
        case DataType::Byte: return Value(plc_tag_get_uint8(h, offset));
        case DataType::Short: return Value(plc_tag_get_int16(h, offset));
        case DataType::Word: return Value(plc_tag_get_uint16(h, offset));
        case DataType::Long: return Value(plc_tag_get_int32(h, offset));
        case DataType::DWord: return Value(plc_tag_get_uint32(h, offset));
        case DataType::LLong: return Value(static_cast<std::int64_t>(plc_tag_get_int64(h, offset)));
        case DataType::QWord: return Value(static_cast<std::uint64_t>(plc_tag_get_uint64(h, offset)));
        case DataType::Float: return Value(plc_tag_get_float32(h, offset));
        case DataType::Double: return Value(plc_tag_get_float64(h, offset));
        case DataType::String: {
          char buf[256] = {};
          plc_tag_get_string(h, offset, buf, sizeof(buf));
          return Value(std::string(buf));
        }
        default: return Value();
      }
    };
    if (a.count == 1 && !a.type.is_array()) {
      item.value = one(0);
    } else {
      Array arr;
      for (std::uint32_t i = 0; i < a.count; ++i) arr.push_back(one(static_cast<int>(i) * elem));
      item.value = Value(std::move(arr));
    }
    item.quality = item.value.empty() ? status::BadTypeMismatch : status::Good;
    item.source_time = now;
  }

  // Bit write for PLCs without the CIP Read-Modify-Write service: read the
  // parent integer, change the bit and write the integer back.
  StatusCode write_bit_rmw(const AbAddress& a, const Value& value) {
    auto b = convert(value, DataType::Boolean);
    if (!b) return b.error().code;
    const auto dot = a.name.rfind('.');
    const int bit = std::stoi(a.name.substr(dot + 1));
    const std::string parent = a.name.substr(0, dot);
    std::string attrs = std::format("protocol=ab-eip&gateway={}&plc={}&name={}&elem_count=1", gateway_, plc_, parent);
    if (!path_.empty()) attrs += "&path=" + path_;
    std::int32_t h = plc_tag_create(attrs.c_str(), timeout_ms_);
    if (h < 0) return map_error(h);
    int rc = plc_tag_read(h, timeout_ms_);
    if (rc == PLCTAG_STATUS_OK && bit < plc_tag_get_size(h) * 8) {
      plc_tag_set_bit(h, bit, *b->get_if<bool>() ? 1 : 0);
      rc = plc_tag_write(h, timeout_ms_);
    } else if (rc == PLCTAG_STATUS_OK) {
      rc = PLCTAG_ERR_OUT_OF_BOUNDS;
    }
    plc_tag_destroy(h);
    if (rc != PLCTAG_STATUS_OK) host_.log.warn("write to '{}' failed: {}", a.text, plc_tag_decode_error(rc));
    return map_error(rc);
  }

  void destroy_all() {
    for (auto& [a, h] : handles_) plc_tag_destroy(h);
    handles_.clear();
  }

  HostContext& host_;
  std::string gateway_, path_, plc_;
  Family family_ = Family::Logix;
  int timeout_ms_ = 3000;
  bool online_ = false;
  std::map<const AbAddress*, std::int32_t> handles_;
  std::map<const AbAddress*, DataType> types_;
  std::set<const AbAddress*> invalid_;
};

inline nlohmann::json prop(const char* key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

}  // namespace logict::cip
