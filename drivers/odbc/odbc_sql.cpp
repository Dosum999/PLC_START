#include "odbc_sql.h"

#include <sql.h>
#include <sqlext.h>

#include <cctype>
#include <charconv>
#include <chrono>
#include <format>

namespace logict::odbc {

namespace status = sdk::status;

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

bool iequals_prefix(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(s[i])) != prefix[i]) return false;
  return true;
}

bool valid_identifier(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (std::isspace(static_cast<unsigned char>(c)) || std::string_view("\"'`[].=;").find(c) != std::string_view::npos)
      return false;
  return true;
}

sdk::Result<std::vector<std::string>> split_table(std::string_view s) {
  std::vector<std::string> parts;
  for (std::size_t start = 0;;) {
    auto dot = s.find('.', start);
    std::string_view part = s.substr(start, dot - start);
    if (!valid_identifier(part)) return sdk::make_error(status::BadInvalidArgument, "bad table name");
    parts.emplace_back(part);
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  if (parts.size() > 3) return sdk::make_error(status::BadInvalidArgument, "table name has too many parts");
  return parts;
}

sdk::Result<KeyValue> parse_key(std::string_view v) {
  if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') {
    std::string out;
    for (std::size_t i = 1; i + 1 < v.size(); ++i) {
      if (v[i] == '\'') {
        if (i + 2 >= v.size() || v[i + 1] != '\'') return sdk::make_error(status::BadInvalidArgument, "bad quote in key value");
        ++i;
      }
      out += v[i];
    }
    return KeyValue(std::move(out));
  }
  const char* b = v.data();
  const char* e = v.data() + v.size();
  std::int64_t i = 0;
  if (auto r = std::from_chars(b, e, i); r.ec == std::errc{} && r.ptr == e && !v.empty()) return KeyValue(i);
  double d = 0;
  if (auto r = std::from_chars(b, e, d); r.ec == std::errc{} && r.ptr == e && !v.empty()) return KeyValue(d);
  return sdk::make_error(status::BadInvalidArgument, "key value must be a number or a 'quoted string'");
}

}  // namespace

std::string OdbcAddress::row_id() const {
  std::string id;
  for (const auto& p : table) id += p + '\x1f';
  id += key_column + '\x1f';
  std::visit([&](const auto& k) {
    if constexpr (std::is_same_v<std::decay_t<decltype(k)>, std::string>) id += "s" + k;
    else id += std::format("n{}", k);
  }, key);
  return id;
}

sdk::Result<std::shared_ptr<OdbcAddress>> parse_address(std::string_view text, sdk::TagType requested) {
  auto a = std::make_shared<OdbcAddress>();
  a->text = std::string(text);
  a->type = requested;
  if (requested.is_array()) return sdk::make_error(status::BadTypeMismatch, "array tags are not supported");
  std::string_view s = trim(text);

  if (iequals_prefix(s, "exec:")) {
    a->kind = Kind::Exec;
    a->sql = std::string(trim(s.substr(5)));
    std::size_t marks = 0;
    bool in_string = false;
    for (char c : a->sql) {
      if (c == '\'') in_string = !in_string;
      else if (c == '?' && !in_string) ++marks;
    }
    if (marks != 1) return sdk::make_error(status::BadInvalidArgument, "exec: statement needs exactly one ? for the value");
    return a;
  }
  // "sql.<column>:" only with a colon, so a table named sql stays reachable.
  if (iequals_prefix(s, "sql:") || (iequals_prefix(s, "sql.") && s.find(':') != std::string_view::npos)) {
    a->kind = Kind::Query;
    s.remove_prefix(3);
    if (s.front() == '.') {
      auto colon = s.find(':');
      a->column = std::string(trim(s.substr(1, colon - 1)));
      if (!valid_identifier(a->column)) return sdk::make_error(status::BadInvalidArgument, "bad column name");
      s.remove_prefix(colon);
    }
    a->sql = std::string(trim(s.substr(1)));
    if (a->sql.empty()) return sdk::make_error(status::BadInvalidArgument, "empty query");
    return a;
  }

  // <table>[<key>=<value>].<column>: the key selector may hold dots and brackets
  // inside a quoted string, so find it first.
  std::string_view table_part, column_part;
  if (auto open = s.find('['); open != std::string_view::npos) {
    std::size_t close = std::string_view::npos;
    bool in_string = false;
    for (std::size_t i = open + 1; i < s.size(); ++i) {
      if (s[i] == '\'') in_string = !in_string;
      else if (s[i] == ']' && !in_string) {
        close = i;
        break;
      }
    }
    if (close == std::string_view::npos) return sdk::make_error(status::BadInvalidArgument, "missing ']'");
    std::string_view sel = s.substr(open + 1, close - open - 1);
    auto eq = sel.find('=');
    if (eq == std::string_view::npos) return sdk::make_error(status::BadInvalidArgument, "expected [<key column>=<value>]");
    a->key_column = std::string(trim(sel.substr(0, eq)));
    if (!valid_identifier(a->key_column)) return sdk::make_error(status::BadInvalidArgument, "bad key column name");
    auto key = parse_key(trim(sel.substr(eq + 1)));
    if (!key) return std::unexpected(key.error());
    a->key = std::move(*key);
    table_part = trim(s.substr(0, open));
    std::string_view rest = trim(s.substr(close + 1));
    if (rest.empty() || rest.front() != '.') return sdk::make_error(status::BadInvalidArgument, "expected .<column> after the key");
    column_part = trim(rest.substr(1));
  } else {
    auto dot = s.rfind('.');
    if (dot == std::string_view::npos) return sdk::make_error(status::BadInvalidArgument, "expected <table>.<column>");
    table_part = s.substr(0, dot);
    column_part = s.substr(dot + 1);
  }
  auto table = split_table(table_part);
  if (!table) return std::unexpected(table.error());
  a->table = std::move(*table);
  if (!valid_identifier(column_part)) return sdk::make_error(status::BadInvalidArgument, "bad column name");
  a->column = std::string(column_part);
  return a;
}

