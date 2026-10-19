// System Monitor driver: host health as read-only tags. Kepware's System
// Monitor reads Windows performance counters; this one reads the Linux
// equivalents from /proc, /sys and statvfs.
//
// Addresses (group and item case-insensitive, the argument in parentheses is
// taken as written):
//   CPU.Usage (%), CPU.Count
//   Memory.Total / Available / Used / Free (bytes), Memory.UsedPercent
//   Swap.Total / Used / Free (bytes), Swap.UsedPercent
//   Load.1 / Load.5 / Load.15
//   System.Uptime (s), System.Hostname, System.ProcessCount
//   Disk(<mount path>).Total / Free / Available / Used (bytes), .UsedPercent
//   Network(<interface>).RxBytes / TxBytes / RxPackets / TxPackets / RxErrors / TxErrors, .Up
//   Process(<name>).Count  (processes whose name, as in /proc/<pid>/comm, matches)
//   Thermal(<zone number>).Temperature (degrees C)
// CPU.Usage is the average since the previous read of the device.

#include <dirent.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cctype>
#include <fstream>
#include <sstream>
#include <thread>

#include "logict/sdk/driver.h"
#include "sysmon.h"

namespace {

using namespace logict::sdk;
namespace sysmon = logict::sysmon;

enum class Item {
  CpuUsage, CpuCount,
  MemTotal, MemAvailable, MemUsed, MemFree, MemUsedPercent,
  SwapTotal, SwapUsed, SwapFree, SwapUsedPercent,
  Load1, Load5, Load15,
  Uptime, Hostname, ProcessCount,
  DiskTotal, DiskFree, DiskAvailable, DiskUsed, DiskUsedPercent,
  NetRxBytes, NetTxBytes, NetRxPackets, NetTxPackets, NetRxErrors, NetTxErrors, NetUp,
  ProcCount,
  Temperature,
};

struct SysAddress final : Address {
  Item item{};
  std::string arg;
};

struct Spec {
  const char* group;
  const char* name;
  Item item;
  DataType type;
};

constexpr Spec kSpecs[] = {
    {"CPU", "USAGE", Item::CpuUsage, DataType::Float},
    {"CPU", "COUNT", Item::CpuCount, DataType::DWord},
    {"MEMORY", "TOTAL", Item::MemTotal, DataType::QWord},
    {"MEMORY", "AVAILABLE", Item::MemAvailable, DataType::QWord},
    {"MEMORY", "USED", Item::MemUsed, DataType::QWord},
    {"MEMORY", "FREE", Item::MemFree, DataType::QWord},
    {"MEMORY", "USEDPERCENT", Item::MemUsedPercent, DataType::Float},
    {"SWAP", "TOTAL", Item::SwapTotal, DataType::QWord},
    {"SWAP", "USED", Item::SwapUsed, DataType::QWord},
    {"SWAP", "FREE", Item::SwapFree, DataType::QWord},
    {"SWAP", "USEDPERCENT", Item::SwapUsedPercent, DataType::Float},
    {"LOAD", "1", Item::Load1, DataType::Float},
    {"LOAD", "5", Item::Load5, DataType::Float},
    {"LOAD", "15", Item::Load15, DataType::Float},
    {"SYSTEM", "UPTIME", Item::Uptime, DataType::Double},
    {"SYSTEM", "HOSTNAME", Item::Hostname, DataType::String},
    {"SYSTEM", "PROCESSCOUNT", Item::ProcessCount, DataType::DWord},
    {"DISK", "TOTAL", Item::DiskTotal, DataType::QWord},
    {"DISK", "FREE", Item::DiskFree, DataType::QWord},
    {"DISK", "AVAILABLE", Item::DiskAvailable, DataType::QWord},
    {"DISK", "USED", Item::DiskUsed, DataType::QWord},
    {"DISK", "USEDPERCENT", Item::DiskUsedPercent, DataType::Float},
    {"NETWORK", "RXBYTES", Item::NetRxBytes, DataType::QWord},
    {"NETWORK", "TXBYTES", Item::NetTxBytes, DataType::QWord},
    {"NETWORK", "RXPACKETS", Item::NetRxPackets, DataType::QWord},
    {"NETWORK", "TXPACKETS", Item::NetTxPackets, DataType::QWord},
    {"NETWORK", "RXERRORS", Item::NetRxErrors, DataType::QWord},
    {"NETWORK", "TXERRORS", Item::NetTxErrors, DataType::QWord},
    {"NETWORK", "UP", Item::NetUp, DataType::Boolean},
    {"PROCESS", "COUNT", Item::ProcCount, DataType::DWord},
    {"THERMAL", "TEMPERATURE", Item::Temperature, DataType::Float},
};

bool takes_argument(std::string_view group) {
  return group == "DISK" || group == "NETWORK" || group == "PROCESS" || group == "THERMAL";
}

std::string upper(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::optional<std::string> slurp(const std::string& path) {
  std::ifstream f(path);
  if (!f) return std::nullopt;
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::optional<std::uint64_t> read_u64(const std::string& path) {
  auto s = slurp(path);
  if (!s) return std::nullopt;
  try {
    return std::stoull(*s);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

bool safe_name(std::string_view s) {  // interface names / zone numbers become path components
  return !s.empty() && s.find('/') == std::string_view::npos && s != "." && s != "..";
}

double percent(std::uint64_t part, std::uint64_t whole) {
  return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

class SysDevice final : public DeviceRuntime {
 public:
  Result<std::shared_ptr<const Address>> parse_address(std::string_view text, TagType requested) override {
    auto a = std::make_shared<SysAddress>();
    a->text = std::string(text);
    std::string_view s = trim(text);
    auto dot = s.rfind('.');
    if (dot == std::string_view::npos) return make_error(status::BadInvalidArgument, "expected <group>.<item>");
    std::string_view head = trim(s.substr(0, dot));
    const std::string item = upper(trim(s.substr(dot + 1)));
    std::string group;
    if (auto open = head.find('('); open != std::string_view::npos) {
      if (head.back() != ')') return make_error(status::BadInvalidArgument, "missing ')'");
      group = upper(trim(head.substr(0, open)));
      a->arg = std::string(trim(head.substr(open + 1, head.size() - open - 2)));
      if (a->arg.empty()) return make_error(status::BadInvalidArgument, "empty argument");
    } else {
      group = upper(head);
    }
    if (takes_argument(group) != !a->arg.empty())
      return make_error(status::BadInvalidArgument, takes_argument(group) ? "group needs an argument, e.g. Disk(/).Free"
                                                                          : "group takes no argument");
    if ((group == "NETWORK" || group == "THERMAL") && !safe_name(a->arg))
      return make_error(status::BadInvalidArgument, "bad interface or zone name");
    const Spec* spec = nullptr;
    for (const auto& sp : kSpecs)
      if (group == sp.group && item == sp.name) spec = &sp;
    if (spec == nullptr) return make_error(status::BadInvalidArgument, "unknown item (see address help)");
    if (requested.is_array()) return make_error(status::BadTypeMismatch, "arrays are not supported");
    a->item = spec->item;
    const DataType t = requested.scalar == DataType::Default ? spec->type : requested.scalar;
    if ((t == DataType::String) != (spec->type == DataType::String))
      return make_error(status::BadTypeMismatch, "type does not fit the item");
    a->type = TagType{t};
    return a;
  }

  StatusCode connect(Deadline) override {
    if (auto s = slurp("/proc/stat"); s) cpu_prev_ = sysmon::parse_cpu(*s);
    return status::Good;
  }
  void disconnect() override {}
  bool connected() const override { return true; }

  StatusCode read(std::span<ReadItem*> items) override {
    const auto now = Clock::now();
    Snapshot snap;
    for (ReadItem* item : items) {
      const auto& a = static_cast<const SysAddress&>(*item->address);
      auto v = value_of(a, snap);
      if (v) v = convert(*v, a.type.scalar);
      item->quality = v ? status::Good : v.error().code;
      if (v) item->value = std::move(*v);
      item->source_time = now;
    }
    return status::Good;
  }

  StatusCode write(const Address&, const Value&) override { return status::BadNotWritable; }

 private:
  // Files read at most once per read() call.
  struct Snapshot {
    std::optional<std::map<std::string, std::uint64_t, std::less<>>> meminfo;
    std::optional<std::optional<sysmon::LoadAvg>> load;
    std::optional<double> cpu;
  };

  Result<Value> value_of(const SysAddress& a, Snapshot& snap) {
    auto missing = [&](std::string_view what) { return make_error(status::BadOutOfRange, std::string(what)); };
    auto mem = [&](std::string_view key) -> std::optional<std::uint64_t> {
      if (!snap.meminfo) snap.meminfo = sysmon::parse_meminfo(slurp("/proc/meminfo").value_or(""));
      auto it = snap.meminfo->find(key);
      if (it == snap.meminfo->end()) return std::nullopt;
      return it->second;
    };
    auto load = [&]() -> std::optional<sysmon::LoadAvg> {
      if (!snap.load) snap.load = sysmon::parse_loadavg(slurp("/proc/loadavg").value_or(""));
      return *snap.load;
    };
    switch (a.item) {
      case Item::CpuUsage: {
        if (!snap.cpu) {
          auto cur = sysmon::parse_cpu(slurp("/proc/stat").value_or(""));
          if (!cur) return missing("/proc/stat unreadable");
          if (cpu_prev_ && cur->total > cpu_prev_->total) last_usage_ = sysmon::cpu_usage(*cpu_prev_, *cur);
          if (!cpu_prev_ || cur->total > cpu_prev_->total) cpu_prev_ = cur;
          snap.cpu = last_usage_;
        }
        return Value(*snap.cpu);
      }
      case Item::CpuCount: return Value(static_cast<std::uint32_t>(std::max(1u, std::thread::hardware_concurrency())));
      case Item::MemTotal:
      case Item::MemAvailable:
      case Item::MemFree:
      case Item::MemUsed:
      case Item::MemUsedPercent: {
        auto total = mem("MemTotal"), avail = mem("MemAvailable"), free = mem("MemFree");
        if (!total || !avail || !free) return missing("/proc/meminfo unreadable");
        const std::uint64_t used = *total - std::min(*total, *avail);
        if (a.item == Item::MemTotal) return Value(*total);
        if (a.item == Item::MemAvailable) return Value(*avail);
        if (a.item == Item::MemFree) return Value(*free);
        if (a.item == Item::MemUsed) return Value(used);
        return Value(percent(used, *total));
      }
      case Item::SwapTotal:
      case Item::SwapUsed:
      case Item::SwapFree:
      case Item::SwapUsedPercent: {
        auto total = mem("SwapTotal"), free = mem("SwapFree");
        if (!total || !free) return missing("/proc/meminfo unreadable");
        const std::uint64_t used = *total - std::min(*total, *free);
        if (a.item == Item::SwapTotal) return Value(*total);
        if (a.item == Item::SwapFree) return Value(*free);
        if (a.item == Item::SwapUsed) return Value(used);
        return Value(percent(used, *total));
      }
      case Item::Load1:
      case Item::Load5:
      case Item::Load15: {
        auto l = load();
        if (!l) return missing("/proc/loadavg unreadable");
        return Value(a.item == Item::Load1 ? l->one : a.item == Item::Load5 ? l->five : l->fifteen);
      }
      case Item::Uptime: {
        auto s = slurp("/proc/uptime");
        if (!s) return missing("/proc/uptime unreadable");
        try {
          return Value(std::stod(*s));
        } catch (const std::exception&) {
          return missing("/proc/uptime unreadable");
        }
      }
      case Item::Hostname: {
        char buf[256] = {};
        if (gethostname(buf, sizeof buf - 1) != 0) return missing("gethostname failed");
        return Value(std::string(buf));
      }
      case Item::ProcessCount:
      case Item::ProcCount: return Value(count_processes(a.item == Item::ProcCount ? &a.arg : nullptr));
      case Item::DiskTotal:
      case Item::DiskFree:
      case Item::DiskAvailable:
      case Item::DiskUsed:
      case Item::DiskUsedPercent: {
        struct statvfs st{};
        if (statvfs(a.arg.c_str(), &st) != 0) return missing("path not found");
        const std::uint64_t unit = st.f_frsize, total = st.f_blocks * unit, free = st.f_bfree * unit;
        const std::uint64_t avail = st.f_bavail * unit, used = total - std::min(total, free);
        if (a.item == Item::DiskTotal) return Value(total);
        if (a.item == Item::DiskFree) return Value(free);
        if (a.item == Item::DiskAvailable) return Value(avail);
        if (a.item == Item::DiskUsed) return Value(used);
        return Value(percent(used, used + avail));  // as df: share of the space usable by non-root users
      }
      case Item::NetUp: {
        auto s = slurp("/sys/class/net/" + a.arg + "/operstate");
        if (!s) return missing("interface not found");
        return Value(trim(*s) == "up");
      }
      case Item::NetRxBytes:
      case Item::NetTxBytes:
      case Item::NetRxPackets:
      case Item::NetTxPackets:
      case Item::NetRxErrors:
      case Item::NetTxErrors: {
        static constexpr const char* files[] = {"rx_bytes", "tx_bytes", "rx_packets", "tx_packets", "rx_errors", "tx_errors"};
        const auto i = static_cast<std::size_t>(a.item) - static_cast<std::size_t>(Item::NetRxBytes);
        auto v = read_u64("/sys/class/net/" + a.arg + "/statistics/" + files[i]);
        if (!v) return missing("interface not found");
        return Value(*v);
      }
      case Item::Temperature: {
        auto s = slurp("/sys/class/thermal/thermal_zone" + a.arg + "/temp");  // millidegrees, may be negative
        try {
          if (s) return Value(static_cast<double>(std::stoll(*s)) / 1000.0);
        } catch (const std::exception&) {
        }
        return missing("thermal zone not found");
      }
    }
    return make_error(status::BadInternalError, "unreachable");
  }

  static std::uint32_t count_processes(const std::string* name) {
    std::uint32_t n = 0;
    DIR* dir = opendir("/proc");
    if (dir == nullptr) return 0;
    while (dirent* e = readdir(dir)) {
      if (!std::isdigit(static_cast<unsigned char>(e->d_name[0]))) continue;
      if (name == nullptr) {
        ++n;
        continue;
      }
      auto comm = slurp(std::string("/proc/") + e->d_name + "/comm");
      if (comm && trim(*comm) == *name) ++n;
    }
    closedir(dir);
    return n;
  }

  std::optional<sysmon::CpuTimes> cpu_prev_;
  double last_usage_ = 0.0;
};

class SystemMonitorDriver final : public Driver {
 public:
  DriverInfo info() const override { return {"system_monitor", "System Monitor", "0.1.0", Medium::Virtual, {"Linux"}}; }

  nlohmann::json schema() const override {
    return {
        {"driver", "system_monitor"},
        {"display_name", "System Monitor"},
        {"channel", nlohmann::json::array()},
        {"device", nlohmann::json::array()},
        {"address_help",
         "CPU.Usage, CPU.Count, Memory.Total/Available/Used/Free/UsedPercent, Swap.Total/Used/Free/UsedPercent, "
         "Load.1/5/15, System.Uptime/Hostname/ProcessCount, Disk(/path).Total/Free/Available/Used/UsedPercent, "
         "Network(eth0).RxBytes/TxBytes/RxPackets/TxPackets/RxErrors/TxErrors/Up, Process(name).Count, "
         "Thermal(0).Temperature"},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig&, const DeviceConfig&, HostContext&) override {
    return std::make_unique<SysDevice>();
  }
};

}  // namespace

LOGICT_DECLARE_DRIVER(SystemMonitorDriver)
