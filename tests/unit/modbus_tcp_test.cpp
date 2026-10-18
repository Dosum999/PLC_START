#include <gtest/gtest.h>

#include "model/keys.h"
#include "modbus_test_server.h"
#include "runtime/plugin_loader.h"
#include "runtime/runtime.h"

using namespace logict;
using namespace std::chrono_literals;
namespace keys = model::keys;
namespace status = sdk::status;

namespace {

class NullLogger final : public sdk::Logger {
 public:
  void log(sdk::LogLevel, std::string_view) override {}
  bool enabled(sdk::LogLevel) const override { return false; }
};

class ModbusTcpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto d = drivers.load_file(LOGICT_TEST_DRIVER_DIR "/modbus_tcp.so");
    ASSERT_TRUE(d) << d.error().message;
    driver = *d;
  }

  std::unique_ptr<sdk::DeviceRuntime> device(nlohmann::json props = nlohmann::json::object(),
                                             std::chrono::milliseconds timeout = 300ms) {
    sdk::DeviceConfig dc;
    dc.name = "Dev";
    dc.id = "127.0.0.1.1";
    dc.request_timeout = timeout;
    dc.attempts = 2;
    props[std::string("modbus_tcp.DEVICE_PORT")] = server.port();
    dc.properties = props;
    auto rt = driver->create_device({}, dc, host);
    EXPECT_TRUE(rt) << rt.error().message;
    auto dev = std::move(*rt);
    EXPECT_EQ(dev->connect(sdk::SteadyClock::now() + 1s), status::Good);
    return dev;
  }

  std::shared_ptr<const sdk::Address> addr(sdk::DeviceRuntime& d, std::string_view a,
                                           sdk::DataType t = sdk::DataType::Default) {
    auto r = d.parse_address(a, sdk::TagType{t});
    EXPECT_TRUE(r) << a;
    return *r;
  }

  static sdk::ReadItem item_for(const std::shared_ptr<const sdk::Address>& a) {
    sdk::ReadItem it;
    it.address = a.get();
    return it;
  }

  test::ModbusTestServer server;
  core::DriverRegistry drivers;
  sdk::Driver* driver = nullptr;
  NullLogger log;
  sdk::HostContext host{log, nullptr};
};

}  // namespace

TEST_F(ModbusTcpTest, ReadsAndCoalescesIntoBlocks) {
  for (std::uint16_t i = 0; i < 20; ++i) server.set_holding(i, static_cast<std::uint16_t>(100 + i));
  server.set_holding(200, 7);
  server.set_input(4, 55);
  server.set_coil(9, true);
  auto dev = device();
  std::vector<std::shared_ptr<const sdk::Address>> addrs{addr(*dev, "40001"), addr(*dev, "40003"), addr(*dev, "40010"),
                                                          addr(*dev, "40201"), addr(*dev, "30005"), addr(*dev, "000010")};
  std::vector<sdk::ReadItem> items(addrs.size());
  std::vector<sdk::ReadItem*> ptrs;
  for (std::size_t i = 0; i < addrs.size(); ++i) {
    items[i].address = addrs[i].get();
    ptrs.push_back(&items[i]);
  }
  server.reset_counters();
  ASSERT_EQ(dev->read(ptrs), status::Good);
  EXPECT_EQ(*items[0].value.get_if<std::uint16_t>(), 100);
  EXPECT_EQ(*items[1].value.get_if<std::uint16_t>(), 102);
  EXPECT_EQ(*items[2].value.get_if<std::uint16_t>(), 109);
  EXPECT_EQ(*items[3].value.get_if<std::uint16_t>(), 7);
  EXPECT_EQ(*items[4].value.get_if<std::uint16_t>(), 55);
  EXPECT_EQ(*items[5].value.get_if<bool>(), true);
  for (auto& it : items) EXPECT_EQ(it.quality, status::Good);
  // 40001..40010 merge (gaps <= 10); 40201 separate; one input-register and one coil request.
  EXPECT_EQ(server.requests(0x03), 2);
  EXPECT_EQ(server.requests(0x04), 1);
  EXPECT_EQ(server.requests(0x01), 1);
}