std::string quote(const std::vector<std::string>& parts, std::string_view quote_char) {
  const std::string_view q = quote_char == " " ? std::string_view{} : quote_char;
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) out += '.';
    out += q;
    out += parts[i];
    out += q;
  }
  return out;
}

std::string select_sql(const OdbcAddress& row, const std::vector<std::string>& columns, std::string_view quote_char) {
  std::string sql = "SELECT ";
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (i) sql += ", ";
    sql += quote({columns[i]}, quote_char);
  }
  sql += " FROM " + quote(row.table, quote_char);
  if (!row.key_column.empty()) sql += " WHERE " + quote({row.key_column}, quote_char) + " = ?";
  return sql;
}

std::string update_sql(const OdbcAddress& a, std::string_view quote_char) {
  std::string sql = "UPDATE " + quote(a.table, quote_char) + " SET " + quote({a.column}, quote_char) + " = ?";
  if (!a.key_column.empty()) sql += " WHERE " + quote({a.key_column}, quote_char) + " = ?";
  return sql;
}

sdk::DataType natural_type(int sql_type) {
  switch (sql_type) {
    case SQL_BIT: return sdk::DataType::Boolean;
    case SQL_TINYINT:
    case SQL_SMALLINT: return sdk::DataType::Short;
    case SQL_INTEGER: return sdk::DataType::Long;
    case SQL_BIGINT: return sdk::DataType::LLong;
    case SQL_REAL: return sdk::DataType::Float;
    case SQL_FLOAT:
    case SQL_DOUBLE:
    case SQL_DECIMAL:
    case SQL_NUMERIC: return sdk::DataType::Double;
    case SQL_TYPE_DATE:
    case SQL_TYPE_TIMESTAMP:
    case SQL_DATETIME: return sdk::DataType::Date;
    default: return sdk::DataType::String;
  }
}

namespace {

// "YYYY-MM-DD[ HH:MM:SS[.fffffffff]]", taken as UTC.
sdk::Result<sdk::Value> parse_timestamp(std::string_view s) {
  auto num = [&](std::size_t pos, std::size_t len, int& out) {
    if (pos + len > s.size()) return false;
    auto r = std::from_chars(s.data() + pos, s.data() + pos + len, out);
    return r.ec == std::errc{} && r.ptr == s.data() + pos + len;
  };
  int y, mo, d, h = 0, mi = 0, sec = 0;
  if (!num(0, 4, y) || s.size() < 10 || s[4] != '-' || !num(5, 2, mo) || s[7] != '-' || !num(8, 2, d))
    return sdk::make_error(status::BadTypeMismatch, "bad timestamp");
  std::chrono::nanoseconds frac{0};
  if (s.size() > 10) {
    if ((s[10] != ' ' && s[10] != 'T') || !num(11, 2, h) || s.size() < 19 || s[13] != ':' || !num(14, 2, mi) || s[16] != ':' ||
        !num(17, 2, sec))
      return sdk::make_error(status::BadTypeMismatch, "bad timestamp");
    if (s.size() > 19) {
      if (s[19] != '.') return sdk::make_error(status::BadTypeMismatch, "time zones are not supported");
      std::int64_t ns = 0;
      std::size_t digits = 0;
      for (std::size_t i = 20; i < s.size(); ++i, ++digits) {
        if (!std::isdigit(static_cast<unsigned char>(s[i])) || digits >= 9) return sdk::make_error(status::BadTypeMismatch, "bad timestamp");
        ns = ns * 10 + (s[i] - '0');
      }
      for (; digits < 9; ++digits) ns *= 10;
      frac = std::chrono::nanoseconds(ns);
    }
  }
  const std::chrono::year_month_day ymd{std::chrono::year(y), std::chrono::month(static_cast<unsigned>(mo)),
                                        std::chrono::day(static_cast<unsigned>(d))};
  if (!ymd.ok() || h > 23 || mi > 59 || sec > 60) return sdk::make_error(status::BadTypeMismatch, "bad timestamp");
  const auto t = std::chrono::sys_days(ymd) + std::chrono::hours(h) + std::chrono::minutes(mi) + std::chrono::seconds(sec) + frac;
  return sdk::Value(sdk::DateTime{std::chrono::time_point_cast<sdk::Timestamp::duration>(t)});
}

}  // namespace

sdk::Result<sdk::Value> from_text(std::string_view text, int sql_type) {
  switch (natural_type(sql_type)) {
    case sdk::DataType::Boolean: {
      std::string_view t = trim(text);
      if (t == "1" || t == "t" || t == "T" || t == "true" || t == "TRUE") return sdk::Value(true);
      if (t == "0" || t == "f" || t == "F" || t == "false" || t == "FALSE") return sdk::Value(false);
      return sdk::make_error(status::BadTypeMismatch, "bad boolean");
    }
    case sdk::DataType::Date: return parse_timestamp(trim(text));
    case sdk::DataType::String: return sdk::Value(std::string(text));
    default: {
      auto v = sdk::convert(sdk::Value(std::string(trim(text))), natural_type(sql_type));
      if (!v) return std::unexpected(v.error());
      return v;
    }
  }
}

}  // namespace logict::odbc
