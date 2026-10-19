#include "sysmon.h"

#include <charconv>
#include <cstdlib>
#include <string>

namespace logict::sysmon {

namespace {

std::string_view next_field(std::string_view& s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  auto end = s.find_first_of(" \t\n");
  auto field = s.substr(0, end);
  s.remove_prefix(end == std::string_view::npos ? s.size() : end);
  return field;
}

std::optional<std::uint64_t> to_u64(std::string_view s) {
  std::uint64_t v;
  auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
  return v;
}

}  // namespace

std::optional<CpuTimes> parse_cpu(std::string_view text) {
  // "cpu  user nice system idle iowait irq softirq steal guest guest_nice"
  if (!text.starts_with("cpu ")) return std::nullopt;
  text.remove_prefix(3);
  text = text.substr(0, text.find('\n'));
  CpuTimes t;
  for (int i = 0; i < 8; ++i) {  // guest time is already counted in user/nice
    auto f = next_field(text);
    if (f.empty()) {
      if (i < 4) return std::nullopt;
      break;
    }
    auto v = to_u64(f);
    if (!v) return std::nullopt;
    t.total += *v;
    if (i != 3 && i != 4) t.busy += *v;  // idle and iowait are not busy
  }
  return t;
}

double cpu_usage(const CpuTimes& before, const CpuTimes& after) {
  if (after.total <= before.total || after.busy < before.busy) return 0.0;
  return 100.0 * static_cast<double>(after.busy - before.busy) / static_cast<double>(after.total - before.total);
}

std::map<std::string, std::uint64_t, std::less<>> parse_meminfo(std::string_view text) {
  std::map<std::string, std::uint64_t, std::less<>> out;
  while (!text.empty()) {
    auto nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
    auto colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    std::string_view rest = line.substr(colon + 1);
    auto v = to_u64(next_field(rest));
    if (!v) continue;
    const bool kb = next_field(rest) == "kB";
    out.emplace(std::string(line.substr(0, colon)), kb ? *v * 1024 : *v);
  }
  return out;
}

std::optional<LoadAvg> parse_loadavg(std::string_view text) {
  LoadAvg l;
  double* fields[] = {&l.one, &l.five, &l.fifteen};
  for (double* f : fields) {
    auto s = next_field(text);
    auto r = std::from_chars(s.data(), s.data() + s.size(), *f);
    if (s.empty() || r.ec != std::errc{}) return std::nullopt;
  }
  return l;
}

}  // namespace logict::sysmon
