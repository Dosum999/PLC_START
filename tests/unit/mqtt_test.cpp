#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "mqtt/agent_config.h"
#include "mqtt/change_filter.h"
#include "mqtt/template.h"

using namespace logict;
using namespace logict::mqtt;
using sdk::Value;
using sdk::Vqt;
namespace status = sdk::status;

namespace {

sdk::Timestamp at_ms(std::int64_t ms) { return sdk::Timestamp(std::chrono::milliseconds(ms)); }

struct Ev {
  std::string path, channel, device, tag;
  Vqt vqt;
  sdk::TagType type;
};

std::vector<EventView> views(const std::vector<Ev>& evs) {
  std::vector<EventView> out;
  for (const auto& e : evs) out.push_back(EventView{e.path, e.channel, e.device, e.tag, "40001", "desc", true, &e.vqt, &e.type});
  return out;
}

Vqt good(Value v, std::int64_t t = 1000) { return Vqt{std::move(v), status::Good, at_ms(t), at_ms(t)}; }

}  // namespace

TEST(MqttTemplate, StandardDefaultMatchesKepwareShape) {
  auto t = Template::standard(kDefaultStandardTemplate, kDefaultExpansion);
  ASSERT_TRUE(t) << t.error().message;
  std::vector<Ev> evs{{"Ch.Dev.T1", "Ch", "Dev", "T1", good(Value(std::int32_t{250}), 1438011254668), {sdk::DataType::Long}},
                      {"Ch.Dev.S", "Ch", "Dev", "S", good(Value(std::string("a\"b"))), {sdk::DataType::String}}};
  auto out = t->render("host", at_ms(1438011255230), views(evs));
  auto j = nlohmann::json::parse(out);
  EXPECT_EQ(j["timestamp"], 1438011255230);
  ASSERT_EQ(j["values"].size(), 2u);
  EXPECT_EQ(j["values"][0]["id"], "Ch.Dev.T1");
  EXPECT_EQ(j["values"][0]["v"], 250);
  EXPECT_EQ(j["values"][0]["q"], true);
  EXPECT_EQ(j["values"][0]["t"], 1438011254668);
  EXPECT_EQ(j["values"][1]["v"], "a\"b");
}

TEST(MqttTemplate, ValuesAsJsonLiterals) {
  EXPECT_EQ(json_value(Value(true), nullptr), "true");
  EXPECT_EQ(json_value(Value(1.5), nullptr), "1.5");
  EXPECT_EQ(json_value(Value(std::numeric_limits<double>::quiet_NaN()), nullptr), "null");
  EXPECT_EQ(json_value(Value(std::uint8_t{7}), nullptr), "7");
  EXPECT_EQ(json_value(Value(), nullptr), "null");
  sdk::TagType shape{sdk::DataType::Word, 2, 2};
  EXPECT_EQ(json_value(Value(sdk::Array{Value(1), Value(2), Value(3), Value(4)}), &shape), "\"[1,2][3,4]\"");
}

TEST(MqttTemplate, AdvancedEachUnlessAndVariables) {
  auto t = Template::advanced(
      "{|#each VALUES|\"|TAGNAME|\": |VALUE||#unless @last|, |/unless||/each|, \"by\": \"|SERVERNAME|\"}");
  ASSERT_TRUE(t) << t.error().message;
  std::vector<Ev> evs{{"A.B.x", "A", "B", "x", good(Value(1)), {}}, {"A.B.y", "A", "B", "y", good(Value(2)), {}}};
  EXPECT_EQ(t->render("gw1", at_ms(0), views(evs)), R"({"A.B.x": 1, "A.B.y": 2, "by": "gw1"})");
  EXPECT_EQ(t->render("gw1", at_ms(0), {}), R"({, "by": "gw1"})");
}

TEST(MqttTemplate, CsvWithLinefeed) {
  auto t = Template::advanced("|#each VALUES||TAG|,|VALUE|,|QUALITY||LINEFEED||/each|");
  ASSERT_TRUE(t);
  Vqt bad{Value(0), status::BadNotConnected, at_ms(0), at_ms(0)};
  std::vector<Ev> evs{{"C.D.a", "C", "D", "a", good(Value(5)), {}}, {"C.D.b", "C", "D", "b", bad, {}}};
  EXPECT_EQ(t->render("h", at_ms(0), views(evs)), "a,5,true\nb,0,false\n");
}

