#include "modbus_device.h"

#include <algorithm>
#include <format>

namespace logict::modbus {

namespace {
nlohmann::json prop(const std::string& key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}
}  // namespace

ModbusDevice::ModbusDevice(const DeviceConfig& cfg, HostContext& host, const std::string& prefix) : host_(host) {
  const auto& p = cfg.properties;
  auto key = [&](const char* k) { return prefix + "." + k; };
  addr_opt_.zero_based = property(p, key("DEVICE_ZERO_BASED_ADDRESSING"), true);
  addr_opt_.zero_based_bits = property(p, key("DEVICE_ZERO_BASED_BIT_ADDRESSING"), true);
  enc_.modbus_byte_order = property(p, key("DEVICE_MODBUS_BYTE_ORDER"), true);
  enc_.first_word_low = property(p, key("DEVICE_FIRST_WORD_LOW"), true);
  enc_.first_dword_low = property(p, key("DEVICE_FIRST_DWORD_LOW"), true);
  use_fc05_ = property(p, key("DEVICE_USE_FC05"), true);
  use_fc06_ = property(p, key("DEVICE_USE_FC06"), true);
  mask_write_ = property(p, key("DEVICE_HOLDING_REGISTER_MASK_WRITES"), false);
  max_coils_ = std::clamp<std::uint32_t>(property<std::uint32_t>(p, key("DEVICE_MAX_COILS_PER_REQUEST"), 2000), 8, 2000);
  max_regs_ = std::clamp<std::uint32_t>(property<std::uint32_t>(p, key("DEVICE_MAX_REGISTERS_PER_REQUEST"), 120), 1, 125);
  coil_gap_ = property<std::uint32_t>(p, key("DEVICE_MAX_COIL_GAP"), 64);
  reg_gap_ = property<std::uint32_t>(p, key("DEVICE_MAX_REGISTER_GAP"), 10);
  request_timeout_ = cfg.request_timeout;
  attempts_ = std::max(1, cfg.attempts);
}

std::vector<nlohmann::json> common_device_schema(const std::string& prefix) {
  auto key = [&](const char* k) { return prefix + "." + k; };
  return {
      prop(key("DEVICE_ZERO_BASED_ADDRESSING"), "boolean", true, "Address 40001 is protocol register 0"),
      prop(key("DEVICE_ZERO_BASED_BIT_ADDRESSING"), "boolean", true, "Register bits are numbered 0-15 (else 1-16)"),
      prop(key("DEVICE_MODBUS_BYTE_ORDER"), "boolean", true, "High byte first within a register"),
      prop(key("DEVICE_FIRST_WORD_LOW"), "boolean", true, "Low word first in 32-bit values"),
      prop(key("DEVICE_FIRST_DWORD_LOW"), "boolean", true, "Low double word first in 64-bit values"),
      prop(key("DEVICE_USE_FC05"), "boolean", true, "Write single coils with function 05 (else 15)"),
      prop(key("DEVICE_USE_FC06"), "boolean", true, "Write single registers with function 06 (else 16)"),
      prop(key("DEVICE_HOLDING_REGISTER_MASK_WRITES"), "boolean", false, "Write register bits with function 22 (else read-modify-write)"),
      prop(key("DEVICE_MAX_COILS_PER_REQUEST"), "integer", 2000, "Coils per read request (8-2000)"),
      prop(key("DEVICE_MAX_REGISTERS_PER_REQUEST"), "integer", 120, "Registers per read request (1-125)"),
      prop(key("DEVICE_MAX_COIL_GAP"), "integer", 64, "Unused coils allowed between merged tags"),
      prop(key("DEVICE_MAX_REGISTER_GAP"), "integer", 10, "Unused registers allowed between merged tags"),
  };
}

Result<std::shared_ptr<const Address>> ModbusDevice::parse_address(std::string_view text, TagType requested) {
  auto a = modbus::parse_address(text, requested, addr_opt_);
  if (!a) return std::unexpected(a.error());
  return std::shared_ptr<const Address>(std::move(*a));
}

StatusCode ModbusDevice::read(std::span<ReadItem*> items) {
  const auto now = Clock::now();
  std::vector<PlanItem> plan;
  plan.reserve(items.size());
  for (std::size_t i = 0; i < items.size(); ++i) {
    const auto& a = addr(*items[i]);
    if (invalid_.contains(&a)) {
      items[i]->quality = status::BadOutOfRange;
      continue;
    }
    plan.push_back({static_cast<std::uint32_t>(a.area), a.offset, a.units(), i});
  }
  auto blocks = split_at_bad_gaps(plan_blocks(plan, [this](std::uint32_t area) {
    return is_bit_area(static_cast<Area>(area)) ? PlanLimits{max_coils_, coil_gap_}
                                                        : PlanLimits{max_regs_, reg_gap_};
  }), items);

  for (const auto& b : blocks) {
    const auto area = static_cast<Area>(b.area);
    std::vector<std::uint8_t> data;
    std::uint8_t exception = 0;
    StatusCode st = read_range(area, static_cast<std::uint32_t>(b.start), b.length, data, exception);
    if (!is_good(st)) return st;  // communication failure: abort the whole transaction

    if (exception == 0) {
      for (std::size_t idx : b.items) decode(*items[idx], data, static_cast<std::uint32_t>(b.start), now);
      continue;
    }
    if (exception != 0x02 || b.items.size() == 1) {
      for (std::size_t idx : b.items) fail_item(*items[idx], exception);
      continue;
    }
    // Illegal address somewhere in a merged block: isolate the bad tags so
    // the good ones keep working (bad ones are skipped from now on), and
    // stop merging across the unused gaps of this block (the device may
    // not implement them).
    remember_gaps(b, items);
    for (std::size_t idx : b.items) {
      const auto& a = addr(*items[idx]);
      std::vector<std::uint8_t> one;
      std::uint8_t exc = 0;
      st = read_range(a.area, a.offset, a.units(), one, exc);
      if (!is_good(st)) return st;
      if (exc == 0) {
        decode(*items[idx], one, a.offset, now);
      } else {
        fail_item(*items[idx], exc);
      }
    }
  }
  return status::Good;
}

StatusCode ModbusDevice::write(const Address& address, const Value& value) {
  const auto& a = static_cast<const ModbusAddress&>(address);
  switch (a.area) {
    case Area::DiscreteInput:
    case Area::InputRegister: return status::BadNotWritable;
    case Area::Coil: return write_coils(a, value);
    case Area::HoldingRegister: return a.bit >= 0 ? write_register_bit(a, value) : write_registers(a, value);
  }
  return status::BadNotSupported;
}




// Records the unused ranges between the items of a block as "do not span".
void ModbusDevice::remember_gaps(const Block& b, std::span<ReadItem*> items) {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
  for (std::size_t idx : b.items) ranges.emplace_back(addr(*items[idx]).offset, addr(*items[idx]).offset + addr(*items[idx]).units());
  std::sort(ranges.begin(), ranges.end());
  std::uint32_t covered = ranges.front().second;
  for (std::size_t i = 1; i < ranges.size(); ++i) {
    for (std::uint32_t u = covered; u < ranges[i].first; ++u) no_span_.insert({b.area, u});
    covered = std::max(covered, ranges[i].second);
  }
}

// Splits planned blocks so none spans a remembered bad gap.
std::vector<Block> ModbusDevice::split_at_bad_gaps(std::vector<Block> blocks, std::span<ReadItem*> items) const {
  if (no_span_.empty()) return blocks;
  std::vector<Block> out;
  for (auto& b : blocks) {
    std::vector<std::size_t> order = b.items;
    std::sort(order.begin(), order.end(),
              [&](std::size_t x, std::size_t y) { return addr(*items[x]).offset < addr(*items[y]).offset; });
    Block cur{};
    bool open = false;
    for (std::size_t idx : order) {
      const auto& a = addr(*items[idx]);
      const std::uint32_t end = a.offset + a.units();
      if (open) {
        const std::uint32_t cur_end = static_cast<std::uint32_t>(cur.start) + cur.length;
        auto it = no_span_.lower_bound({b.area, cur_end});
        const bool blocked = it != no_span_.end() && it->first == b.area && it->second < a.offset;
        if (!blocked) {
          cur.length = std::max(cur_end, end) - static_cast<std::uint32_t>(cur.start);
          cur.items.push_back(idx);
          continue;
        }
        out.push_back(std::move(cur));
      }
      cur = Block{b.area, a.offset, a.units(), {idx}, a.units() > (is_bit_area(static_cast<Area>(b.area)) ? max_coils_ : max_regs_)};
      open = true;
    }
    if (open) out.push_back(std::move(cur));
  }
  return out;
}

void ModbusDevice::fail_item(ReadItem& item, std::uint8_t exception) {
  item.quality = exception_status(exception);
  if (exception == 0x02) {
    const auto& a = addr(item);
    invalid_.insert(&a);
    for (std::uint32_t u = a.offset; u < a.offset + a.units(); ++u)
      no_span_.insert({static_cast<std::uint32_t>(a.area), u});
    host_.log.warn("address '{}' rejected by device (illegal data address); tag deactivated", item.address->text);
  }
}

void ModbusDevice::decode(ReadItem& item, const std::vector<std::uint8_t>& data, std::uint32_t base, Timestamp now) {
  const auto& a = addr(item);
  const std::uint32_t rel = a.offset - base;
  if (is_bit_area(a.area)) {
    item.value = decode_bits(a, data, rel);
    item.quality = status::Good;
  } else {
    auto v = decode_registers(a, std::span(data).subspan(rel * 2), enc_);
    if (v) {
      item.value = std::move(*v);
      item.quality = status::Good;
    } else {
      item.quality = v.error().code;
    }
  }
  item.source_time = now;
}

// Reads `count` units starting at `start`, splitting into protocol-sized
// requests. Register data is returned as wire bytes, coils as packed bits.
StatusCode ModbusDevice::read_range(Area area, std::uint32_t start, std::uint32_t count, std::vector<std::uint8_t>& out,
                      std::uint8_t& exception) {
  const bool bits = is_bit_area(area);
  const std::uint32_t max = bits ? max_coils_ : max_regs_;
  out.assign(bits ? (count + 7) / 8 : count * 2, 0);
  for (std::uint32_t done = 0; done < count;) {
    const std::uint32_t n = std::min(max, count - done);
    std::vector<std::uint8_t> resp;
    StatusCode st = transact(make_read(area, static_cast<std::uint16_t>(start + done), static_cast<std::uint16_t>(n)), resp);
    if (!is_good(st)) return st;
    if (resp[0] & 0x80) {
      exception = resp.size() > 1 ? resp[1] : 0xFF;
      return status::Good;
    }
    const std::size_t expect = bits ? (n + 7) / 8 : n * 2;
    if (resp.size() < 2 || resp[1] != expect || resp.size() != 2 + expect) return status::BadCommunicationError;
    if (bits) {
      for (std::uint32_t i = 0; i < n; ++i)
        if ((resp[2 + i / 8] >> (i % 8)) & 1) out[(done + i) / 8] |= static_cast<std::uint8_t>(1u << ((done + i) % 8));
    } else {
      std::copy(resp.begin() + 2, resp.end(), out.begin() + done * 2);
    }
    done += n;
  }
  return status::Good;
}

StatusCode ModbusDevice::write_coils(const ModbusAddress& a, const Value& v) {
  std::vector<bool> bits;
  if (const auto* arr = v.get_if<Array>()) {
    for (const auto& e : *arr) {
      auto b = convert(e, DataType::Boolean);
      if (!b) return b.error().code;
      bits.push_back(*b->get_if<bool>());
    }
  } else {
    auto b = convert(v, DataType::Boolean);
    if (!b) return b.error().code;
    bits.push_back(*b->get_if<bool>());
  }
  std::vector<std::uint8_t> pdu;
  if (bits.size() == 1 && use_fc05_) {
    pdu = {kWriteSingleCoil};
    put16(pdu, static_cast<std::uint16_t>(a.offset));
    put16(pdu, bits[0] ? 0xFF00 : 0x0000);
  } else {
    pdu = {kWriteMultipleCoils};
    put16(pdu, static_cast<std::uint16_t>(a.offset));
    put16(pdu, static_cast<std::uint16_t>(bits.size()));
    pdu.push_back(static_cast<std::uint8_t>((bits.size() + 7) / 8));
    std::vector<std::uint8_t> packed((bits.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < bits.size(); ++i)
      if (bits[i]) packed[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
    pdu.insert(pdu.end(), packed.begin(), packed.end());
  }
  return write_pdu(pdu);
}

StatusCode ModbusDevice::write_registers(const ModbusAddress& a, const Value& v) {
  auto data = encode_registers(a, v, enc_);
  if (!data) return data.error().code;
  const std::size_t regs = data->size() / 2;
  if (regs > 123) return status::BadOutOfRange;  // FC16 limit
  std::vector<std::uint8_t> pdu;
  if (regs == 1 && use_fc06_) {
    pdu = {kWriteSingleRegister};
    put16(pdu, static_cast<std::uint16_t>(a.offset));
  } else {
    pdu = {kWriteMultipleRegisters};
    put16(pdu, static_cast<std::uint16_t>(a.offset));
    put16(pdu, static_cast<std::uint16_t>(regs));
    pdu.push_back(static_cast<std::uint8_t>(data->size()));
  }
  pdu.insert(pdu.end(), data->begin(), data->end());
  return write_pdu(pdu);
}

StatusCode ModbusDevice::write_register_bit(const ModbusAddress& a, const Value& v) {
  auto b = convert(v, DataType::Boolean);
  if (!b) return b.error().code;
  const bool on = *b->get_if<bool>();
  // Bit numbering refers to the register value, so translate through the
  // configured byte order to find the bit on the wire.
  const std::uint16_t value_mask = static_cast<std::uint16_t>(1u << a.bit);
  const std::uint16_t wire_mask = enc_.modbus_byte_order ? value_mask : static_cast<std::uint16_t>((value_mask >> 8) | (value_mask << 8));
  std::vector<std::uint8_t> pdu;
  if (mask_write_) {
    pdu = {kMaskWriteRegister};
    put16(pdu, static_cast<std::uint16_t>(a.offset));
    put16(pdu, static_cast<std::uint16_t>(~wire_mask));       // AND mask
    put16(pdu, on ? wire_mask : std::uint16_t{0});              // OR mask
    return write_pdu(pdu);
  }
  std::vector<std::uint8_t> cur;
  std::uint8_t exc = 0;
  if (auto st = read_range(Area::HoldingRegister, a.offset, 1, cur, exc); !is_good(st)) return st;
  if (exc) return exception_status(exc);
  std::uint16_t reg = get16(cur.data());
  reg = on ? (reg | wire_mask) : (reg & static_cast<std::uint16_t>(~wire_mask));
  pdu = {use_fc06_ ? kWriteSingleRegister : kWriteMultipleRegisters};
  put16(pdu, static_cast<std::uint16_t>(a.offset));
  if (!use_fc06_) {
    put16(pdu, 1);
    pdu.push_back(2);
  }
  put16(pdu, reg);
  return write_pdu(pdu);
}

StatusCode ModbusDevice::write_pdu(const std::vector<std::uint8_t>& pdu) {
  std::vector<std::uint8_t> resp;
  if (auto st = transact(pdu, resp); !is_good(st)) return st;
  if (resp[0] & 0x80) return exception_status(resp.size() > 1 ? resp[1] : 0xFF);
  if (resp[0] != pdu[0] || resp.size() < 5) return status::BadCommunicationError;
  return status::Good;
}


}  // namespace logict::modbus
