// ODBC Client driver (Kepware "ODBC Client"): database tables and queries as
// tags through unixODBC, so any data source with an ODBC driver works
// (PostgreSQL, MySQL/MariaDB, SQL Server, SQLite, ...).
//
// The device ID is an ODBC connection string ("DSN=plant" or
// "Driver=PostgreSQL Unicode;Server=db;Database=plant"); user and password may
// also be set as properties. Address syntax is in odbc_sql.h.
//
// Reads run one SELECT per table row (all columns of the tags on that row);
// if it fails, each column is read on its own so one bad column does not take
// the others down. A NULL reads as BadNoData. Table writes are an UPDATE in a
// transaction that is committed only if exactly one row changed.

#include <sql.h>
#include <sqlext.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <unordered_map>

#include "logict/sdk/driver.h"
#include "odbc_sql.h"

namespace {

using namespace logict;
using odbc::Kind;
using odbc::OdbcAddress;
namespace status = sdk::status;

constexpr const char* kId = "odbc_client";

bool ok(SQLRETURN r) { return r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO; }

struct Diag {
  std::string state;  // SQLSTATE of the first record
  std::string message;
};

Diag diagnose(SQLSMALLINT type, SQLHANDLE h) {
  Diag d;
  SQLCHAR state[6] = {};
  SQLINTEGER native = 0;
  SQLCHAR msg[1024] = {};
  SQLSMALLINT len = 0;
  if (ok(SQLGetDiagRec(type, h, 1, state, &native, msg, sizeof msg, &len))) {
    d.state = reinterpret_cast<const char*>(state);
    d.message = reinterpret_cast<const char*>(msg);
  }
  return d;
}

class Statement {
 public:
  explicit Statement(SQLHDBC dbc) { SQLAllocHandle(SQL_HANDLE_STMT, dbc, &h_); }
  ~Statement() {
    if (h_) SQLFreeHandle(SQL_HANDLE_STMT, h_);
  }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  SQLHSTMT get() const { return h_; }
  Diag diag() const { return diagnose(SQL_HANDLE_STMT, h_); }

 private:
  SQLHSTMT h_ = nullptr;
};

// A bound parameter; kept alive until the statement has run.
struct Param {
  SQLSMALLINT ctype = SQL_C_CHAR, sqltype = SQL_VARCHAR;
  SQLULEN size = 0;
  SQLSMALLINT digits = 0;
  std::int64_t i = 0;
  std::uint64_t u = 0;
  double d = 0;
  float f = 0;
  unsigned char bit = 0;
  std::string s;
  SQLLEN ind = 0;

  void* data() {
    switch (ctype) {
      case SQL_C_SBIGINT: return &i;
      case SQL_C_UBIGINT: return &u;
      case SQL_C_DOUBLE: return &d;
      case SQL_C_FLOAT: return &f;
      case SQL_C_BIT: return &bit;
      default: return s.data();
    }
  }
};

std::string format_timestamp(const sdk::DateTime& t) {
  return std::format("{:%Y-%m-%d %H:%M:%S}", std::chrono::time_point_cast<std::chrono::milliseconds>(t.time));
}

Param make_param(const sdk::Value& v) {
  Param p;
  std::visit(
      [&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, bool>) {
          p.ctype = SQL_C_BIT, p.sqltype = SQL_BIT, p.bit = x ? 1 : 0;
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          p.ctype = SQL_C_UBIGINT, p.sqltype = SQL_BIGINT, p.u = x;
        } else if constexpr (std::is_integral_v<T> && sizeof(T) <= 4 && !std::is_same_v<T, std::uint32_t>) {
          p.ctype = SQL_C_SBIGINT, p.sqltype = SQL_INTEGER, p.i = x;  // INTEGER: matches int parameters
        } else if constexpr (std::is_integral_v<T>) {
          p.ctype = SQL_C_SBIGINT, p.sqltype = SQL_BIGINT, p.i = static_cast<std::int64_t>(x);
        } else if constexpr (std::is_same_v<T, float>) {
          p.ctype = SQL_C_FLOAT, p.sqltype = SQL_REAL, p.f = x;
        } else if constexpr (std::is_same_v<T, double>) {
          p.ctype = SQL_C_DOUBLE, p.sqltype = SQL_DOUBLE, p.d = x;
        } else if constexpr (std::is_same_v<T, sdk::DateTime>) {
          p.s = format_timestamp(x);
          p.sqltype = SQL_TYPE_TIMESTAMP, p.size = 23, p.digits = 3;
          p.ind = static_cast<SQLLEN>(p.s.size());
        } else if constexpr (std::is_same_v<T, std::string>) {
          p.s = x;
          p.size = std::max<SQLULEN>(1, p.s.size());
          p.ind = static_cast<SQLLEN>(p.s.size());
        }
      },
      v.data);
  return p;
}

