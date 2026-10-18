#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace logict::test {

// Minimal LS XGT FEnet (dedicated protocol) server for driver tests. Strict on
// framing: company ID, source byte 0x33, header BCC, one data type per
// request, <= 16 blocks, <= 1400 bytes. Each area is 64 KiB of byte memory;
// %TX/%CX contacts and I/Q/U dotted variables are kept separately.
class XgtTestServer {
 public:
  XgtTestServer();
  ~XgtTestServer();

  std::uint16_t port() const { return port_; }

  void set_word(const std::string& area, std::uint32_t word, std::uint16_t v);
  std::uint16_t word(const std::string& area, std::uint32_t word);
  void set_bytes(const std::string& area, std::uint32_t offset, const std::vector<std::uint8_t>& b);
  std::vector<std::uint8_t> bytes(const std::string& area, std::uint32_t offset, std::size_t n);
  bool bit(const std::string& area, std::uint32_t bit_index);
  void set_contact(const std::string& var, bool v);  // e.g. "%TX5"
  bool contact(const std::string& var);
  void set_var(const std::string& var, std::vector<std::uint8_t> data);  // dotted I/Q/U
  // Areas that answer continuous requests with a NAK (to test fallback).
  void reject_continuous(const std::string& area);
  // Byte offsets (in an area) that NAK any access covering them.
  void add_illegal(const std::string& area, std::uint32_t byte_offset);
  void set_delay(std::chrono::milliseconds d) { delay_ms_ = static_cast<int>(d.count()); }

  struct Request {
    std::uint16_t command, type, blocks;
    std::vector<std::string> vars;
    std::uint8_t cpu_info;
  };
  std::vector<Request> requests();
  void reset();
  int framing_errors() const { return framing_errors_; }

 private:
  void run();
  std::vector<std::uint8_t> handle(const std::uint8_t* hdr, const std::vector<std::uint8_t>& body);

  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<int> delay_ms_{0};
  std::atomic<int> framing_errors_{0};
  std::thread thread_;

  std::mutex mu_;
  std::map<std::string, std::vector<std::uint8_t>> mem_;
  std::map<std::string, bool> contacts_;
  std::map<std::string, std::vector<std::uint8_t>> vars_;
  std::set<std::string> reject_continuous_;
  std::set<std::pair<std::string, std::uint32_t>> illegal_;
  std::vector<Request> log_;
};

}  // namespace logict::test
