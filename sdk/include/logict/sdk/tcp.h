#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "logict/sdk/driver.h"
#include "logict/sdk/status.h"
#include "logict/sdk/types.h"

namespace logict::sdk {

// Blocking TCP client with deadlines, for drivers running on a channel thread.
// Every call returns Good, BadTimeout (deadline passed), BadNotConnected
// (no connection / peer closed) or BadCommunicationError (socket error).
class TcpClient {
 public:
  TcpClient() = default;
  ~TcpClient();
  TcpClient(const TcpClient&) = delete;
  TcpClient& operator=(const TcpClient&) = delete;

  StatusCode connect(const std::string& host, std::uint16_t port, Deadline deadline);
  void close() noexcept;
  bool is_open() const noexcept { return fd_ >= 0; }

  StatusCode send_all(std::span<const std::uint8_t> data, Deadline deadline);
  // Reads exactly data.size() bytes.
  StatusCode recv_exact(std::span<std::uint8_t> data, Deadline deadline);
  // Reads whatever is available (at least one byte) into `data`; `size` is
  // set to the number of bytes read.
  StatusCode recv_some(std::span<std::uint8_t> data, std::size_t& size, Deadline deadline);
  // Discards anything already buffered by the kernel (stale responses).
  void drain() noexcept;

  // Optional frame observer for communication diagnostics / byte counters.
  void set_observer(FrameObserver* obs) noexcept { observer_ = obs; }
  const std::string& last_error() const noexcept { return last_error_; }

 private:
  StatusCode wait(short events, Deadline deadline);

  int fd_ = -1;
  FrameObserver* observer_ = nullptr;
  std::string last_error_;
};

}  // namespace logict::sdk