TEST_F(ModbusTcpTest, SplitsRequestsAtBlockSize) {
  auto dev = device({{"modbus_tcp.DEVICE_MAX_REGISTERS_PER_REQUEST", 50}});
  auto a = addr(*dev, "40001 [120]");
  sdk::ReadItem item = item_for(a);
  sdk::ReadItem* p = &item;
  server.set_holding(119, 42);
  ASSERT_EQ(dev->read({&p, 1}), status::Good);
  EXPECT_EQ(server.requests(0x03), 3);
  EXPECT_EQ(*item.value.get_if<sdk::Array>()->back().get_if<std::uint16_t>(), 42);
}

TEST_F(ModbusTcpTest, KepwareDefaultWordOrderAndWrites) {
  auto dev = device();
  auto f = addr(*dev, "40011", sdk::DataType::Float);
  ASSERT_EQ(dev->write(*f, sdk::Value(3.25f)), status::Good);
  // 3.25f = 0x40500000, Kepware default first-word-low -> [0x0000, 0x4050]
  EXPECT_EQ(server.holding(10), 0x0000);
  EXPECT_EQ(server.holding(11), 0x4050);
  sdk::ReadItem item = item_for(f);
  sdk::ReadItem* p = &item;
  ASSERT_EQ(dev->read({&p, 1}), status::Good);
  EXPECT_FLOAT_EQ(*item.value.get_if<float>(), 3.25f);

  auto w = addr(*dev, "40001");
  server.reset_counters();
  ASSERT_EQ(dev->write(*w, sdk::Value(std::uint16_t{9})), status::Good);
  EXPECT_EQ(server.requests(0x06), 1);
  auto c = addr(*dev, "000003");
  ASSERT_EQ(dev->write(*c, sdk::Value(true)), status::Good);
  EXPECT_TRUE(server.coil(2));
  EXPECT_EQ(server.requests(0x05), 1);
  auto ca = addr(*dev, "000010 [3]");
  ASSERT_EQ(dev->write(*ca, sdk::Value(sdk::Array{sdk::Value(true), sdk::Value(false), sdk::Value(true)})), status::Good);
  EXPECT_TRUE(server.coil(9));
  EXPECT_FALSE(server.coil(10));
  EXPECT_TRUE(server.coil(11));
  EXPECT_EQ(dev->write(*addr(*dev, "30001"), sdk::Value(1)), status::BadNotWritable);
}

TEST_F(ModbusTcpTest, RegisterBitReadModifyWriteAndMaskWrite) {
  server.set_holding(4, 0x00F0);
  auto dev = device();
  ASSERT_EQ(dev->write(*addr(*dev, "40005.0"), sdk::Value(true)), status::Good);
  ASSERT_EQ(dev->write(*addr(*dev, "40005.4"), sdk::Value(false)), status::Good);
  EXPECT_EQ(server.holding(4), 0x00E1);

  dev.reset();  // the test server (like many devices) serves one connection at a time
  auto masked = device({{"modbus_tcp.DEVICE_HOLDING_REGISTER_MASK_WRITES", true}});
  server.reset_counters();
  ASSERT_EQ(masked->write(*addr(*masked, "40005.15"), sdk::Value(true)), status::Good);
  EXPECT_EQ(server.holding(4), 0x80E1);
  EXPECT_EQ(server.requests(0x16), 1);
  EXPECT_EQ(server.requests(0x03), 0);
}

TEST_F(ModbusTcpTest, IllegalAddressIsolatedFromGoodTags) {
  server.set_holding(0, 1);
  server.set_holding(2, 3);
  server.add_illegal(1);
  auto dev = device();
  auto a0 = addr(*dev, "40001"), a1 = addr(*dev, "40002"), a2 = addr(*dev, "40003");
  std::vector<sdk::ReadItem> items{item_for(a0), item_for(a1), item_for(a2)};
  std::vector<sdk::ReadItem*> ptrs{&items[0], &items[1], &items[2]};
  ASSERT_EQ(dev->read(ptrs), status::Good);
  EXPECT_EQ(items[0].quality, status::Good);
  EXPECT_EQ(items[1].quality, status::BadOutOfRange);
  EXPECT_EQ(items[2].quality, status::Good);
  EXPECT_EQ(*items[2].value.get_if<std::uint16_t>(), 3);

  // Afterwards the bad tag is skipped and the gap is not spanned: two clean
  // requests per scan, no more exceptions.
  for (int scan = 0; scan < 3; ++scan) {
    server.reset_counters();
    for (auto& it : items) it.quality = status::BadWaitingForInitialData;
    ASSERT_EQ(dev->read(ptrs), status::Good);
    EXPECT_EQ(items[0].quality, status::Good);
    EXPECT_EQ(items[1].quality, status::BadOutOfRange);
    EXPECT_EQ(items[2].quality, status::Good);
    EXPECT_EQ(server.requests(0x03), 2);
  }
}

