#include <sql.h>
#include <sqlext.h>

#include <gtest/gtest.h>

#include "odbc_sql.h"

using namespace logict;
using odbc::Kind;

namespace {

std::shared_ptr<odbc::OdbcAddress> parse(std::string_view text, sdk::DataType t = sdk::DataType::Default) {
  auto a = odbc::parse_address(text, sdk::TagType{t});
  EXPECT_TRUE(a) << text << ": " << (a ? "" : a.error().message);
  return a ? *a : nullptr;
}

}  // namespace

TEST(OdbcSql, FixedTable) {
  auto a = parse("Tank.Level");
  EXPECT_EQ(a->kind, Kind::Table);
  EXPECT_EQ(a->table, (std::vector<std::string>{"Tank"}));
  EXPECT_EQ(a->column, "Level");
  EXPECT_TRUE(a->key_column.empty());
  EXPECT_EQ(odbc::select_sql(*a, {"Level", "Temp"}, "\""), R"(SELECT "Level", "Temp" FROM "Tank")");
  EXPECT_EQ(odbc::update_sql(*a, "`"), "UPDATE `Tank` SET `Level` = ?");

  auto s = parse("plant.tank.level");
  EXPECT_EQ(s->table, (std::vector<std::string>{"plant", "tank"}));
  EXPECT_EQ(odbc::select_sql(*s, {"level"}, "\""), R"(SELECT "level" FROM "plant"."tank")");
  EXPECT_EQ(odbc::select_sql(*s, {"level"}, " "), "SELECT level FROM plant.tank");  // no quoting

  auto k = parse("설비.온도");  // non-ASCII identifiers
  EXPECT_EQ(k->column, "온도");
}

TEST(OdbcSql, KeyedRow) {
  auto a = parse("plant.tags[name='Line.1 [A]'].value");
  EXPECT_EQ(a->table, (std::vector<std::string>{"plant", "tags"}));
  EXPECT_EQ(a->key_column, "name");
  EXPECT_EQ(std::get<std::string>(a->key), "Line.1 [A]");
  EXPECT_EQ(a->column, "value");
  EXPECT_EQ(odbc::select_sql(*a, {"value"}, "\""), R"(SELECT "value" FROM "plant"."tags" WHERE "name" = ?)");
  EXPECT_EQ(odbc::update_sql(*a, "\""), R"(UPDATE "plant"."tags" SET "value" = ? WHERE "name" = ?)");

  EXPECT_EQ(std::get<std::string>(parse("t[n='it''s'].v")->key), "it's");
  EXPECT_EQ(std::get<std::int64_t>(parse("t[ id = -12 ].v")->key), -12);
  EXPECT_DOUBLE_EQ(std::get<double>(parse("t[x=1.5].v")->key), 1.5);

  // Same row, same id; other key or table, other id.
  EXPECT_EQ(parse("t[id=1].a")->row_id(), parse("t[id=1].b")->row_id());
  EXPECT_NE(parse("t[id=1].a")->row_id(), parse("t[id=2].a")->row_id());
  EXPECT_NE(parse("t[id=1].a")->row_id(), parse("t[id='1'].a")->row_id());
  EXPECT_NE(parse("t.a")->row_id(), parse("u.a")->row_id());
}

TEST(OdbcSql, QueriesAndStatements) {
  auto q = parse("sql: SELECT avg(v) FROM log WHERE note = 'a|b:c'");
  EXPECT_EQ(q->kind, Kind::Query);
  EXPECT_EQ(q->sql, "SELECT avg(v) FROM log WHERE note = 'a|b:c'");
  EXPECT_TRUE(q->column.empty());
  auto n = parse("SQL.total:SELECT count(*) AS total, 1 FROM t");
  EXPECT_EQ(n->column, "total");
  EXPECT_EQ(n->sql, "SELECT count(*) AS total, 1 FROM t");
  EXPECT_EQ(parse("sql.value")->kind, Kind::Table);  // a table named sql

  auto e = parse("exec:CALL set_speed(?, 'why?')");
  EXPECT_EQ(e->kind, Kind::Exec);
  EXPECT_EQ(e->sql, "CALL set_speed(?, 'why?')");
}

