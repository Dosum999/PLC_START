#include "sysmon.h"

#include <gtest/gtest.h>

using namespace logict::sysmon;

TEST(Sysmon, CpuLine) {
  // user nice system idle iowait irq softirq steal guest guest_nice
  auto t = parse_cpu("cpu  100 5 50 800 20 3 2 0 40 0\ncpu0 1 2 3 4\n");
  ASSERT_TRUE(t);
  EXPECT_EQ(t->total, 980u);  // guest is already part of user
  EXPECT_EQ(t->busy, 160u);   // everything but idle and iowait
  EXPECT_FALSE(parse_cpu("cpu0 1 2 3 4\n"));
  EXPECT_FALSE(parse_cpu("cpu  1 2\n"));
  EXPECT_DOUBLE_EQ(cpu_usage({100, 1000}, {150, 1100}), 50.0);
  EXPECT_DOUBLE_EQ(cpu_usage({100, 1000}, {100, 1000}), 0.0);
}

TEST(Sysmon, Meminfo) {
  auto m = parse_meminfo("MemTotal:       16318496 kB\nMemFree:  1000 kB\nHugePages_Total:       0\nbroken line\n");
  EXPECT_EQ(m.at("MemTotal"), 16318496ull * 1024);
  EXPECT_EQ(m.at("MemFree"), 1024000u);
  EXPECT_EQ(m.at("HugePages_Total"), 0u);  // counts have no unit
  EXPECT_EQ(m.size(), 3u);
}

TEST(Sysmon, LoadAvg) {
  auto l = parse_loadavg("0.52 1.25 2.00 3/812 12345\n");
  ASSERT_TRUE(l);
  EXPECT_DOUBLE_EQ(l->one, 0.52);
  EXPECT_DOUBLE_EQ(l->five, 1.25);
  EXPECT_DOUBLE_EQ(l->fifteen, 2.0);
  EXPECT_FALSE(parse_loadavg("garbage"));
}
