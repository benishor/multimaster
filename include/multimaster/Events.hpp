#pragma once

#include "multimaster/PeerId.hpp"
#include "multimaster/Span.hpp"

#include <functional>
#include <optional>
#include <string>

namespace mm {

enum class ErrorCategory {
    Socket,       // a syscall on a socket failed
    Protocol,     // a peer sent something malformed / oversized
    Discovery,    // multicast setup or send/recv failure
    Backpressure, // an outbound queue overflowed
    Internal,     // a library invariant was violated
};

/// Non-fatal error notification. The mesh keeps running; this is informational
/// (and useful for logging). `peer` is set when the error is attributable to a
/// specific connection.
struct Error {
    ErrorCategory         category;
    int                   sysErrno; // errno at point of failure, or 0
    std::string           what;
    std::optional<PeerId> peer;
};

/// The complete set of mesh event callbacks. Any field may be left empty.
///
/// THREADING: every callback is invoked on the mesh's single internal IO
/// thread, serialized (never concurrently with each other). Callbacks must not
/// block. They MAY call Mesh::broadcast / Mesh::send. They must NOT call a
/// blocking Mesh::stop() from within (that is detected and handled, but avoid
/// it). See Span.hpp for the lifetime rule on the Bytes passed to onMessage.
struct Callbacks {
    /// A peer was learned via multicast/seed, before any TCP connection.
    std::function<void(PeerId)> onPeerDiscovered;

    /// TCP connection up and handshake completed; the peer is now usable.
    std::function<void(PeerId)> onPeerConnected;

    /// An established connection dropped. The peer may still be on the LAN and
    /// reconnection will be attempted.
    std::function<void(PeerId)> onPeerDisconnected;

    /// The peer stopped announcing and is considered gone; reconnection has
    /// ceased and routing state for it was pruned.
    std::function<void(PeerId)> onPeerLost;

    /// Application data addressed to (or broadcast to) this node. `from` is the
    /// original sender, not necessarily a directly-connected neighbor.
    std::function<void(PeerId /*from*/, Bytes)> onMessage;

    /// Non-fatal error notification.
    std::function<void(const Error&)> onError;
};

} // namespace mm
