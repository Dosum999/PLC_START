#include "mqtt/template.h"

#include <cmath>
#include <ctime>
#include <format>
#include <map>

#include <nlohmann/json.hpp>

namespace logict::mqtt {

namespace status = sdk::status;

enum class Var {
  ServerName, ServerTimestamp, ServerDate, ServerIsoUtc, ServerIsoLocal, LineFeed,
  TagName, Channel, Device, Tag, Value, Quality, Timestamp, Address, Description, ClientAccess, IsoUtc, IsoLocal,
  StatusCode,
};

struct Template::Node {
  enum Kind { Text, Variable, Each, UnlessLast } kind = Text;
  std::string text;
  Var var{};
  std::vector<std::shared_ptr<const Node>> children;
};

namespace {

using NodePtr = std::shared_ptr<const Template::Node>;

const std::map<std::string_view, Var>& top_vars() {
  static const std::map<std::string_view, Var> m{
      {"SERVERNAME", Var::ServerName},         {"SERVERTIMESTAMP", Var::ServerTimestamp},
      {"SERVERDATE", Var::ServerDate},         {"SERVERISO8601UTC", Var::ServerIsoUtc},
      {"SERVERISO8601LOCAL", Var::ServerIsoLocal}, {"LINEFEED", Var::LineFeed},
  };
  return m;
}

const std::map<std::string_view, Var>& event_vars() {
  static const std::map<std::string_view, Var> m{
      {"TAGNAME", Var::TagName},       {"CHANNEL", Var::Channel},           {"DEVICE", Var::Device},
      {"TAG", Var::Tag},               {"VALUE", Var::Value},               {"TAGVALUE", Var::Value},
      {"QUALITY", Var::Quality},       {"TAGQUALITY", Var::Quality},        {"TIMESTAMP", Var::Timestamp},
      {"TAGTIMESTAMP", Var::Timestamp}, {"ADDRESS", Var::Address},          {"TAGADDRESS", Var::Address},
      {"DESCRIPTION", Var::Description}, {"TAGDESCRIPTION", Var::Description}, {"CLIENTACCESS", Var::ClientAccess},
      {"TAGCLIENTACCESS", Var::ClientAccess}, {"ISO8601UTC", Var::IsoUtc}, {"TAGISO8601UTC", Var::IsoUtc},
      {"ISO8601LOCAL", Var::IsoLocal}, {"TAGISO8601LOCAL", Var::IsoLocal}, {"STATUSCODE", Var::StatusCode},
      {"TAGSTATUSCODE", Var::StatusCode},
  };
  return m;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  return s;
}

int line_of(std::string_view text, std::size_t pos) {
  int line = 1;
  for (std::size_t i = 0; i < pos && i < text.size(); ++i) line += text[i] == '\n';
  return line;
}

// Parser state shared with the recursive walker in Template::advanced().
struct Parser {
  explicit Parser(std::string_view t) : text_(t) {}

  NodePtr text(std::string_view s) const {
    auto n = std::make_shared<Template::Node>();
    n->text = std::string(s);
    return n;
  }

