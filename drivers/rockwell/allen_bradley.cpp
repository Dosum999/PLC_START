// Allen-Bradley ControlLogix Ethernet driver (EtherNet/IP) on libplctag.
//
// Models: ControlLogix / CompactLogix (symbolic Logix tags, path "1,0"),
// Micro800, and PCCC PLCs (PLC-5, SLC 500, MicroLogix) with file addresses.
// See cip_device.h for addressing.

#include "cip_device.h"

namespace {

using namespace logict;
using namespace logict::sdk;
using cip::prop;

class AbDriver final : public Driver {
 public:
  DriverInfo info() const override {
    return {"ab_controllogix", "Allen-Bradley ControlLogix Ethernet", "0.1.0", Medium::Ethernet,
            {"ControlLogix", "CompactLogix", "Micro800", "PLC-5", "SLC 500", "MicroLogix"}};
  }

  nlohmann::json schema() const override {
    using nlohmann::json;
    return {
        {"driver", "ab_controllogix"},
        {"display_name", "Allen-Bradley ControlLogix Ethernet"},
        {"channel", json::array()},
        {"device",
         json::array({
             prop("servermain.DEVICE_MODEL", "enum", "ControlLogix", "ControlLogix, CompactLogix, Micro800, PLC-5, SLC 500, MicroLogix"),
             prop("servermain.DEVICE_ID_STRING", "string", "192.168.1.10,1,0", "IP address, optionally followed by the routing path"),
             prop("ab.DEVICE_ROUTING_PATH", "string", "1,0", "Routing path to the CPU (backplane, slot) for Logix"),
             prop("ab.DEVICE_PORT", "integer", 44818, "EtherNet/IP port"),
         })},
        {"address_help",
         "Logix: MyTag, Program:Main.MyTag, MyArray[3], MyUdt.Member, MyDint.5 (bit), MyArray[0]{10} (10 elements). "
         "PCCC: N7:0, F8:3, B3:0/5, L19:2, ST9:0. Data type Default uses the PLC's type."},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig&, const DeviceConfig& device,
                                                       HostContext& host) override {
    if (device.id.empty()) return make_error(status::BadConfigurationError, "device ID (IP address) is empty");
    return std::make_unique<cip::AbDevice>(device, host, "ab");
  }
};

}  // namespace

LOGICT_DECLARE_DRIVER(AbDriver)
