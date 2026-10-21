#pragma once

// Address syntax and SQL text for the ODBC Client driver. No ODBC calls here.
//
//   <table>.<column>                  first row of the table (Kepware "fixed table")
//   <table>[<key>=<value>].<column>   row whose key column equals the value
//                                     (Kepware "dynamic table"); value is a number
//                                     or a 'quoted string'
//   sql:<select>                      first column of the first row (read-only)
//   sql.<column>:<select>             named column of the first row (read-only)
//   exec:<statement with one ?>       write-only: runs the statement with the
//                                     written value, e.g. CALL set_speed(?)
//
// <table> may be schema-qualified (plant.tank). Identifiers are quoted with the
// data source's quote character, so they may hold any character except quotes,
// brackets, dots, '=', ';' and white space. Key values are bound as parameters.

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "logict/sdk/driver.h"

namespace logict::odbc {

enum class Kind { Table, Query, Exec };

// A key literal: integer, decimal or string.
using KeyValue = std::variant<std::int64_t, double, std::string>;

struct OdbcAddress final : sdk::Address {
  Kind kind = Kind::Table;
  std::vector<std::string> table;  // schema parts + table name
  std::string key_column;          // empty: first row
  KeyValue key;
  std::string column;              // Table: column; Query: column name (empty: first column)
  std::string sql;                 // Query / Exec statement

  // Rows read together share this: same table and key.
  std::string row_id() const;
};

sdk::Result<std::shared_ptr<OdbcAddress>> parse_address(std::string_view text, sdk::TagType requested);

// Quoted identifier ("a"."b"); quote is the data source's quote character
// (empty or " " when it does not support quoting).
std::string quote(const std::vector<std::string>& parts, std::string_view quote_char);

// SELECT "c1", "c2" FROM "t" [WHERE "k" = ?]
std::string select_sql(const OdbcAddress& row, const std::vector<std::string>& columns, std::string_view quote_char);
// UPDATE "t" SET "c" = ? [WHERE "k" = ?]
std::string update_sql(const OdbcAddress& a, std::string_view quote_char);

// Logical type of a result column, from its ODBC SQL type (SQL_INTEGER, ...).
sdk::DataType natural_type(int sql_type);

// Value of a column fetched as text, for a column of the given SQL type.
sdk::Result<sdk::Value> from_text(std::string_view text, int sql_type);

}  // namespace logict::odbc
