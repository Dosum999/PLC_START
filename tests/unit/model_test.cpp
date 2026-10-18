#include <gtest/gtest.h>

#include <fstream>

#include "model/keys.h"
#include "model/project.h"

using namespace logict::model;

TEST(Model, LoadsExampleProject) {
  std::ifstream in(LOGICT_SOURCE_DIR "/config/example-project.json");
  ASSERT_TRUE(in);
  auto p = project_from_json(json::parse(in));
  ASSERT_TRUE(p) << p.error().message;
  ASSERT_EQ(p->channels.size(), 1u);
  const auto& dev = p->channels[0].devices[0];
  EXPECT_EQ(dev.name, "Device1");
  EXPECT_EQ(dev.groups[0].tags[0].name, "State");
  const auto& temp = dev.tags[4];
  EXPECT_EQ(temp.scaling.type, Scaling::Type::Linear);
  EXPECT_EQ(temp.scaling.units, "degC");
  EXPECT_EQ(dev.tags[7].type.scalar, logict::sdk::DataType::Word);
  EXPECT_TRUE(dev.tags[7].type.is_array());
}

TEST(Model, RoundTripPreservesUnknownKeys) {
  json j = {{"project",
             {{"channels",
               {{{keys::kName, "Ch"},
                 {keys::kChannelDriver, "Simulator"},
                 {"vendor.CUSTOM", 42},
                 {"devices", {{{keys::kName, "Dev"}, {"tags", {{{keys::kName, "T"}, {keys::kTagAddress, "R1"}}}}}}}}}}}}};
  auto p = project_from_json(j);
  ASSERT_TRUE(p) << p.error().message;
  json out = project_to_json(*p);
  EXPECT_EQ(out["project"]["channels"][0]["vendor.CUSTOM"], 42);
  EXPECT_EQ(out["project"]["channels"][0]["devices"][0]["tags"][0][keys::kTagAddress], "R1");
  auto again = project_from_json(out);
  ASSERT_TRUE(again);
  EXPECT_EQ(project_to_json(*again), out);
}

TEST(Model, RejectsBadNames) {
  EXPECT_FALSE(is_valid_name(""));
  EXPECT_FALSE(is_valid_name("_System"));
  EXPECT_FALSE(is_valid_name("a.b"));
  EXPECT_TRUE(is_valid_name("Line 1"));
  json dup = {{"channels",
               {{{keys::kName, "A"}, {keys::kChannelDriver, "Simulator"}},
                {{keys::kName, "A"}, {keys::kChannelDriver, "Simulator"}}}}};
  EXPECT_FALSE(project_from_json(dup));
}

TEST(Model, AcceptsDataTypeNames) {
  json t = {{keys::kName, "T"}, {keys::kTagAddress, "R1"}, {keys::kTagDataType, "Float Array"}};
  auto tag = tag_from_json(t);
  ASSERT_TRUE(tag);
  EXPECT_EQ(tag->type.scalar, logict::sdk::DataType::Float);
  EXPECT_TRUE(tag->type.is_array());
}
