#include "mqtt_payload.h"

#include <cctype>
#include <charconv>
#include <regex>

namespace logict::mqtt_client {

namespace status = sdk::status;
using nlohmann::json;

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

sdk::Result<std::vector<PathStep>> parse_path(std::string_view p) {
  if (p.starts_with("$.")) p.remove_prefix(2);
  else if (p.starts_with("$")) p.remove_prefix(1);
  std::vector<PathStep> steps;
  std::size_t i = 0;
  while (i < p.size()) {
    if (p[i] == '[') {
      auto close = p.find(']', i);
      if (close == std::string_view::npos) return sdk::make_error(status::BadInvalidArgument, "missing ']' in JSON path");
      std::size_t n = 0;
      auto r = std::from_chars(p.data() + i + 1, p.data() + close, n);
      if (r.ec != std::errc{} || r.ptr != p.data() + close) return sdk::make_error(status::BadInvalidArgument, "bad array index in JSON path");
      steps.emplace_back(n);
      i = close + 1;
      if (i < p.size() && p[i] == '.') ++i;
      continue;
    }
    auto end = p.find_first_of(".[", i);
    if (end == std::string_view::npos) end = p.size();
    if (end == i) return sdk::make_error(status::BadInvalidArgument, "empty member name in JSON path");
    steps.emplace_back(std::string(p.substr(i, end - i)));
    i = end;
    if (i < p.size() && p[i] == '.') ++i;
  }
  if (steps.empty()) return sdk::make_error(status::BadInvalidArgument, "empty JSON path");
  return steps;
}

sdk::Result<sdk::Value> from_json(const json& j) {
  switch (j.type()) {
    case json::value_t::boolean: return sdk::Value(j.get<bool>());
    case json::value_t::number_integer: return sdk::Value(j.get<std::int64_t>());
    case json::value_t::number_unsigned: return sdk::Value(j.get<std::uint64_t>());
    case json::value_t::number_float: return sdk::Value(j.get<double>());
    case json::value_t::string: return sdk::Value(j.get<std::string>());
    case json::value_t::array: {
      sdk::Array arr;
      for (const auto& e : j) {
        if (e.is_array() || e.is_object()) return sdk::make_error(status::BadTypeMismatch, "nested arrays are not supported");
        auto v = from_json(e);
        if (!v) return v;
        arr.push_back(std::move(*v));
      }
      return sdk::Value(std::move(arr));
    }
    case json::value_t::object: return sdk::Value(j.dump());
    default: return sdk::make_error(status::BadTypeMismatch, "null value");
  }
}

json to_json(const sdk::Value& v) {
  return std::visit(
      [](const auto& x) -> json {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) return nullptr;
        else if constexpr (std::is_same_v<T, sdk::DateTime>) return sdk::to_string(sdk::Value(x));
        else if constexpr (std::is_same_v<T, sdk::Array>) {
          json arr = json::array();
          for (const auto& e : x) arr.push_back(to_json(e));
          return arr;
        } else if constexpr (std::is_same_v<T, std::int8_t> || std::is_same_v<T, std::uint8_t>) return static_cast<int>(x);
        else return x;
      },
      v.data);
}

// Converts to the tag type; scalars and arrays must match the tag's shape.
sdk::Result<sdk::Value> shape(const MqttAddress& a, sdk::Value v) {
  if (a.type.is_array() != v.is_array()) return sdk::make_error(status::BadTypeMismatch, "payload shape does not match the tag");
  if (a.type.is_array() && v.get_if<sdk::Array>()->size() != a.type.element_count())
    return sdk::make_error(status::BadTypeMismatch, "array length does not match the tag");
  return sdk::convert(v, a.type.scalar);
}

}  // namespace