TEST(OdbcSql, BadAddresses) {
  for (const char* bad : {"Tank", "Tank.", ".Level", "a.b.c.d.e", "t[id=1]", "t[id=1]v", "t[id=x].v", "t[id='x].v", "t[=1].v",
                          "t[id=1.v", "Ta nk.Level", "T\"x.Level", "t.co;l", "exec:UPDATE t SET v = 1", "exec:CALL p(?, ?)", "sql:"}) {
    EXPECT_FALSE(odbc::parse_address(bad, sdk::TagType{sdk::DataType::Default})) << bad;
  }
  EXPECT_FALSE(odbc::parse_address("t.v", sdk::TagType{sdk::DataType::Long, 4, 0}));  // arrays
}

TEST(OdbcSql, TypesAndText) {
  EXPECT_EQ(odbc::natural_type(SQL_INTEGER), sdk::DataType::Long);
  EXPECT_EQ(odbc::natural_type(SQL_BIGINT), sdk::DataType::LLong);
  EXPECT_EQ(odbc::natural_type(SQL_SMALLINT), sdk::DataType::Short);
  EXPECT_EQ(odbc::natural_type(SQL_REAL), sdk::DataType::Float);
  EXPECT_EQ(odbc::natural_type(SQL_NUMERIC), sdk::DataType::Double);
  EXPECT_EQ(odbc::natural_type(SQL_BIT), sdk::DataType::Boolean);
  EXPECT_EQ(odbc::natural_type(SQL_VARCHAR), sdk::DataType::String);
  EXPECT_EQ(odbc::natural_type(SQL_TYPE_TIMESTAMP), sdk::DataType::Date);

  EXPECT_EQ(*odbc::from_text("-42", SQL_INTEGER), sdk::Value(std::int32_t{-42}));
  EXPECT_EQ(*odbc::from_text("9000000000", SQL_BIGINT), sdk::Value(std::int64_t{9000000000}));
  EXPECT_EQ(*odbc::from_text("12.50", SQL_NUMERIC), sdk::Value(12.5));
  EXPECT_EQ(*odbc::from_text("1.5e3", SQL_DOUBLE), sdk::Value(1500.0));
  EXPECT_EQ(*odbc::from_text("1", SQL_BIT), sdk::Value(true));
  EXPECT_EQ(*odbc::from_text("f", SQL_BIT), sdk::Value(false));
  EXPECT_EQ(*odbc::from_text(" padded ", SQL_CHAR), sdk::Value(std::string(" padded ")));
  EXPECT_FALSE(odbc::from_text("abc", SQL_INTEGER));
  EXPECT_FALSE(odbc::from_text("70000", SQL_SMALLINT));

  using namespace std::chrono;
  auto ts = odbc::from_text("2024-02-29 12:34:56.789", SQL_TYPE_TIMESTAMP);
  ASSERT_TRUE(ts);
  EXPECT_EQ(duration_cast<milliseconds>(ts->get_if<sdk::DateTime>()->time.time_since_epoch()).count(), 1709210096789);
  auto d = odbc::from_text("1999-12-31", SQL_TYPE_DATE);
  ASSERT_TRUE(d);
  EXPECT_EQ(duration_cast<seconds>(d->get_if<sdk::DateTime>()->time.time_since_epoch()).count(), 946598400);
  EXPECT_FALSE(odbc::from_text("2023-02-29 00:00:00", SQL_TYPE_TIMESTAMP));
  EXPECT_FALSE(odbc::from_text("2024-01-01 00:00:00+09", SQL_TYPE_TIMESTAMP));
}
