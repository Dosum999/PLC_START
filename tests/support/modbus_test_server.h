#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace logict::test {

// Minimal Modbus TCP server for driver tests: one client at a time, FC 1-6,
// 15, 16, 22. Registers/coils are plain arrays; addresses in `illegal` answer
// with exception 02.
class ModbusTestServer {
 public:
  ModbusTestServer();
  ~ModbusTestServer();

  std::uint16_t port() const { return port_; }

  void set_holding(std::uint16_t addr, std::uint16_t v);
  std::uint16_t holding(std::uint16_t addr);
  void set_input(std::uint16_t addr, std::uint16_t v);
  void set_coil(std::uint16_t addr, bool v);
  bool coil(std::uint16_t addr);
  void set_discrete(std::uint16_t addr, bool v);
  void add_illegal(std::uint16_t addr);
  // Delay before answering (to provoke timeouts).
  void set_delay(std::chrono::milliseconds d) { delay_ms_ = static_cast<int>(d.count()); }
  // Requests received, by function code.
  int requests(std::uint8_t fc);
  std::vector<std::vector<std::uint8_t>> pdus();
  void reset_counters();
  // Closes the current client connection.
  void drop_client();

 private:
  void run();
  std::vector<std::uint8_t> handle(const std::vector<std::uint8_t>& pdu);

  int listen_fd_ = -1;
  std::atomic<int> client_fd_{-1};
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<int> delay_ms_{0};
  std::thread thread_;

  std::mutex mu_;
  std::vector<std::uint16_t> holding_, input_;
  std::vector<bool> coils_, discrete_;
  std::set<std::uint16_t> illegal_;
  std::vector<std::vector<std::uint8_t>> log_;
};

}  // namespace logict::test