TEST(MqttTemplate, RejectsInvalidTemplates) {
  EXPECT_FALSE(Template::standard("{ no values }", kDefaultExpansion));
  EXPECT_FALSE(Template::advanced("|TAGNAME|"));                         // event variable outside each
  EXPECT_FALSE(Template::advanced("|#each VALUES| |VALUE|"));            // missing close
  EXPECT_FALSE(Template::advanced("|#unless @last|x|/unless|"));         // unless outside each
  EXPECT_FALSE(Template::advanced("|#each VALUES||#each VALUES||/each||/each|"));
  EXPECT_FALSE(Template::advanced("|BOGUS|"));
  auto e = Template::advanced("line1\nline2 |NOPE|");
  ASSERT_FALSE(e);
  EXPECT_NE(e.error().message.find("line 2"), std::string::npos);
}

TEST(MqttChangeFilter, InitialChangeAndQuality) {
  ChangeFilter f(true, false, false, 0);
  EXPECT_TRUE(f.accept(good(Value(1))));
  EXPECT_FALSE(f.accept(good(Value(1))));
  EXPECT_TRUE(f.accept(good(Value(2))));
  EXPECT_TRUE(f.accept(Vqt{Value(2), status::BadNotConnected, {}, {}}));
  EXPECT_FALSE(f.accept(Vqt{Value(2), status::BadNotConnected, {}, {}}));
  EXPECT_TRUE(f.accept(good(Value(2))));

  ChangeFilter quiet(false, false, true, 0);
  EXPECT_FALSE(quiet.accept(good(Value(1))));                                 // no initial update
  EXPECT_FALSE(quiet.accept(Vqt{Value(1), status::BadNotConnected, {}, {}}));  // quality ignored
  EXPECT_TRUE(quiet.accept(good(Value(3))));
}

TEST(MqttChangeFilter, DeadbandComparesWithLastSent) {
  ChangeFilter f(true, false, false, 1.0);
  EXPECT_TRUE(f.accept(good(Value(10.0))));
  EXPECT_FALSE(f.accept(good(Value(10.6))));
  EXPECT_FALSE(f.accept(good(Value(10.9))));  // still within 1.0 of the last *sent* 10.0
  EXPECT_TRUE(f.accept(good(Value(11.1))));
  EXPECT_FALSE(f.accept(good(Value(10.2))));
  EXPECT_DOUBLE_EQ(ChangeFilter::percent_threshold(sdk::DataType::Word, 10), 6553.5);
  EXPECT_EQ(ChangeFilter::percent_threshold(sdk::DataType::String, 10), 0);
}

TEST(MqttChangeFilter, EveryScanSendsBadOnce) {
  ChangeFilter f(true, true, false, 0);
  EXPECT_TRUE(f.accept(good(Value(1))));
  EXPECT_TRUE(f.accept(good(Value(1))));
  Vqt bad{Value(1), status::BadTimeout, {}, {}};
  EXPECT_TRUE(f.accept(bad));
  EXPECT_FALSE(f.accept(bad));
  EXPECT_TRUE(f.accept(good(Value(1))));
}

TEST(MqttConfig, DefaultsAndValidation) {
  nlohmann::json j = {{keys::kName, "Agent1"},
                      {"iot_items", {{{keys::kItemTag, "Ch.Dev.T"}}, {{keys::kName, "x"}, {keys::kItemTag, "Ch.Dev.U"}}}}};
  auto a = agent_from_json(j);
  ASSERT_TRUE(a) << a.error().message;
  EXPECT_EQ(a->url, "tcp://localhost:1883");
  EXPECT_EQ(a->topic, "iotgateway");
  EXPECT_EQ(a->rate_ms, 10000u);
  EXPECT_EQ(a->items[0].name, "Ch_Dev_T");
  EXPECT_EQ(a->items[0].scan_rate_ms, 1000u);

  auto bad = [&](const char* key, nlohmann::json v) {
    auto c = j;
    c[key] = std::move(v);
    return !agent_from_json(c);
  };
  EXPECT_TRUE(bad(keys::kUrl, "http://x"));
  EXPECT_TRUE(bad(keys::kUrl, "tcp://x:1883/"));
  EXPECT_TRUE(bad(keys::kTopic, "a/+/b"));
  EXPECT_TRUE(bad(keys::kQos, 3));
  EXPECT_TRUE(bad(keys::kProtocol, 3));
  auto adv = j;
  adv[keys::kMessageFormat] = 1;
  adv[keys::kAdvancedTemplate] = "|#each VALUES|";
  EXPECT_FALSE(agent_from_json(adv));
  auto onchange = j;
  onchange[keys::kPublishType] = 1;
  onchange[keys::kFormat] = 1;
  EXPECT_EQ(agent_from_json(onchange)->format, AgentConfig::Format::Narrow);  // on-change is always narrow
}