  sdk::Unexpected error(std::string msg, std::size_t at) const {
    return sdk::make_error(status::BadConfigurationError, std::format("template line {}: {}", line_of(text_, at), msg));
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  bool closed_ = false;  // set when a nested block consumed its closing tag
};

std::string escaped(std::string_view s) {
  std::string q = nlohmann::json(std::string(s)).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  return q.substr(1, q.size() - 2);
}

std::int64_t epoch_ms(sdk::Timestamp t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

std::string iso_utc(sdk::Timestamp t) {
  return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(t));
}

std::string iso_local(sdk::Timestamp t, bool human) {
  const auto ms = epoch_ms(t);
  std::time_t secs = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
  localtime_r(&secs, &tm);
  char buf[64];
  std::strftime(buf, sizeof(buf), human ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%dT%H:%M:%S", &tm);
  std::string out = std::format("{}.{:03}", buf, ms % 1000);
  if (human) return out;
  char off[8];
  std::strftime(off, sizeof(off), "%z", &tm);  // +0900
  std::string z(off);
  if (z.size() == 5) z.insert(3, ":");
  return out + z;
}

}  // namespace

std::string json_value(const sdk::Value& v, const sdk::TagType* shape) {
  return std::visit(
      [&](const auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return "null";
        } else if constexpr (std::is_same_v<T, bool>) {
          return x ? "true" : "false";
        } else if constexpr (std::is_floating_point_v<T>) {
          return std::isfinite(x) ? std::format("{}", x) : "null";
        } else if constexpr (std::is_arithmetic_v<T>) {
          return std::to_string(x);
        } else if constexpr (std::is_same_v<T, std::string>) {
          return "\"" + escaped(x) + "\"";
        } else {
          return "\"" + escaped(sdk::to_string(v, shape)) + "\"";  // DateTime, Array
        }
      },
      v.data);
}

sdk::Result<Template> Template::advanced(std::string_view text) {
  Parser p(text);
  // Recursive descent over |token| markers; a nested call returns when it
  // reaches its closing tag.
  struct Walker {
    Parser& p;
    sdk::Result<std::vector<NodePtr>> run(int depth, std::string_view closing) {
      std::vector<NodePtr> out;
      const std::string_view t = p.text_;
      while (p.pos_ < t.size()) {
        auto bar = t.find('|', p.pos_);
        if (bar == std::string_view::npos) {
          out.push_back(p.text(t.substr(p.pos_)));
          p.pos_ = t.size();
          break;
        }
        if (bar > p.pos_) out.push_back(p.text(t.substr(p.pos_, bar - p.pos_)));
        auto end = t.find('|', bar + 1);
        if (end == std::string_view::npos) return p.error("unterminated '|'", bar);
        const std::string_view tok = trim(t.substr(bar + 1, end - bar - 1));
        p.pos_ = end + 1;
        if (tok.starts_with('/')) {
          if (closing.empty() || tok.substr(1) != closing) return p.error(std::format("unexpected '|{}|'", tok), bar);
          p.closed_ = true;
          return out;
        }
        if (tok.starts_with('#')) {
          auto node = std::make_shared<Node>();
          std::string_view close;
          if (tok == "#each VALUES") {
            if (depth > 0) return p.error("nested '#each' is not supported", bar);
            node->kind = Node::Each;
            close = "each";
          } else if (tok == "#unless @last") {
            if (depth == 0) return p.error("'#unless @last' outside '#each'", bar);
            node->kind = Node::UnlessLast;
            close = "unless";
          } else {
            return p.error(std::format("unknown block '|{}|'", tok), bar);
          }
          p.closed_ = false;
          auto children = run(depth + (node->kind == Node::Each ? 1 : 0), close);
          if (!children) return children;
          if (!p.closed_) return p.error(std::format("missing '|/{}|'", close), bar);
          p.closed_ = false;
          node->children = std::move(*children);
          out.push_back(std::move(node));
          continue;
        }
        auto node = std::make_shared<Node>();
        node->kind = Node::Variable;
        if (auto it = top_vars().find(tok); it != top_vars().end()) {
          node->var = it->second;
        } else if (auto ev = event_vars().find(tok); ev != event_vars().end()) {
          if (depth == 0) return p.error(std::format("'|{}|' is only valid inside '#each VALUES'", tok), bar);
          node->var = ev->second;
        } else {
          return p.error(std::format("unknown variable '|{}|'", tok), bar);
        }
        out.push_back(std::move(node));
      }
      return out;
    }
  } walker{p};
  auto nodes = walker.run(0, "");
  if (!nodes) return std::unexpected(nodes.error());
  Template t;
  t.nodes_ = std::move(*nodes);
  return t;
}

sdk::Result<Template> Template::standard(std::string_view top, std::string_view expansion) {
  if (expansion.find("|#") != std::string_view::npos || expansion.find("|VALUES|") != std::string_view::npos)
    return sdk::make_error(status::BadConfigurationError, "expansion of |VALUES| may not contain blocks or |VALUES|");
  const auto at = top.find("|VALUES|");
  if (at == std::string_view::npos)
    return sdk::make_error(status::BadConfigurationError, "standard template must contain |VALUES|");
  std::string text;
  text += top.substr(0, at);
  text += "|#each VALUES|";
  text += expansion;
  text += "|#unless @last|,|/unless||/each|";
  text += top.substr(at + 8);
  return advanced(text);
}

namespace {

struct Ctx {
  std::string_view server;
  sdk::Timestamp now;
  const std::vector<EventView>* events;
};

void render_nodes(const std::vector<NodePtr>& nodes, const Ctx& c, const EventView* ev, bool last, std::string& out) {
  for (const auto& n : nodes) {
    switch (n->kind) {
      case Template::Node::Text:
        out += n->text;
        break;
      case Template::Node::Each:
        for (std::size_t i = 0; i < c.events->size(); ++i)
          render_nodes(n->children, c, &(*c.events)[i], i + 1 == c.events->size(), out);
        break;
      case Template::Node::UnlessLast:
        if (!last) render_nodes(n->children, c, ev, last, out);
        break;
      case Template::Node::Variable:
        switch (n->var) {
          case Var::ServerName: out += escaped(c.server); break;
          case Var::ServerTimestamp: out += std::to_string(epoch_ms(c.now)); break;
          case Var::ServerDate: out += iso_local(c.now, true); break;
          case Var::ServerIsoUtc: out += iso_utc(c.now); break;
          case Var::ServerIsoLocal: out += iso_local(c.now, false); break;
          case Var::LineFeed: out += '\n'; break;
          case Var::TagName: out += escaped(ev->path); break;
          case Var::Channel: out += escaped(ev->channel); break;
          case Var::Device: out += escaped(ev->device); break;
          case Var::Tag: out += escaped(ev->tag); break;
          case Var::Address: out += escaped(ev->address); break;
          case Var::Description: out += escaped(ev->description); break;
          case Var::ClientAccess: out += ev->writable ? "RW" : "RO"; break;
          case Var::Value: out += json_value(ev->vqt->value, ev->type); break;
          case Var::Quality: out += sdk::is_good(ev->vqt->quality) ? "true" : "false"; break;
          case Var::StatusCode: out += std::to_string(ev->vqt->quality); break;
          case Var::Timestamp: out += std::to_string(epoch_ms(ev->vqt->source_time)); break;
          case Var::IsoUtc: out += iso_utc(ev->vqt->source_time); break;
          case Var::IsoLocal: out += iso_local(ev->vqt->source_time, false); break;
        }
        break;
    }
  }
}

}  // namespace

std::string Template::render(std::string_view server_name, sdk::Timestamp now,
                             const std::vector<EventView>& events) const {
  std::string out;
  Ctx c{server_name, now, &events};
  render_nodes(nodes_, c, nullptr, true, out);
  return out;
}

}  // namespace logict::mqtt