Param key_param(const odbc::KeyValue& k) {
  return std::visit([](const auto& x) { return make_param(sdk::Value(x)); }, k);
}

// Connection string attribute value, braced only when needed: some drivers
// (psqlODBC) keep the braces of a plain value.
std::string attribute_value(const std::string& v) {
  if (v.find_first_of(";{}=") == std::string::npos && v.front() != ' ' && v.back() != ' ') return v;
  std::string out = "{";
  for (char c : v) {
    out += c;
    if (c == '}') out += '}';
  }
  return out + "}";
}

struct Settings {
  std::string connection;
  std::string username, password;
  int login_timeout_s = 3;
  int query_timeout_s = 1;
};

class OdbcDevice final : public sdk::DeviceRuntime {
 public:
  OdbcDevice(Settings s, sdk::HostContext& host) : s_(std::move(s)), host_(host) {}
  ~OdbcDevice() override { disconnect(); }

  sdk::Result<std::shared_ptr<const sdk::Address>> parse_address(std::string_view text, sdk::TagType requested) override {
    auto a = odbc::parse_address(text, requested);
    if (!a) return std::unexpected(a.error());
    return std::shared_ptr<const sdk::Address>(std::move(*a));
  }

  sdk::StatusCode connect(sdk::Deadline deadline) override {
    disconnect();
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_);
    SQLSetEnvAttr(env_, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
    SQLAllocHandle(SQL_HANDLE_DBC, env_, &dbc_);
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(deadline - sdk::SteadyClock::now()).count();
    const auto login = static_cast<SQLULEN>(std::clamp<long long>(left, 1, std::max(1, s_.login_timeout_s)));
    SQLSetConnectAttr(dbc_, SQL_ATTR_LOGIN_TIMEOUT, reinterpret_cast<SQLPOINTER>(login), 0);
    std::string conn = s_.connection;
    if (!s_.username.empty()) conn += ";UID=" + attribute_value(s_.username);
    if (!s_.password.empty()) conn += ";PWD=" + attribute_value(s_.password);
    SQLCHAR out[1024];
    SQLSMALLINT out_len = 0;
    const SQLRETURN r = SQLDriverConnect(dbc_, nullptr, reinterpret_cast<SQLCHAR*>(conn.data()), static_cast<SQLSMALLINT>(conn.size()),
                                         out, sizeof out, &out_len, SQL_DRIVER_NOPROMPT);
    if (!ok(r)) {
      const Diag d = diagnose(SQL_HANDLE_DBC, dbc_);
      if (d.message != last_error_) host_.log.warn("ODBC connect failed: [{}] {}", d.state, d.message);
      last_error_ = d.message;
      disconnect();
      return d.state == "HYT00" ? status::BadTimeout : status::BadNotConnected;
    }
    last_error_.clear();
    connected_ = true;
    SQLCHAR q[8] = {};
    SQLSMALLINT q_len = 0;
    quote_ = ok(SQLGetInfo(dbc_, SQL_IDENTIFIER_QUOTE_CHAR, q, sizeof q, &q_len)) ? reinterpret_cast<const char*>(q) : "\"";
    column_types_.clear();
    return status::Good;
  }

  void disconnect() override {
    if (dbc_) {
      if (connected_) SQLDisconnect(dbc_);
      SQLFreeHandle(SQL_HANDLE_DBC, dbc_);
      dbc_ = nullptr;
    }
    if (env_) {
      SQLFreeHandle(SQL_HANDLE_ENV, env_);
      env_ = nullptr;
    }
    connected_ = false;
  }

