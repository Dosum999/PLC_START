#include "server.h"

#include "protocol.h"

namespace logict::modbus {

namespace {

std::vector<std::uint8_t> exception(std::uint8_t function, std::uint8_t code) {
  return {static_cast<std::uint8_t>(function | 0x80), code};
}

std::uint16_t at16(std::span<const std::uint8_t> pdu, std::size_t i) { return get16(pdu.data() + i); }

bool in_range(std::uint32_t start, std::uint32_t count) { return start + count <= 65536; }

std::vector<std::uint8_t> read_bits(const std::vector<std::uint8_t>& table, std::uint8_t fc, std::span<const std::uint8_t> pdu) {
  const std::uint16_t start = at16(pdu, 1), count = at16(pdu, 3);
  if (count < 1 || count > 2000) return exception(fc, kIllegalDataValue);
  if (!in_range(start, count)) return exception(fc, kIllegalDataAddress);
  std::vector<std::uint8_t> out{fc, static_cast<std::uint8_t>((count + 7) / 8)};
  out.resize(2 + out[1], 0);
  for (std::uint32_t i = 0; i < count; ++i)
    if (table[start + i]) out[2 + i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
  return out;
}

std::vector<std::uint8_t> read_regs(const std::vector<std::uint16_t>& table, std::uint8_t fc, std::uint16_t start,
                                    std::uint16_t count) {
  if (count < 1 || count > 125) return exception(fc, kIllegalDataValue);
  if (!in_range(start, count)) return exception(fc, kIllegalDataAddress);
  std::vector<std::uint8_t> out{fc, static_cast<std::uint8_t>(count * 2)};
  for (std::uint32_t i = 0; i < count; ++i) put16(out, table[start + i]);
  return out;
}

// Writes registers from `data` (2 bytes each); returns an exception code or 0.
std::uint8_t write_regs(std::vector<std::uint16_t>& table, std::uint16_t start, std::uint16_t count, std::uint8_t bytes,
                        std::span<const std::uint8_t> data, std::uint16_t max) {
  if (count < 1 || count > max || bytes != count * 2 || data.size() < bytes) return kIllegalDataValue;
  if (!in_range(start, count)) return kIllegalDataAddress;
  for (std::uint32_t i = 0; i < count; ++i) table[start + i] = get16(data.data() + 2 * i);
  return 0;
}

}  // namespace

std::vector<std::uint8_t> process_request(DataTable& t, std::span<const std::uint8_t> pdu, Redirect redirect) {
  if (pdu.empty()) return {};
  const std::uint8_t fc = pdu[0];
  auto& bits = redirect == Redirect::DiscreteInputs ? t.discrete : t.coils;
  auto& regs = redirect == Redirect::InputRegisters ? t.input : t.holding;
  auto need = [&](std::size_t n) { return pdu.size() >= n; };
  switch (fc) {
    case kReadCoils:
      if (!need(5)) break;
      return read_bits(bits, fc, pdu);
    case kReadDiscreteInputs:
      if (!need(5)) break;
      return read_bits(t.discrete, fc, pdu);
    case kReadHoldingRegisters:
      if (!need(5)) break;
      return read_regs(regs, fc, at16(pdu, 1), at16(pdu, 3));
    case kReadInputRegisters:
      if (!need(5)) break;
      return read_regs(t.input, fc, at16(pdu, 1), at16(pdu, 3));
    case kWriteSingleCoil: {
      if (!need(5)) break;
      const std::uint16_t v = at16(pdu, 3);
      if (v != 0xFF00 && v != 0x0000) return exception(fc, kIllegalDataValue);
      bits[at16(pdu, 1)] = v == 0xFF00;
      return {pdu.begin(), pdu.begin() + 5};
    }
    case kWriteSingleRegister:
      if (!need(5)) break;
      regs[at16(pdu, 1)] = at16(pdu, 3);
      return {pdu.begin(), pdu.begin() + 5};
    case kWriteMultipleCoils: {
      if (!need(6)) break;
      const std::uint16_t start = at16(pdu, 1), count = at16(pdu, 3);
      const std::uint8_t bytes = pdu[5];
      if (count < 1 || count > 1968 || bytes != (count + 7) / 8 || pdu.size() < 6u + bytes)
        return exception(fc, kIllegalDataValue);
      if (!in_range(start, count)) return exception(fc, kIllegalDataAddress);
      for (std::uint32_t i = 0; i < count; ++i) bits[start + i] = (pdu[6 + i / 8] >> (i % 8)) & 1;
      return {pdu.begin(), pdu.begin() + 5};
    }
    case kWriteMultipleRegisters: {
      if (!need(6)) break;
      if (auto e = write_regs(regs, at16(pdu, 1), at16(pdu, 3), pdu[5], pdu.subspan(6), 123)) return exception(fc, e);
      return {pdu.begin(), pdu.begin() + 5};
    }
    case kMaskWriteRegister: {
      if (!need(7)) break;
      auto& r = regs[at16(pdu, 1)];
      const std::uint16_t and_mask = at16(pdu, 3), or_mask = at16(pdu, 5);
      r = static_cast<std::uint16_t>((r & and_mask) | (or_mask & ~and_mask));
      return {pdu.begin(), pdu.begin() + 7};
    }
    case 0x17: {  // read/write multiple registers: the write happens first
      if (!need(10)) break;
      const std::uint16_t rstart = at16(pdu, 1), rcount = at16(pdu, 3);
      if (rcount < 1 || rcount > 125) return exception(fc, kIllegalDataValue);
      if (auto e = write_regs(regs, at16(pdu, 5), at16(pdu, 7), pdu[9], pdu.subspan(10), 121)) return exception(fc, e);
      return read_regs(regs, fc, rstart, rcount);
    }
    default: return exception(fc, kIllegalFunction);
  }
  return exception(fc, kIllegalDataValue);  // truncated request
}

std::size_t request_head_size(std::uint8_t function) {
  switch (function) {
    case 1: case 2: case 3: case 4: case 5: case 6: case 0x16: return 2;
    case 0x0F: case 0x10: return 7;  // address, function, start, count, byte count
    case 0x17: return 11;
    default: return 0;
  }
}

std::size_t rtu_request_length(std::span<const std::uint8_t> head) {
  switch (head[1]) {
    case 0x16: return 10;
    case 0x0F: case 0x10: return 9u + head[6];
    case 0x17: return 13u + head[10];
    default: return 8;
  }
}

}  // namespace logict::modbus
