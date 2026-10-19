#pragma once

// Parsers for the Linux files the System Monitor driver reads. Pure functions
// so they can be tested with fixed text.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace logict::sysmon {

// Aggregate "cpu" line of /proc/stat.
struct CpuTimes {
  std::uint64_t busy = 0, total = 0;
};
std::optional<CpuTimes> parse_cpu(std::string_view proc_stat);

// Usage in percent between two samples; 0 if no time passed.
double cpu_usage(const CpuTimes& before, const CpuTimes& after);

// /proc/meminfo as key -> bytes (values are given in kB).
std::map<std::string, std::uint64_t, std::less<>> parse_meminfo(std::string_view text);

// /proc/loadavg: 1, 5 and 15 minute averages.
struct LoadAvg {
  double one = 0, five = 0, fifteen = 0;
};
std::optional<LoadAvg> parse_loadavg(std::string_view text);

}  // namespace logict::sysmon
