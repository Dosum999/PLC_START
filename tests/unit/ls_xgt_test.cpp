#include <gtest/gtest.h>

#include "model/keys.h"
#include "runtime/plugin_loader.h"
#include "runtime/runtime.h"
#include "xgt_test_server.h"

using namespace logict;
using namespace std::chrono_literals;
namespace keys = model::keys;
namespace status = sdk::status;
using sdk::DataType;
using sdk::Value;

namespace {

class NullLogger final : public sdk::Logger {
 public:
  void log(sdk::LogLevel, std::string_view) override {}
  bool enabled(sdk::LogLevel) const override { return false; }
};

class XgtTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto d = drivers.load_file(LOGICT_TEST_DRIVER_DIR "/ls_xgt.so");
    ASSERT_TRUE(d) << d.error().message;
    driver = *d;
  }

  std::unique_ptr<sdk::DeviceRuntime> device(std::string model = "XGK", std::chrono::milliseconds timeout = 300ms) {
    sdk::DeviceConfig dc;
    dc.name = "PLC";
    dc.model = model;
    dc.id = "127.0.0.1";
    dc.request_timeout = timeout;
    dc.attempts = 2;
    dc.properties = {{"ls_xgt.DEVICE_PORT", server.port()}};
    auto rt = driver->create_device({}, dc, host);
    EXPECT_TRUE(rt) << rt.error().message;
    auto dev = std::move(*rt);
    EXPECT_EQ(dev->connect(sdk::SteadyClock::now() + 1s), status::Good);
    return dev;
  }

  // Parses addresses, reads them in one transaction and returns the items.
  std::vector<sdk::ReadItem> read(sdk::DeviceRuntime& dev, std::vector<std::pair<std::string, DataType>> tags,
                                  sdk::StatusCode expect = status::Good) {
    addrs.clear();
    for (auto& [a, t] : tags) {
      auto r = dev.parse_address(a, sdk::TagType{t});
      EXPECT_TRUE(r) << a << ": " << (r ? "" : r.error().message);
      addrs.push_back(*r);
    }
    std::vector<sdk::ReadItem> items(addrs.size());
    std::vector<sdk::ReadItem*> ptrs;
    for (std::size_t i = 0; i < items.size(); ++i) {
      items[i].address = addrs[i].get();
      ptrs.push_back(&items[i]);
    }
    EXPECT_EQ(dev.read(ptrs), expect);
    return items;
  }

  sdk::StatusCode write(sdk::DeviceRuntime& dev, std::string a, DataType t, Value v) {
    auto r = dev.parse_address(a, sdk::TagType{t});
    EXPECT_TRUE(r) << a;
    return dev.write(**r, v);
  }

  int count(std::uint16_t cmd, std::uint16_t type) {
    int n = 0;
    for (auto& r : server.requests()) n += r.command == cmd && r.type == type;
    return n;
  }

  test::XgtTestServer server;
  core::DriverRegistry drivers;
  sdk::Driver* driver = nullptr;
  NullLogger log;
  sdk::HostContext host{log, nullptr};
  std::vector<std::shared_ptr<const sdk::Address>> addrs;
};

}  // namespace

TEST_F(XgtTest, CoalescesIntoContinuousReads) {
  server.set_word("D", 0, 11);
  server.set_word("D", 2, 22);
  server.set_word("D", 10, 0xBEEF);
  server.set_bytes("D", 200, {0x44, 0x33, 0x22, 0x11});
  auto dev = device();
  server.reset();
  auto items = read(*dev, {{"D0", DataType::Default}, {"D2", DataType::Short}, {"D10", DataType::Default}, {"D100", DataType::DWord}});
  EXPECT_EQ(*items[0].value.get_if<std::uint16_t>(), 11);
  EXPECT_EQ(*items[1].value.get_if<std::int16_t>(), 22);
  EXPECT_EQ(*items[2].value.get_if<std::uint16_t>(), 0xBEEF);
  EXPECT_EQ(*items[3].value.get_if<std::uint32_t>(), 0x11223344u);
  auto reqs = server.requests();
  ASSERT_EQ(reqs.size(), 2u);  // D0..D10 merged, D100 separate (gap > 32 bytes)
  EXPECT_EQ(reqs[0].type, 0x14);
  EXPECT_EQ(reqs[0].vars[0], "%DB0");
  EXPECT_EQ(reqs[1].vars[0], "%DB200");
  EXPECT_EQ(server.framing_errors(), 0);
  EXPECT_EQ(reqs[0].cpu_info, 0xA0);
}

