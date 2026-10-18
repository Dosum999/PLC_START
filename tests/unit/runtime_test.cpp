#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>

#include "model/keys.h"
#include "runtime/runtime.h"

using namespace logict;
using namespace std::chrono_literals;
namespace keys = model::keys;

namespace {

class RuntimeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto r = drivers.load_file(LOGICT_TEST_DRIVER_DIR "/simulator.so");
    ASSERT_TRUE(r) << r.error().message;
  }

  model::Project make_project(model::json device_extra = model::json::object()) {
    model::json dev = {
        {keys::kName, "Dev"},
        {keys::kDeviceId, "1"},
        {"tags",
         {
             {{keys::kName, "Reg"}, {keys::kTagAddress, "R0001"}},
             {{keys::kName, "Ramp"}, {keys::kTagAddress, "RAMP(50, 0, 1000, 1)"}, {keys::kTagAccess, 0}},
             {{keys::kName, "Scaled"},
              {keys::kTagAddress, "R0002"},
              {keys::kTagScalingType, 1},
              {keys::kTagScalingRawHigh, 1000},
              {keys::kTagScalingScaledHigh, 10}},
             {{keys::kName, "Bad"}, {keys::kTagAddress, "X99"}},
         }}};
    dev.update(device_extra);
    model::json j = {{"channels", {{{keys::kName, "Ch"}, {keys::kChannelDriver, "Simulator"}, {"devices", {dev}}}}}};
    auto p = model::project_from_json(j);
    EXPECT_TRUE(p) << p.error().message;
    return *p;
  }

  // Waits until `pred(vqt)` holds for the tag.
  template <typename Pred>
  bool wait_for(core::Runtime& rt, core::TagId id, Pred pred, std::chrono::milliseconds timeout = 2s) {
    auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
      if (pred(rt.tags().read(id))) return true;
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }

  core::DriverRegistry drivers;
};

}  // namespace

TEST_F(RuntimeTest, PollsOnlySubscribedTags) {
  core::Runtime rt(drivers);
  EXPECT_TRUE(rt.load(make_project()).empty());
  rt.start();
  auto ramp = rt.tags().find("Ch.Dev.Ramp");
  ASSERT_NE(ramp, core::kInvalidTag);

  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(rt.tags().read(ramp).quality, sdk::status::BadWaitingForInitialData);

  auto sub = rt.tags().add_subscriber([](auto, auto&, bool) {});
  rt.tags().subscribe(ramp, sub, 50);
  EXPECT_TRUE(wait_for(rt, ramp, [](const sdk::Vqt& v) { return sdk::is_good(v.quality); }));
  auto first = rt.tags().read(ramp).value;
  EXPECT_TRUE(wait_for(rt, ramp, [&](const sdk::Vqt& v) { return !(v.value == first); }));
  EXPECT_EQ(rt.tags().active_tag_count(), 1u);
  rt.stop();
}

TEST_F(RuntimeTest, WriteThenReadBackWithScaling) {
  core::Runtime rt(drivers);
  rt.load(make_project());
  rt.start();
  auto scaled = rt.tags().find("Ch.Dev.Scaled");
  auto sub = rt.tags().add_subscriber([](auto, auto&, bool) {});
  rt.tags().subscribe(scaled, sub, 50);

  auto st = rt.write(scaled, sdk::Value(2.5)).get();  // raw = 250
  EXPECT_EQ(st, sdk::status::Good);
  EXPECT_TRUE(wait_for(rt, scaled, [](const sdk::Vqt& v) {
    auto d = sdk::to_double(v.value);
    return d && *d == 2.5;
  }));
  rt.stop();
}

TEST_F(RuntimeTest, RejectsWritesToReadOnlyAndSystemTags) {
  core::Runtime rt(drivers);
  rt.load(make_project());
  rt.start();
  EXPECT_EQ(rt.write(rt.tags().find("Ch.Dev.Ramp"), sdk::Value(1)).get(), sdk::status::BadNotWritable);
  EXPECT_EQ(rt.write(rt.tags().find("Ch.Dev._System._Error"), sdk::Value(true)).get(), sdk::status::BadNotWritable);
  EXPECT_EQ(rt.write(rt.tags().find("Ch.Dev.Reg"), sdk::Value(-5)).get(), sdk::status::BadOutOfRange);
  rt.stop();
}

TEST_F(RuntimeTest, InvalidAddressGetsConfigurationError) {
  core::Runtime rt(drivers);
  rt.load(make_project());
  auto bad = rt.tags().find("Ch.Dev.Bad");
  ASSERT_NE(bad, core::kInvalidTag);
  EXPECT_EQ(rt.tags().read(bad).quality, sdk::status::BadConfigurationError);
}

TEST_F(RuntimeTest, OfflineDeviceSetsErrorAndDemotes) {
  core::Runtime rt(drivers);
  rt.load(make_project({{keys::kDeviceId, "offline"},
                        {keys::kDeviceDemotionEnable, true},
                        {keys::kDeviceDemoteAfter, 2},
                        {keys::kDeviceDemotionPeriod, 500}}));
  rt.start();
  auto reg = rt.tags().find("Ch.Dev.Reg");
  auto err = rt.tags().find("Ch.Dev._System._Error");
  auto demoted = rt.tags().find("Ch.Dev._System._AutoDemoted");
  auto sub = rt.tags().add_subscriber([](auto, auto&, bool) {});
  rt.tags().subscribe(reg, sub, 20);

  EXPECT_TRUE(wait_for(rt, reg, [](const sdk::Vqt& v) { return v.quality == sdk::status::BadNotConnected; }));
  EXPECT_TRUE(wait_for(rt, err, [](const sdk::Vqt& v) { return v.value == sdk::Value(true); }));
  EXPECT_TRUE(wait_for(rt, demoted, [](const sdk::Vqt& v) { return v.value == sdk::Value(true); }));
  rt.stop();
}

TEST_F(RuntimeTest, SubscriberReceivesChanges) {
  core::Runtime rt(drivers);
  rt.load(make_project());
  rt.start();
  std::mutex mu;
  std::condition_variable cv;
  int changes = 0;
  auto sub = rt.tags().add_subscriber([&](core::TagId, const sdk::Vqt&, bool changed) {
    std::lock_guard lock(mu);
    if (changed) ++changes;
    cv.notify_all();
  });
  rt.tags().subscribe(rt.tags().find("Ch.Dev.Ramp"), sub, 50);
  std::unique_lock lock(mu);
  EXPECT_TRUE(cv.wait_for(lock, 2s, [&] { return changes >= 3; }));
  lock.unlock();
  rt.tags().remove_subscriber(sub);
  rt.stop();
}
