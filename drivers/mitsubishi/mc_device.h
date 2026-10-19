#pragma once

// Device logic shared by the Mitsubishi Ethernet (3E/1E) and Serial (3C/4C)
// drivers: request planning, bad-address isolation, value encoding and bit
// read-modify-write. Subclasses implement the transport (read_raw/write_raw)
// with data in the 3E binary layout (words little endian, bits two per byte).

#include <algorithm>
#include <format>
#include <map>
#include <set>

#include "logict/sdk/block_planner.h"
#include "logict/sdk/driver.h"
#include "mc_protocol.h"

namespace logict::mc {

class McDeviceBase : public sdk::DeviceRuntime {
 public:
  McDeviceBase(sdk::HostContext& host, AddressOptions opt, std::uint32_t word_gap, std::uint32_t bit_gap)
      : host_(host), opt_(opt), word_gap_(word_gap), bit_gap_(bit_gap) {}

  sdk::Result<std::shared_ptr<const sdk::Address>> parse_address(std::string_view text, sdk::TagType requested) override {
    auto a = mc::parse_address(text, requested, opt_);
    if (!a) return std::unexpected(a.error());
    if (auto st = check_address(**a); !st) return std::unexpected(st.error());
    return std::shared_ptr<const sdk::Address>(std::move(*a));
  }

  sdk::StatusCode read(std::span<sdk::ReadItem*> items) override {
    namespace status = sdk::status;
    const auto now = sdk::Clock::now();
    std::vector<sdk::PlanItem> plan;
    std::vector<Job> jobs;
    for (std::size_t i = 0; i < items.size(); ++i) {
      const auto& a = addr(*items[i]);
      if (invalid_.contains(&a)) {
        items[i]->quality = status::BadOutOfRange;
        continue;
      }
      const auto [area, start] = plan_key(a);
      if (solo_.contains(&a)) jobs.push_back(Job{area, start, a.points, {i}});
      else plan.push_back({area, start, a.points, i});
    }
    for (auto& b : sdk::plan_blocks(plan, [this](std::uint32_t area) {
           return (area & 1) ? sdk::PlanLimits{max_bits(), bit_gap_} : sdk::PlanLimits{max_words(), word_gap_};
         }))
      jobs.push_back(Job{b.area, static_cast<std::uint32_t>(b.start), b.length, std::move(b.items)});
    return run_jobs(items, std::move(jobs), now);
  }

  sdk::StatusCode write(const sdk::Address& address, const sdk::Value& value) override {
    namespace status = sdk::status;
    const auto& a = static_cast<const McAddress&>(address);
    if (a.bit >= 0) {
      // Bit of a word device: read-modify-write the word.
      auto b = sdk::convert(value, sdk::DataType::Boolean);
      if (!b) return b.error().code;
      std::vector<std::uint8_t> word;
      if (sdk::StatusCode st = read_raw(*a.device, a.number, 1, false, word); !sdk::is_good(st)) return st;
      if (word.size() != 2) return status::BadCommunicationError;
      const std::uint16_t mask = static_cast<std::uint16_t>(1u << a.bit);
      std::uint16_t w = static_cast<std::uint16_t>(word[0] | (word[1] << 8));
      w = *b->get_if<bool>() ? static_cast<std::uint16_t>(w | mask) : static_cast<std::uint16_t>(w & ~mask);
      const std::uint8_t out[2] = {static_cast<std::uint8_t>(w), static_cast<std::uint8_t>(w >> 8)};
      return write_raw(*a.device, a.number, 1, false, out);
    }
    auto data = mc::encode(a, value);
    if (!data) return data.error().code;
    if (a.bit_units) {
      for (std::size_t off = 0; off < data->size(); off += max_bits()) {
        const std::size_t n = std::min<std::size_t>(max_bits(), data->size() - off);
        auto packed = mc::pack_bits(std::span<const std::uint8_t>(data->data() + off, n));
        sdk::StatusCode st =
            write_raw(*a.device, a.number + static_cast<std::uint32_t>(off), static_cast<std::uint16_t>(n), true, packed);
        if (!sdk::is_good(st)) return st;
      }
      return status::Good;
    }
    // Word units (also bit devices accessed as words: 16 points per word).
    const std::uint32_t words = a.points;
    for (std::uint32_t off = 0; off < words; off += max_words()) {
      const std::uint32_t n = std::min(max_words(), words - off);
      const std::uint32_t number = a.number + (a.device->bit ? off * 16 : off);
      sdk::StatusCode st = write_raw(*a.device, number, static_cast<std::uint16_t>(n), false,
                                     std::span<const std::uint8_t>(data->data() + off * 2, n * 2));
      if (!sdk::is_good(st)) return st;
    }
    return status::Good;
  }

