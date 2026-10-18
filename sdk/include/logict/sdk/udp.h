#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "logict/sdk/driver.h"
#include "logict/sdk/status.h"
#include "logict/sdk/types.h"

namespace logict::sdk {

// Connected UDP socket with deadlines, for drivers running on a channel thread.
// Datagrams from other peers are dropped by the kernel. Calls return Good,
// BadTimeout, BadNotConnected or BadCommunicationError like TcpClient.
class UdpClient {
 public:
  UdpClient() = default;
  ~UdpClient();
  UdpClient(const UdpClient&) = delete;
  UdpClient& operator=(const UdpClient&) = delete;

  // Resolves `host` and fixes the peer address; `local_port` 0 = ephemeral.
  StatusCode open(const std::string& host, std::uint16_t port, std::uint16_t local_port = 0);
  void close() noexcept;
  bool is_open() const noexcept { return fd_ >= 0; }

  StatusCode send(std::span<const std::uint8_t> datagram);
  // Receives one datagram into `buf`; `size` is set to its length (truncated
  // to buf.size()).
  StatusCode recv(std::span<std::uint8_t> buf, std::size_t& size, Deadline deadline);
  // Discards datagrams already queued (stale responses).
  void drain() noexcept;

  // Last octet of the local / peer IPv4 address (FINS node numbers); 0 if unknown.
  std::uint8_t local_ipv4_last_octet() const noexcept;
  std::uint8_t peer_ipv4_last_octet() const noexcept;

  void set_observer(FrameObserver* obs) noexcept { observer_ = obs; }
  const std::string& last_error() const noexcept { return last_error_; }

 private:
  int fd_ = -1;
  FrameObserver* observer_ = nullptr;
  std::string last_error_;
};

}  // namespace logict::sdk