TEST_F(ModbusTcpTest, TimeoutRetriesThenRecovers) {
  auto dev = device({}, 100ms);
  auto a = addr(*dev, "40001");
  sdk::ReadItem item = item_for(a);
  sdk::ReadItem* p = &item;
  server.set_delay(250ms);
  server.reset_counters();
  EXPECT_EQ(dev->read({&p, 1}), status::BadTimeout);
  server.set_delay(0ms);
  std::this_thread::sleep_for(600ms);  // let the late answers arrive
  EXPECT_EQ(server.requests(0x03), 2);  // attempts = 2
  server.set_holding(0, 77);
  EXPECT_EQ(dev->read({&p, 1}), status::Good);  // stale responses are discarded
  EXPECT_EQ(*item.value.get_if<std::uint16_t>(), 77);
}

TEST_F(ModbusTcpTest, DetectsDroppedConnection) {
  auto dev = device();
  auto a = addr(*dev, "40001");
  sdk::ReadItem item = item_for(a);
  sdk::ReadItem* p = &item;
  ASSERT_EQ(dev->read({&p, 1}), status::Good);
  server.drop_client();
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(dev->read({&p, 1}), status::BadNotConnected);
  EXPECT_FALSE(dev->connected());
  EXPECT_EQ(dev->connect(sdk::SteadyClock::now() + 1s), status::Good);
  EXPECT_EQ(dev->read({&p, 1}), status::Good);
}

TEST_F(ModbusTcpTest, DeviceIdFormats) {
  sdk::DeviceConfig dc;
  for (auto id : {"127.0.0.1", "127.0.0.1.255", "<localhost>.3", "<localhost>"}) {
    dc.id = id;
    EXPECT_TRUE(driver->create_device({}, dc, host)) << id;
  }
  for (auto id : {"", "127.0.0.1.256", "<localhost", "<localhost>x"}) {
    dc.id = id;
    EXPECT_FALSE(driver->create_device({}, dc, host)) << id;
  }
}

TEST_F(ModbusTcpTest, EndToEndThroughRuntime) {
  for (std::uint16_t i = 0; i < 4; ++i) server.set_holding(i, static_cast<std::uint16_t>(i * 1000));
  model::json j = {{"channels",
                    {{{keys::kName, "MB"},
                      {keys::kChannelDriver, "Modbus TCP/IP Ethernet"},
                      {"devices",
                       {{{keys::kName, "PLC"},
                         {keys::kDeviceId, "127.0.0.1.1"},
                         {"modbus_tcp.DEVICE_PORT", server.port()},
                         {"tags",
                          {{{keys::kName, "Level"},
                            {keys::kTagAddress, "40002"},
                            {keys::kTagScalingType, 1},
                            {keys::kTagScalingRawHigh, 4000},
                            {keys::kTagScalingScaledHigh, 100}},
                           {{keys::kName, "Speed"}, {keys::kTagAddress, "40004"}}}}}}}}}}};
  auto p = model::project_from_json(j);
  ASSERT_TRUE(p) << p.error().message;
  core::Runtime rt(drivers);
  ASSERT_TRUE(rt.load(*p).empty());
  rt.start();
  auto level = rt.tags().find("MB.PLC.Level");
  auto speed = rt.tags().find("MB.PLC.Speed");
  auto sub = rt.tags().add_subscriber([](auto, auto&, bool) {});
  rt.tags().subscribe(level, sub, 50);
  rt.tags().subscribe(speed, sub, 50);
  auto wait_good = [&](core::TagId id) {
    for (int i = 0; i < 100; ++i) {
      if (sdk::is_good(rt.tags().read(id).quality)) return true;
      std::this_thread::sleep_for(10ms);
    }
    return false;
  };
  ASSERT_TRUE(wait_good(level));
  ASSERT_TRUE(wait_good(speed));
  EXPECT_DOUBLE_EQ(*rt.tags().read(level).value.get_if<double>(), 25.0);  // 1000 / 4000 * 100
  EXPECT_EQ(rt.write(level, sdk::Value(50.0)).get(), status::Good);
  EXPECT_EQ(server.holding(1), 2000);
  rt.stop();
}