  bool connected() const override { return connected_; }

  sdk::StatusCode read(std::span<sdk::ReadItem*> items) override {
    std::map<std::string, std::vector<sdk::ReadItem*>> rows, queries;
    for (auto* item : items) {
      const auto& a = static_cast<const OdbcAddress&>(*item->address);
      if (a.kind == Kind::Exec) item->quality = status::BadNotReadable;
      else if (a.kind == Kind::Query) queries[a.sql].push_back(item);
      else rows[a.row_id()].push_back(item);
    }
    for (auto& [id, group] : rows) {
      if (auto st = read_row(group); st != status::Good) return st;
    }
    for (auto& [sql, group] : queries) {
      if (auto st = read_query(sql, group); st != status::Good) return st;
    }
    return status::Good;
  }

  sdk::StatusCode write(const sdk::Address& address, const sdk::Value& value) override {
    const auto& a = static_cast<const OdbcAddress&>(address);
    if (!connected_) return status::BadNotConnected;
    if (a.kind == Kind::Query) return status::BadNotWritable;
    sdk::DataType target = a.type.scalar;
    if (target == sdk::DataType::Default) {
      if (a.kind == Kind::Exec) {
        target = sdk::type_of(value);
      } else {
        auto t = column_type(a);
        if (!t) return t.error().code;
        target = odbc::natural_type(*t);
      }
    }
    auto v = sdk::convert(value, target);
    if (!v) return v.error().code;
    std::vector<Param> params{make_param(*v)};

    if (a.kind == Kind::Exec) {
      auto r = execute(a.sql, params);
      return r ? status::Good : r.error().code;
    }
    if (!a.key_column.empty()) params.push_back(key_param(a.key));
    // Commit only if exactly one row changed: a table without a key must hold
    // one row, a key must be unique.
    SQLSetConnectAttr(dbc_, SQL_ATTR_AUTOCOMMIT, reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0);
    auto changed = execute(odbc::update_sql(a, quote_), params);
    sdk::StatusCode st = status::Good;
    if (!changed) st = changed.error().code;
    else if (*changed == 0) st = status::BadNoData;
    else if (*changed != 1) st = status::BadNotWritable;
    if (changed && *changed > 1)
      host_.log.warn("ODBC write to {} would change {} rows; rolled back", a.text, *changed);
    if (connected_) {
      SQLEndTran(SQL_HANDLE_DBC, dbc_, st == status::Good ? SQL_COMMIT : SQL_ROLLBACK);
      SQLSetConnectAttr(dbc_, SQL_ATTR_AUTOCOMMIT, reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0);
    }
    return st;
  }

 private:
  // Status for a failed statement: a lost connection or timeout is a
  // communication error (the host reconnects); anything else (unknown table,
  // column, syntax) is a per-tag configuration error.
  sdk::StatusCode failure(const Diag& d, std::string_view what) {
    SQLUINTEGER dead = SQL_CD_FALSE;
    const bool lost = d.state.starts_with("08") || d.state == "57P01" ||
                      (ok(SQLGetConnectAttr(dbc_, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr)) && dead == SQL_CD_TRUE);
    if (lost || d.state == "HYT00" || d.state == "HYT01") {
      host_.log.warn("ODBC connection lost: [{}] {}", d.state, d.message);
      return lost ? status::BadNotConnected : status::BadTimeout;
    }
    if (errors_[std::string(what)] != d.message) host_.log.warn("ODBC: {}: [{}] {}", what, d.state, d.message);
    errors_[std::string(what)] = d.message;
    return status::BadConfigurationError;
  }

  bool comm_failure(sdk::StatusCode st) const {
    return st == status::BadNotConnected || st == status::BadTimeout;
  }