 protected:
  // Transport. read_raw: with `end_code` the PLC's end code is reported there
  // and Good returned; without it a non-zero end code becomes the status.
  virtual sdk::StatusCode read_raw(const Device& d, std::uint32_t number, std::uint16_t points, bool bit_units,
                                   std::vector<std::uint8_t>& out, std::uint16_t* end_code = nullptr) = 0;
  virtual sdk::StatusCode write_raw(const Device& d, std::uint32_t number, std::uint16_t points, bool bit_units,
                                    std::span<const std::uint8_t> payload) = 0;
  virtual std::uint32_t max_words() const = 0;
  virtual std::uint32_t max_bits() const = 0;
  virtual sdk::StatusCode end_status(std::uint16_t end_code) const = 0;
  // Extra transport-specific address checks.
  virtual sdk::Result<void> check_address(const McAddress&) const { return {}; }

  // Numbering base of a device in ASCII frames (iQ-F/FX X/Y are octal).
  int base_of(const Device& d) const {
    if (opt_.xy_octal && (d.code == 0x9C || d.code == 0x9D)) return 8;
    return d.base;
  }

  sdk::HostContext& host_;
  AddressOptions opt_;

 private:
  struct Job {
    std::uint32_t area;  // (device code << 1) | bit_units
    std::uint32_t start, length;
    std::vector<std::size_t> items;
  };

  static const McAddress& addr(const sdk::ReadItem& item) { return static_cast<const McAddress&>(*item.address); }

  // Planning space: bit units are device points; word units are words, where a
  // bit device read as words is indexed by number / 16.
  static std::pair<std::uint32_t, std::uint32_t> plan_key(const McAddress& a) {
    const std::uint32_t area = (static_cast<std::uint32_t>(a.device->code) << 1) | (a.bit_units ? 1u : 0u);
    const std::uint32_t start = !a.bit_units && a.device->bit ? a.number / 16 : a.number;
    return {area, start};
  }

  static const Device& device_of(std::uint32_t area) {
    static const std::map<std::uint8_t, const Device*> by_code = [] {
      std::map<std::uint8_t, const Device*> m;
      for (const char* n : {"SM", "SD", "SB", "SW", "DX", "DY", "TS", "TC", "TN", "SS", "SC", "SN", "CS", "CC", "CN", "ZR",
                            "X", "Y", "M", "L", "F", "V", "B", "D", "W", "R", "Z"})
        m[find_device(n)->code] = find_device(n);
      return m;
    }();
    return *by_code.at(static_cast<std::uint8_t>(area >> 1));
  }

  sdk::StatusCode run_jobs(std::span<sdk::ReadItem*> items, std::vector<Job> jobs, sdk::Timestamp now) {
    namespace status = sdk::status;
    std::vector<Job> retry;
    for (auto& job : jobs) {
      const bool bit_units = job.area & 1;
      const auto& dev = device_of(job.area);
      const std::uint32_t number = !bit_units && dev.bit ? job.start * 16 : job.start;
      std::vector<std::uint8_t> raw;
      std::uint16_t end_code = 0;
      sdk::StatusCode st = read_raw(dev, number, static_cast<std::uint16_t>(job.length), bit_units, raw, &end_code);
      if (!sdk::is_good(st)) return st;
      if (end_code == 0) {
        std::vector<std::uint8_t> data;
        if (bit_units) {
          data.resize(job.length);
          for (std::uint32_t i = 0; i < job.length; ++i) data[i] = unpack_bit(raw, i) ? 1 : 0;
        } else {
          data = std::move(raw);
        }
        for (std::size_t idx : job.items) {
          const auto& a = addr(*items[idx]);
          const std::uint32_t rel = plan_key(a).second - job.start;
          auto v = mc::decode(a, std::span(data).subspan(bit_units ? rel : rel * 2));
          items[idx]->quality = v ? status::Good : v.error().code;
          if (v) items[idx]->value = std::move(*v);
          items[idx]->source_time = now;
        }
      } else if (job.items.size() == 1) {
        auto& item = *items[job.items[0]];
        item.quality = end_status(end_code);
        if (item.quality == status::BadOutOfRange) {
          invalid_.insert(&addr(item));
          host_.log.warn("'{}' rejected by PLC (end code 0x{:04X}); tag deactivated", item.address->text, end_code);
        } else {
          host_.log.warn("'{}': PLC end code 0x{:04X}", item.address->text, end_code);
        }
      } else {
        for (std::size_t idx : job.items) {
          const auto& a = addr(*items[idx]);
          solo_.insert(&a);
          const auto [area, start] = plan_key(a);
          retry.push_back(Job{area, start, a.points, {idx}});
        }
      }
    }
    return retry.empty() ? status::Good : run_jobs(items, std::move(retry), now);
  }

  std::uint32_t word_gap_, bit_gap_;
  std::set<const McAddress*> invalid_, solo_;
};

}  // namespace logict::mc