sdk::Result<std::shared_ptr<MqttAddress>> parse_address(std::string_view text, sdk::TagType requested) {
  auto a = std::make_shared<MqttAddress>();
  a->text = std::string(text);
  std::string_view s = trim(text);
  if (s.starts_with("spb:")) {
    s.remove_prefix(4);
    auto bar = s.find('|');
    if (bar == std::string_view::npos || trim(s.substr(bar + 1)).empty())
      return sdk::make_error(status::BadInvalidArgument, "expected spb:<group>/<edge node>[/<device>]|<metric>");
    a->metric = std::string(trim(s.substr(bar + 1)));
    std::vector<std::string> ids;
    std::string_view rest = trim(s.substr(0, bar));
    for (std::size_t start = 0;;) {
      auto slash = rest.find('/', start);
      ids.emplace_back(rest.substr(start, slash - start));
      if (slash == std::string_view::npos) break;
      start = slash + 1;
    }
    if (ids.size() < 2 || ids.size() > 3) return sdk::make_error(status::BadInvalidArgument, "expected <group>/<edge node>[/<device>]");
    for (const auto& id : ids)
      if (id.empty() || id.find_first_of("+#") != std::string::npos)
        return sdk::make_error(status::BadInvalidArgument, "bad Sparkplug group, edge node or device ID");
    if (requested.is_array()) return sdk::make_error(status::BadTypeMismatch, "Sparkplug array metrics are not supported");
    a->sparkplug = true;
    a->group = ids[0];
    a->edge = ids[1];
    if (ids.size() == 3) a->device = ids[2];
    a->topic = "spBv1.0/" + a->group + "/+/" + a->edge + "/#";
    a->type = sdk::TagType{requested.scalar};
    return a;
  }
  // Array dimensions after a space: "plant/arr [3]", "plant/m|rows [2][3]".
  static const std::regex dims_re(R"(\s+\[(\d{1,5})\](?:\[(\d{1,5})\])?$)");
  std::match_results<std::string_view::const_iterator> m;
  std::uint32_t rows = 0, cols = 0;
  if (std::regex_search(s.begin(), s.end(), m, dims_re)) {
    rows = static_cast<std::uint32_t>(std::stoul(m[1].str()));
    cols = m[2].matched ? static_cast<std::uint32_t>(std::stoul(m[2].str())) : 0;
    if (rows == 0 || (m[2].matched && cols == 0) || rows * std::max(cols, 1u) > 65535)
      return sdk::make_error(status::BadInvalidArgument, "bad array dimensions");
    s = trim(s.substr(0, static_cast<std::size_t>(m.position(0))));
  } else if (requested.is_array()) {
    return sdk::make_error(status::BadInvalidArgument, "array tags need dimensions, e.g. \"plant/arr [10]\"");
  }
  if (auto bar = s.find('|'); bar != std::string_view::npos) {
    auto path = parse_path(trim(s.substr(bar + 1)));
    if (!path) return std::unexpected(path.error());
    a->path = std::move(*path);
    a->has_path = true;
    s = trim(s.substr(0, bar));
  }
  if (s.empty()) return sdk::make_error(status::BadInvalidArgument, "empty topic");
  // '#' only as the last level, '+' only as a whole level.
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '+' && s[i] != '#') continue;
    const bool whole = (i == 0 || s[i - 1] == '/') && (i + 1 == s.size() || s[i + 1] == '/');
    if (!whole || (s[i] == '#' && i + 1 != s.size())) return sdk::make_error(status::BadInvalidArgument, "bad wildcard in topic");
    a->wildcard = true;
  }
  a->topic = std::string(s);
  a->type = rows ? sdk::TagType{requested.scalar, rows, cols} : sdk::TagType{requested.scalar};
  if (a->type.scalar == sdk::DataType::Default) a->type.scalar = a->has_path ? sdk::DataType::Double : sdk::DataType::String;
  return a;
}

bool topic_matches(std::string_view filter, std::string_view topic) {
  if (!topic.empty() && topic.front() == '$' && (filter.empty() || (filter.front() == '+' || filter.front() == '#')))
    return false;
  while (true) {
    auto fslash = filter.find('/');
    auto f = filter.substr(0, fslash);
    if (f == "#") return true;
    auto tslash = topic.find('/');
    auto t = topic.substr(0, tslash);
    if (f != "+" && f != t) return false;
    if (fslash == std::string_view::npos || tslash == std::string_view::npos) {
      // "a/#" also matches "a".
      return fslash == tslash || (tslash == std::string_view::npos && filter.substr(fslash + 1) == "#");
    }
    filter.remove_prefix(fslash + 1);
    topic.remove_prefix(tslash + 1);
  }
}

sdk::Result<sdk::Value> decode(const MqttAddress& a, std::string_view payload) {
  if (!a.has_path) {
    if (a.type.scalar == sdk::DataType::String && !a.type.is_array()) return sdk::Value(std::string(payload));
    json j = json::parse(payload, nullptr, false);
    if (j.is_discarded() || j.is_object()) return shape(a, sdk::Value(std::string(trim(payload))));
    auto v = from_json(j);
    if (!v) return v;
    return shape(a, std::move(*v));
  }
  json j = json::parse(payload, nullptr, false);
  if (j.is_discarded()) return sdk::make_error(status::BadTypeMismatch, "payload is not JSON");
  const json* cur = &j;
  for (const auto& step : a.path) {
    if (const auto* key = std::get_if<std::string>(&step)) {
      if (!cur->is_object() || !cur->contains(*key)) return sdk::make_error(status::BadOutOfRange, "member not in payload");
      cur = &(*cur)[*key];
    } else {
      const auto idx = std::get<std::size_t>(step);
      if (!cur->is_array() || idx >= cur->size()) return sdk::make_error(status::BadOutOfRange, "index not in payload");
      cur = &(*cur)[idx];
    }
  }
  auto v = from_json(*cur);
  if (!v) return v;
  return shape(a, std::move(*v));
}

sdk::Result<std::string> encode(const MqttAddress& a, const sdk::Value& v) {
  if (a.wildcard) return sdk::make_error(status::BadNotWritable, "wildcard topics are read-only");
  auto typed = shape(a, v);
  if (!typed) return std::unexpected(typed.error());
  if (!a.has_path) {
    if (const auto* s = typed->get_if<std::string>()) return *s;
    return to_json(*typed).dump();
  }
  json out = to_json(*typed);
  for (auto it = a.path.rbegin(); it != a.path.rend(); ++it) {
    const auto* key = std::get_if<std::string>(&*it);
    if (key == nullptr) return sdk::make_error(status::BadNotWritable, "paths with array indexes are read-only");
    out = json{{*key, std::move(out)}};
  }
  return out.dump();
}

}  // namespace logict::mqtt_client