  // Runs a statement; returns the row count (SQLRowCount) or the failure status.
  sdk::Result<SQLLEN> execute(const std::string& sql, std::vector<Param>& params, Statement* keep = nullptr) {
    Statement own(dbc_);
    Statement& stmt = keep ? *keep : own;
    SQLSetStmtAttr(stmt.get(), SQL_ATTR_QUERY_TIMEOUT, reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(s_.query_timeout_s)), 0);
    for (std::size_t i = 0; i < params.size(); ++i) {
      Param& p = params[i];
      const SQLLEN buflen = p.ctype == SQL_C_CHAR ? static_cast<SQLLEN>(p.s.size()) : 0;
      if (!ok(SQLBindParameter(stmt.get(), static_cast<SQLUSMALLINT>(i + 1), SQL_PARAM_INPUT, p.ctype, p.sqltype, p.size, p.digits,
                               p.data(), buflen, p.ctype == SQL_C_CHAR ? &p.ind : nullptr)))
        return sdk::make_error(failure(stmt.diag(), sql), "bind failed");
    }
    const SQLRETURN r = SQLExecDirect(stmt.get(), reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS);
    if (r == SQL_NO_DATA) return SQLLEN{0};  // UPDATE/DELETE that matched no row
    if (!ok(r)) {
      const auto st = failure(stmt.diag(), sql);
      if (comm_failure(st)) connected_ = false;
      return sdk::make_error(st, "execute failed");
    }
    SQLLEN count = 0;
    SQLRowCount(stmt.get(), &count);
    return count;
  }

  // Text of column `col` of the current row; nullopt for NULL.
  sdk::Result<std::optional<std::string>> get_text(Statement& stmt, SQLUSMALLINT col) {
    std::string out;
    char buf[4096];
    while (true) {
      SQLLEN ind = 0;
      const SQLRETURN r = SQLGetData(stmt.get(), col, SQL_C_CHAR, buf, sizeof buf, &ind);
      if (r == SQL_NO_DATA) break;
      if (!ok(r)) return sdk::make_error(failure(stmt.diag(), "fetch"), "fetch failed");
      if (ind == SQL_NULL_DATA) return std::optional<std::string>{};
      if (r == SQL_SUCCESS_WITH_INFO && (ind == SQL_NO_TOTAL || ind >= static_cast<SQLLEN>(sizeof buf))) {
        out.append(buf, sizeof buf - 1);  // truncated: more to come
        continue;
      }
      out.append(buf, static_cast<std::size_t>(ind));
      break;
    }
    return std::optional<std::string>(std::move(out));
  }

  // Value of column `col` of the current row: a comm failure in `st` ends the
  // read; any other bad `st` (NULL, unparsable) is that column's quality.
  struct Column {
    sdk::StatusCode st = status::Good;
    sdk::Value value;
  };
  Column read_column(Statement& stmt, SQLUSMALLINT col) {
    SQLSMALLINT sql_type = 0, digits = 0, nullable = 0, name_len = 0;
    SQLULEN size = 0;
    SQLCHAR name[256];
    SQLDescribeCol(stmt.get(), col, name, sizeof name, &name_len, &sql_type, &size, &digits, &nullable);
    auto text = get_text(stmt, col);
    if (!text) return {text.error().code, {}};
    if (!*text) return {status::BadNoData, {}};
    auto v = odbc::from_text(**text, sql_type);
    if (!v) return {v.error().code, {}};
    return {status::Good, std::move(*v)};
  }

  // One SELECT for all columns of a table row.
  sdk::StatusCode read_row(std::vector<sdk::ReadItem*>& group) {
    const auto& row = static_cast<const OdbcAddress&>(*group.front()->address);
    std::vector<std::string> columns;
    for (auto* item : group) {
      const auto& c = static_cast<const OdbcAddress&>(*item->address).column;
      if (std::ranges::find(columns, c) == columns.end()) columns.push_back(c);
    }
    auto st = select_row(row, columns, group);
    if (st == status::BadConfigurationError && columns.size() > 1) {
      // Find the bad column(s): read each column on its own.
      for (const auto& c : columns) {
        std::vector<sdk::ReadItem*> sub;
        for (auto* item : group)
          if (static_cast<const OdbcAddress&>(*item->address).column == c) sub.push_back(item);
        st = select_row(row, {c}, sub);
        if (comm_failure(st)) return st;
      }
      return status::Good;
    }
    return comm_failure(st) ? st : status::Good;
  }

  sdk::StatusCode select_row(const OdbcAddress& row, const std::vector<std::string>& columns, std::vector<sdk::ReadItem*>& items) {
    Statement stmt(dbc_);
    std::vector<Param> params;
    if (!row.key_column.empty()) params.push_back(key_param(row.key));
    const std::string sql = odbc::select_sql(row, columns, quote_);
    auto r = execute(sql, params, &stmt);
    if (!r) {
      for (auto* item : items) item->quality = r.error().code;
      return r.error().code;
    }
    return fetch_first(stmt, items, [&](const sdk::ReadItem& item) -> SQLUSMALLINT {
      const auto& c = static_cast<const OdbcAddress&>(*item.address).column;
      return static_cast<SQLUSMALLINT>(std::ranges::find(columns, c) - columns.begin() + 1);
    });
  }

  sdk::StatusCode read_query(const std::string& sql, std::vector<sdk::ReadItem*>& items) {
    Statement stmt(dbc_);
    std::vector<Param> none;
    auto r = execute(sql, none, &stmt);
    if (!r) {
      for (auto* item : items) item->quality = r.error().code;
      return comm_failure(r.error().code) ? r.error().code : status::Good;
    }
    // Result column numbers by name (case-insensitive).
    SQLSMALLINT ncols = 0;
    SQLNumResultCols(stmt.get(), &ncols);
    std::vector<std::string> names;
    for (SQLSMALLINT i = 1; i <= ncols; ++i) {
      SQLCHAR name[256] = {};
      SQLSMALLINT len = 0;
      SQLColAttribute(stmt.get(), static_cast<SQLUSMALLINT>(i), SQL_DESC_LABEL, name, sizeof name, &len, nullptr);
      std::string n(reinterpret_cast<const char*>(name));
      std::ranges::transform(n, n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      names.push_back(std::move(n));
    }
    if (ncols == 0) {
      for (auto* item : items) item->quality = status::BadConfigurationError;
      return status::Good;
    }
    auto st = fetch_first(stmt, items, [&](const sdk::ReadItem& item) -> SQLUSMALLINT {
      std::string c = static_cast<const OdbcAddress&>(*item.address).column;
      if (c.empty()) return 1;
      std::ranges::transform(c, c.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      auto it = std::ranges::find(names, c);
      return it == names.end() ? 0 : static_cast<SQLUSMALLINT>(it - names.begin() + 1);
    });
    return comm_failure(st) ? st : status::Good;
  }

  // Fetches the first row and fills each item from its column (0: missing).
  template <typename ColumnOf>
  sdk::StatusCode fetch_first(Statement& stmt, std::vector<sdk::ReadItem*>& items, ColumnOf column_of) {
    const SQLRETURN r = SQLFetch(stmt.get());
    const auto now = sdk::Clock::now();
    if (r == SQL_NO_DATA) {
      for (auto* item : items) item->quality = status::BadNoData;
      return status::Good;
    }
    if (!ok(r)) {
      const auto st = failure(stmt.diag(), "fetch");
      if (comm_failure(st)) connected_ = false;
      for (auto* item : items) item->quality = st;
      return st;
    }
    // SQLGetData must go through the columns in order.
    std::vector<std::pair<SQLUSMALLINT, sdk::ReadItem*>> order;
    for (auto* item : items) order.emplace_back(column_of(*item), item);
    std::ranges::stable_sort(order, {}, &std::pair<SQLUSMALLINT, sdk::ReadItem*>::first);
    SQLUSMALLINT last = 0;
    Column c;
    for (auto& [col, item] : order) {
      item->source_time = now;
      if (col == 0) {
        item->quality = status::BadConfigurationError;
        continue;
      }
      if (col != last) {
        c = read_column(stmt, col);
        last = col;
        if (comm_failure(c.st)) {
          connected_ = false;
          item->quality = c.st;
          return c.st;
        }
      }
      if (c.st != status::Good) {
        item->quality = c.st;
        continue;
      }
      auto v = item->address->type.scalar == sdk::DataType::Default ? sdk::Result<sdk::Value>(c.value)
                                                                    : sdk::convert(c.value, item->address->type.scalar);
      item->quality = v ? status::Good : v.error().code;
      if (v) item->value = std::move(*v);
    }
    return status::Good;
  }

  // SQL type of a table column (for writes to Default-typed tags), cached.
  sdk::Result<int> column_type(const OdbcAddress& a) {
    const std::string id = a.row_id() + '\x1e' + a.column;
    if (auto it = column_types_.find(id); it != column_types_.end()) return it->second;
    Statement stmt(dbc_);
    std::string sql = "SELECT " + odbc::quote({a.column}, quote_) + " FROM " + odbc::quote(a.table, quote_) + " WHERE 1 = 0";
    std::vector<Param> none;
    auto r = execute(sql, none, &stmt);
    if (!r) return std::unexpected(r.error());
    SQLSMALLINT sql_type = 0, digits = 0, nullable = 0, name_len = 0;
    SQLULEN size = 0;
    SQLCHAR name[256];
    if (!ok(SQLDescribeCol(stmt.get(), 1, name, sizeof name, &name_len, &sql_type, &size, &digits, &nullable)))
      return sdk::make_error(status::BadConfigurationError, "cannot describe column");
    column_types_[id] = sql_type;
    return sql_type;
  }

  Settings s_;
  sdk::HostContext& host_;
  SQLHENV env_ = nullptr;
  SQLHDBC dbc_ = nullptr;
  bool connected_ = false;
  std::string quote_ = "\"";
  std::string last_error_;
  std::unordered_map<std::string, std::string> errors_;  // last message per statement, to log changes only
  std::unordered_map<std::string, int> column_types_;
};

nlohmann::json prop(const char* key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", std::string(kId) + "." + key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

class OdbcClientDriver final : public sdk::Driver {
 public:
  sdk::DriverInfo info() const override {
    return {kId, "ODBC Client", "0.1.0", sdk::Medium::Ethernet, {"ODBC"}};
  }

  nlohmann::json schema() const override {
    using nlohmann::json;
    return {
        {"driver", kId},
        {"display_name", "ODBC Client"},
        {"channel", json::array()},
        {"device",
         json::array({
             {{"key", "servermain.DEVICE_ID_STRING"}, {"type", "string"}, {"default", "DSN=logict"},
              {"description", "ODBC connection string, e.g. DSN=plant or Driver=PostgreSQL Unicode;Server=db;Database=plant"}},
             prop("DEVICE_USERNAME", "string", "", "User name (added as UID)"),
             prop("DEVICE_PASSWORD", "string", "", "Password (added as PWD)"),
         })},
        {"address_help",
         "<table>.<column> (first row), <table>[<key column>=<value>].<column> (row by key; value is a number or 'text'), "
         "sql:<select> (first column of the first row), sql.<column>:<select>, exec:<statement with one ?> (write-only). "
         "Table writes update exactly one row."},
    };
  }

  sdk::Result<std::unique_ptr<sdk::DeviceRuntime>> create_device(const sdk::ChannelConfig&, const sdk::DeviceConfig& device,
                                                                 sdk::HostContext& host) override {
    const auto& p = device.properties;
    auto key = [](const char* k) { return std::string(kId) + "." + k; };
    Settings s;
    s.connection = device.id;
    if (s.connection.find('=') == std::string::npos)
      return sdk::make_error(status::BadConfigurationError, "device ID must be an ODBC connection string such as DSN=plant");
    while (!s.connection.empty() && (s.connection.back() == ';' || s.connection.back() == ' ')) s.connection.pop_back();
    s.username = sdk::property<std::string>(p, key("DEVICE_USERNAME"), "");
    s.password = sdk::property<std::string>(p, key("DEVICE_PASSWORD"), "");
    s.login_timeout_s = static_cast<int>(std::max<long long>(1, std::chrono::ceil<std::chrono::seconds>(device.connect_timeout).count()));
    s.query_timeout_s = static_cast<int>(std::max<long long>(1, std::chrono::ceil<std::chrono::seconds>(device.request_timeout).count()));
    return std::make_unique<OdbcDevice>(std::move(s), host);
  }
};

}  // namespace

LOGICT_DECLARE_DRIVER(OdbcClientDriver)