TEST_F(XgtTest, BitsReadAndWrite) {
  server.set_word("M", 10, 0x0400);  // M0010A
  server.set_word("D", 100, 0x0001);
  auto dev = device();
  auto items = read(*dev, {{"M0010A", DataType::Boolean}, {"M00109", DataType::Boolean}, {"D100.0", DataType::Default}});
  EXPECT_EQ(*items[0].value.get_if<bool>(), true);
  EXPECT_EQ(*items[1].value.get_if<bool>(), false);
  EXPECT_EQ(*items[2].value.get_if<bool>(), true);

  server.reset();
  EXPECT_EQ(write(*dev, "M0010A", DataType::Boolean, Value(false)), status::Good);
  EXPECT_EQ(server.word("M", 10), 0);
  EXPECT_EQ(count(0x58, 0x00), 1);  // direct %MX bit write
  EXPECT_EQ(server.requests().back().vars[0], "%MX170");

  server.reset();
  EXPECT_EQ(write(*dev, "D100.F", DataType::Boolean, Value(true)), status::Good);
  EXPECT_EQ(server.word("D", 100), 0x8001);  // read-modify-write keeps bit 0
  EXPECT_EQ(count(0x54, 0x14), 1);
  EXPECT_EQ(count(0x58, 0x14), 1);
}

TEST_F(XgtTest, TimerContactsAndCurrentValues) {
  server.set_contact("%TX5", true);
  server.set_word("T", 5, 300);
  auto dev = device();
  auto items = read(*dev, {{"T5", DataType::Boolean}, {"T5", DataType::Default}});
  EXPECT_EQ(*items[0].value.get_if<bool>(), true);
  EXPECT_EQ(*items[1].value.get_if<std::uint16_t>(), 300);
  EXPECT_EQ(write(*dev, "T5", DataType::Boolean, Value(false)), status::Good);
  EXPECT_FALSE(server.contact("%TX5"));
}

TEST_F(XgtTest, IndividualAreasUseWordRequests) {
  server.set_bytes("ZR", 200, {0x78, 0x56, 0x34, 0x12});  // ZR100..101
  server.set_word("R", 7, 77);
  auto dev = device();
  server.reset();
  auto items = read(*dev, {{"ZR100", DataType::DWord}, {"R7", DataType::Default}});
  EXPECT_EQ(*items[0].value.get_if<std::uint32_t>(), 0x12345678u);
  EXPECT_EQ(*items[1].value.get_if<std::uint16_t>(), 77);
  auto reqs = server.requests();
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(reqs[0].type, 0x02);
  EXPECT_EQ(reqs[0].vars, (std::vector<std::string>{"%ZRW100", "%ZRW101", "%RW7"}));

  EXPECT_EQ(write(*dev, "ZR100", DataType::Float, Value(2.5)), status::Good);
  auto b = server.bytes("ZR", 200, 4);
  float f;
  std::memcpy(&f, b.data(), 4);
  EXPECT_FLOAT_EQ(f, 2.5f);
}

TEST_F(XgtTest, XgiModelDirectVariables) {
  server.set_var("%IX0.0.3", {1});
  server.set_var("%QW0.1.0", {0, 0});
  auto dev = device("XGI-CPUU");
  EXPECT_EQ(write(*dev, "%MD10", DataType::Float, Value(-12.25)), status::Good);
  EXPECT_EQ(write(*dev, "%QW0.1.0", DataType::Default, Value(0x55AA)), status::Good);
  EXPECT_EQ(write(*dev, "%MW100.3", DataType::Default, Value(true)), status::Good);
  server.reset();
  auto items = read(*dev, {{"%MD10", DataType::Float}, {"%IX0.0.3", DataType::Default}, {"%QW0.1.0", DataType::Default},
                           {"%MX1603", DataType::Default}});
  EXPECT_FLOAT_EQ(*items[0].value.get_if<float>(), -12.25f);
  EXPECT_EQ(*items[1].value.get_if<bool>(), true);
  EXPECT_EQ(*items[2].value.get_if<std::uint16_t>(), 0x55AA);
  EXPECT_EQ(*items[3].value.get_if<bool>(), true);
  for (auto& r : server.requests()) EXPECT_EQ(r.cpu_info, 0xA4);
}

