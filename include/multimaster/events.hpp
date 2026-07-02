// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <functional>
#include <optional>
#include <string>

#include "multimaster/peer_id.hpp"
#include "multimaster/span.hpp"

namespace mm {

enum class error_category {
  Socket,        // a syscall on a socket failed
  Protocol,      // a peer sent something malformed / oversized
  Discovery,     // multicast setup or send/recv failure
  Backpressure,  // an outbound queue overflowed
  Crypto,        // handshake authentication or record decryption failed
  Identity,      // node-identity verification failed (bad sig, id mismatch,
                 // untrusted/missing key)
  Internal,      // a library invariant was violated
};

/// Non-fatal error notification. The mesh keeps running; this is informational
/// (and useful for logging). `peer` is set when the error is attributable to a
/// specific connection.
struct error {
  error_category category;
  int sysErrno;  // errno at point of failure, or 0
  std::string what;
  std::optional<peer_id> peer;
};

/// The complete set of mesh event callbacks. Any field may be left empty.
///
/// THREADING: every callback is invoked on the mesh's single internal IO
/// thread, serialized (never concurrently with each other). callbacks must not
/// block. They MAY call mesh::broadcast / mesh::send. They must NOT call a
/// blocking mesh::stop() from within (that is detected and handled, but avoid
/// it). See Span.hpp for the lifetime rule on the bytes passed to onMessage.
struct callbacks {
  /// A peer was learned via multicast/seed, before any TCP connection.
  std::function<void(peer_id)> onPeerDiscovered;

  /// TCP connection up and handshake completed; the peer is now usable.
  std::function<void(peer_id)> onPeerConnected;

  /// An established connection dropped. The peer may still be on the LAN and
  /// reconnection will be attempted.
  std::function<void(peer_id)> onPeerDisconnected;

  /// The peer stopped announcing and is considered gone; reconnection has
  /// ceased and routing state for it was pruned.
  std::function<void(peer_id)> onPeerLost;

  /// A node became reachable anywhere in the mesh — not necessarily a direct
  /// TCP neighbor. Learned via membership gossip relayed across the mesh
  /// (including over static-peer bridges). A direct neighbor fires both
  /// onPeerConnected and onMemberJoined.
  std::function<void(peer_id)> onMemberJoined;

  /// A node is no longer reachable anywhere in the mesh (its bridge dropped,
  /// it left, or its membership info expired).
  std::function<void(peer_id)> onMemberLeft;

  /// Application data addressed to (or broadcast to) this node. `from` is the
  /// original sender, not necessarily a directly-connected neighbor.
  std::function<void(peer_id /*from*/, bytes)> onMessage;

  /// Non-fatal error notification.
  std::function<void(const error&)> onError;
};

}  // namespace mm
