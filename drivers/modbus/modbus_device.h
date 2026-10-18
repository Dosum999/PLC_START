#pragma once

// Modbus device logic shared by the TCP and RTU drivers: address handling,
// block coalescing, bad-address isolation, encoding and writes. Transports
// implement transact().

#include <chrono>
#include <set>
#include <string>
#include <vector>

#include "logict/sdk/block_planner.h"
#include "logict/sdk/driver.h"
#include "protocol.h"

namespace logict::modbus {

using namespace logict::sdk;

class ModbusDevice : public DeviceRuntime {
 public:
  // `prefix` is the driver's property namespace, e.g. "modbus_tcp".
  ModbusDevice(const DeviceConfig& cfg, HostContext& host, const std::string& prefix);

  Result<std::shared_ptr<const Address>> parse_address(std::string_view text, TagType requested) override;
  StatusCode read(std::span<ReadItem*> items) override;
  StatusCode write(const Address& address, const Value& value) override;

 protected:
  // Sends a request PDU and returns the response PDU (function code first).
  // Returns a communication status; Modbus exceptions are in the response.
  virtual StatusCode transact(const std::vector<std::uint8_t>& pdu, std::vector<std::uint8_t>& resp) = 0;

  HostContext& host_;
  std::chrono::milliseconds request_timeout_{1000};
  int attempts_ = 3;

 private:
  using Unit = std::pair<std::uint32_t, std::uint32_t>;  // (area, address)
  static const ModbusAddress& addr(const ReadItem& item) { return static_cast<const ModbusAddress&>(*item.address); }
  void remember_gaps(const Block& b, std::span<ReadItem*> items);
  std::vector<Block> split_at_bad_gaps(std::vector<Block> blocks, std::span<ReadItem*> items) const;
  void fail_item(ReadItem& item, std::uint8_t exception);
  void decode(ReadItem& item, const std::vector<std::uint8_t>& data, std::uint32_t base, Timestamp now);
  StatusCode read_range(Area area, std::uint32_t start, std::uint32_t count, std::vector<std::uint8_t>& out,
                        std::uint8_t& exception);
  StatusCode write_coils(const ModbusAddress& a, const Value& v);
  StatusCode write_registers(const ModbusAddress& a, const Value& v);
  StatusCode write_register_bit(const ModbusAddress& a, const Value& v);
  StatusCode write_pdu(const std::vector<std::uint8_t>& pdu);

  AddressOptions addr_opt_;
  Encoding enc_;
  bool use_fc05_ = true, use_fc06_ = true, mask_write_ = false;
  std::uint32_t max_coils_ = 2000, max_regs_ = 120, coil_gap_ = 64, reg_gap_ = 10;
  std::set<const ModbusAddress*> invalid_;
  std::set<Unit> no_span_;
};

// Device property definitions common to all Modbus drivers.
std::vector<nlohmann::json> common_device_schema(const std::string& prefix);

}  // namespace logict::modbus