TEST_F(XgtTest, NakIsolatesBadTagsFromMergedBlocks) {
  server.set_word("D", 0, 1);
  server.set_word("D", 10, 3);
  server.add_illegal("D", 10);  // D5
  auto dev = device();
  auto items = read(*dev, {{"D0", DataType::Default}, {"D5", DataType::Default}, {"D10", DataType::Default}});
  EXPECT_EQ(items[0].quality, status::Good);
  EXPECT_EQ(items[1].quality, status::BadOutOfRange);
  EXPECT_EQ(items[2].quality, status::Good);
  EXPECT_EQ(*items[2].value.get_if<std::uint16_t>(), 3);

  // Afterwards: no NAKs, the bad tag is skipped.
  std::vector<sdk::ReadItem*> ptrs{&items[0], &items[1], &items[2]};
  server.reset();
  for (auto& it : items) it.quality = status::BadWaitingForInitialData;
  EXPECT_EQ(dev->read(ptrs), status::Good);
  EXPECT_EQ(items[0].quality, status::Good);
  EXPECT_EQ(items[1].quality, status::BadOutOfRange);
  EXPECT_EQ(items[2].quality, status::Good);
  EXPECT_EQ(server.requests().size(), 2u);
}

TEST_F(XgtTest, TimeoutDiscardsStaleResponses) {
  auto dev = device("XGK", 100ms);
  server.set_delay(250ms);
  read(*dev, {{"D0", DataType::Default}}, status::BadTimeout);
  server.set_delay(0ms);
  std::this_thread::sleep_for(600ms);
  server.set_word("D", 0, 42);
  auto items = read(*dev, {{"D0", DataType::Default}});
  EXPECT_EQ(*items[0].value.get_if<std::uint16_t>(), 42);
}

TEST_F(XgtTest, EndToEndThroughRuntime) {
  server.set_word("D", 100, 2000);
  model::json j = {{"channels",
                    {{{keys::kName, "LS"},
                      {keys::kChannelDriver, "LS XGT FEnet"},
                      {"devices",
                       {{{keys::kName, "XGK1"},
                         {keys::kDeviceModel, "XGK-CPUH"},
                         {keys::kDeviceId, "127.0.0.1"},
                         {"ls_xgt.DEVICE_PORT", server.port()},
                         {"tags",
                          {{{keys::kName, "Pressure"},
                            {keys::kTagAddress, "D100"},
                            {keys::kTagScalingType, 1},
                            {keys::kTagScalingRawHigh, 4000},
                            {keys::kTagScalingScaledHigh, 10}},
                           {{keys::kName, "Run"}, {keys::kTagAddress, "M0000F"}, {keys::kTagDataType, 1}}}}}}}}}}};
  auto p = model::project_from_json(j);
  ASSERT_TRUE(p) << p.error().message;
  core::Runtime rt(drivers);
  ASSERT_TRUE(rt.load(*p).empty());
  rt.start();
  auto pressure = rt.tags().find("LS.XGK1.Pressure");
  auto run = rt.tags().find("LS.XGK1.Run");
  auto sub = rt.tags().add_subscriber([](auto, auto&, bool) {});
  rt.tags().subscribe(pressure, sub, 50);
  rt.tags().subscribe(run, sub, 50);
  for (int i = 0; i < 100 && !sdk::is_good(rt.tags().read(pressure).quality); ++i) std::this_thread::sleep_for(10ms);
  EXPECT_DOUBLE_EQ(*rt.tags().read(pressure).value.get_if<double>(), 5.0);
  EXPECT_EQ(rt.write(run, Value(true)).get(), status::Good);
  EXPECT_EQ(server.word("M", 0), 0x8000);
  EXPECT_EQ(rt.write(pressure, Value(2.5)).get(), status::Good);
  EXPECT_EQ(server.word("D", 100), 1000);
  rt.stop();
}
