// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <netinet/in.h>

#include <functional>
#include <random>

#include "event_loop.hpp"
#include "multimaster/config.hpp"
#include "multimaster/events.hpp"
#include "peer_connection.hpp"  // local_identity
#include "socket.hpp"
#include "wire.hpp"

namespace mm {

/// Periodically multicasts an announce datagram and listens for peers'
/// announces on the multicast group. Discovered peers (other than self) are
/// surfaced via the discover callback together with the datagram's source
/// address.
class discovery : public io_handler {
 public:
  using discover_fn =
      std::function<void(const announce&, const sockaddr_in& src)>;
  using error_fn = std::function<void(const error&)>;

  discovery(event_loop& loop, const mesh_config& cfg,
            const local_identity& self, discover_fn onDiscover,
            error_fn onError);
  ~discovery() override;

  /// Set up the multicast socket and begin announcing. Reports (but tolerates)
  /// failures via the error callback rather than throwing, so a node can still
  /// run with seed peers when multicast is unavailable.
  void start();

  void on_io_events(std::uint32_t events) override;

 private:
  void send_announce();
  void schedule_announce();

  event_loop& loop_;
  const mesh_config& cfg_;
  const local_identity& self_;
  discover_fn onDiscover_;
  error_fn onError_;

  socket sock_;
  sockaddr_in groupAddr_{};  // destination for outbound announces
  bool usable_ = false;

  std::mt19937_64 rng_;
};

}  // namespace mm
