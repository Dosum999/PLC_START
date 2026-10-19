#pragma once

// Device logic shared by the CIMON Serial and Ethernet drivers: request
// planning, bad-address isolation and bit read-modify-write. Subclasses
// implement the transport.

#include <algorithm>
#include <set>

#include "cimon_protocol.h"
#include "logict/sdk/block_planner.h"
#include "logict/sdk/driver.h"

namespace logict::cimon {

struct Limits {
  std::uint32_t read_words, write_words, read_bits, write_bits;
};

class CimonDeviceBase : public sdk::DeviceRuntime {
 public:
  CimonDeviceBase(sdk::HostContext& host, Limits limits, std::uint32_t word_gap, std::uint32_t bit_gap)
      : host_(host), limits_(limits), word_gap_(word_gap), bit_gap_(bit_gap) {}

  sdk::Result<std::shared_ptr<const sdk::Address>> parse_address(std::string_view text, sdk::TagType requested) override {
    auto a = cimon::parse_address(text, requested);
    if (!a) return std::unexpected(a.error());
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
      if (solo_.contains(&a)) jobs.push_back(Job{area_of(a), a.start, a.points, {i}});
      else plan.push_back({area_of(a), a.start, a.points, i});
    }
    for (auto& b : sdk::plan_blocks(plan, [this](std::uint32_t area) {
           return (area & 1) ? sdk::PlanLimits{limits_.read_bits, bit_gap_} : sdk::PlanLimits{limits_.read_words, word_gap_};
         }))
      jobs.push_back(Job{b.area, static_cast<std::uint32_t>(b.start), b.length, std::move(b.items)});
    return run_jobs(items, std::move(jobs), now);
  }

  sdk::StatusCode write(const sdk::Address& address, const sdk::Value& value) override {
    const auto& a = static_cast<const CimonAddress&>(address);
    if (a.bit >= 0) {
      // Bit of a word device: read-modify-write the word.
      auto b = sdk::convert(value, sdk::DataType::Boolean);
      if (!b) return b.error().code;
      Answer ans;
      if (sdk::StatusCode st = read_raw(*a.device, false, a.start, 1, ans); !sdk::is_good(st)) return st;
      if (ans.error != 0) return error_status(ans.error);
      std::uint16_t w = static_cast<std::uint16_t>(ans.data[0] << 8 | ans.data[1]);
      const std::uint16_t mask = static_cast<std::uint16_t>(1u << a.bit);
      w = *b->get_if<bool>() ? static_cast<std::uint16_t>(w | mask) : static_cast<std::uint16_t>(w & ~mask);
      const std::uint8_t raw[2] = {static_cast<std::uint8_t>(w >> 8), static_cast<std::uint8_t>(w)};
      return checked_write(a, a.start, raw);
    }
    auto raw = cimon::encode(a, value);
    if (!raw) return raw.error().code;
    const std::size_t unit = a.bit_units ? 1 : 2;
    const std::uint32_t limit = a.bit_units ? limits_.write_bits : limits_.write_words;
    const std::uint32_t total = static_cast<std::uint32_t>(raw->size() / unit);
    for (std::uint32_t off = 0; off < total; off += limit) {
      const std::uint32_t n = std::min(limit, total - off);
      if (sdk::StatusCode st = checked_write(a, a.start + off, std::span(raw->data() + off * unit, n * unit)); !sdk::is_good(st))
        return st;
    }
    return sdk::status::Good;
  }

 protected:
  // Transport. `out.error` carries the PLC's error code (0 = success).
  virtual sdk::StatusCode read_raw(const Device& d, bool bit_units, std::uint32_t start, std::uint32_t count, Answer& out) = 0;
  virtual sdk::StatusCode write_raw(const Device& d, bool bit_units, std::uint32_t start, std::span<const std::uint8_t> raw,
                                    int& error) = 0;
  virtual sdk::StatusCode error_status(int code) const = 0;

  sdk::HostContext& host_;

 private:
  struct Job {
    std::uint32_t area;  // device index << 1 | bit_units
    std::uint32_t start, length;
    std::vector<std::size_t> items;
  };

  static const CimonAddress& addr(const sdk::ReadItem& item) { return static_cast<const CimonAddress&>(*item.address); }
  static std::uint32_t area_of(const CimonAddress& a) { return device_index(*a.device) << 1 | (a.bit_units ? 1u : 0u); }

  sdk::StatusCode checked_write(const CimonAddress& a, std::uint32_t start, std::span<const std::uint8_t> raw) {
    int error = 0;
    if (sdk::StatusCode st = write_raw(*a.device, a.bit_units, start, raw, error); !sdk::is_good(st)) return st;
    if (error != 0) {
      host_.log.warn("write to '{}' rejected (error {})", a.text, error);
      return error_status(error);
    }
    return sdk::status::Good;
  }

  sdk::StatusCode run_jobs(std::span<sdk::ReadItem*> items, std::vector<Job> jobs, sdk::Timestamp now) {
    namespace status = sdk::status;
    std::vector<Job> retry;
    for (auto& job : jobs) {
      const bool bit_units = job.area & 1;
      Answer ans;
      if (sdk::StatusCode st = read_raw(device_at(job.area >> 1), bit_units, job.start, job.length, ans); !sdk::is_good(st))
        return st;
      if (ans.error == 0) {
        const std::size_t unit = bit_units ? 1 : 2;
        for (std::size_t idx : job.items) {
          const auto& a = addr(*items[idx]);
          auto v = cimon::decode(a, std::span(ans.data).subspan((a.start - job.start) * unit));
          items[idx]->quality = v ? status::Good : v.error().code;
          if (v) items[idx]->value = std::move(*v);
          items[idx]->source_time = now;
        }
      } else if (job.items.size() == 1) {
        auto& item = *items[job.items[0]];
        item.quality = error_status(ans.error);
        if (item.quality == status::BadOutOfRange) {
          invalid_.insert(&addr(item));
          host_.log.warn("'{}' rejected by PLC (error {}); tag deactivated", item.address->text, ans.error);
        } else {
          host_.log.warn("'{}': PLC error {}", item.address->text, ans.error);
        }
      } else {
        for (std::size_t idx : job.items) {
          const auto& a = addr(*items[idx]);
          solo_.insert(&a);
          retry.push_back(Job{area_of(a), a.start, a.points, {idx}});
        }
      }
    }
    return retry.empty() ? status::Good : run_jobs(items, std::move(retry), now);
  }

  Limits limits_;
  std::uint32_t word_gap_, bit_gap_;
  std::set<const CimonAddress*> invalid_, solo_;
};

}  // namespace logict::cimon
