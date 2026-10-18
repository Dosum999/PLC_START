// Omron NJ/NX Ethernet driver: symbolic variable access over EtherNet/IP
// (CIP) with libplctag. The variables must be published ("Publish Only" or
// "Input/Output" network publish in Sysmac Studio).
//
// Device ID: IP address; the CIP route defaults to "18,<ip>" (built-in port).
// Addresses: MyVar, MyArray[3], MyStruct.Member, MyWord.5 (bit), MyArray[0]{10}.

#include "cip_device.h"

namespace {

using namespace logict;
using namespace logict::sdk;
using cip::prop;

class OmronNjDriver final : public Driver {
 public:
  DriverInfo info() const override {
    return {"omron_nj", "Omron NJ Ethernet", "0.1.0", Medium::Ethernet, {"NJ", "NX"}};
  }

  nlohmann::json schema() const override {
    using nlohmann::json;
    return {
        {"driver", "omron_nj"},
        {"display_name", "Omron NJ Ethernet"},
        {"channel", json::array()},
        {"device",
         json::array({
             prop("servermain.DEVICE_MODEL", "enum", "NJ", "NJ, NX"),
             prop("servermain.DEVICE_ID_STRING", "string", "192.168.250.1", "CPU IP address"),
             prop("omron_nj.DEVICE_ROUTING_PATH", "string", "", "CIP route (default: 18,<IP address>)"),
             prop("omron_nj.DEVICE_PORT", "integer", 44818, "EtherNet/IP port"),
         })},
        {"address_help",
         "Published variables: MyVar, MyArray[3], MyStruct.Member, MyWord.5 (bit), MyArray[0]{10} (10 elements). "
         "Data type Default uses the PLC's type."},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig&, const DeviceConfig& device,
                                                       HostContext& host) override {
    if (device.id.empty()) return make_error(status::BadConfigurationError, "device ID (IP address) is empty");
    return std::make_unique<cip::AbDevice>(device, host, "omron_nj", cip::Family::Omron);
  }
};

}  // namespace

LOGICT_DECLARE_DRIVER(OmronNjDriver)
