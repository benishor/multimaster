#pragma once

#include "multimaster/peer_id.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mm {

/// A peer endpoint addressed by host + TCP port. Used both for seed peers
/// (one-shot bootstrap hints) and for static peers (persistently maintained
/// connections, typically across the internet).
struct seed_peer {
    std::string host; // IPv4 dotted-quad or resolvable hostname (DNS re-resolved)
    uint16_t    port; // the peer's TCP listen port
};

/// All tunables for a mesh node. Defaults are sensible for a small LAN mesh
/// (tens of nodes). Durations use steady_clock semantics internally.
struct mesh_config {
    using ms = std::chrono::milliseconds;

    // --- identity -----------------------------------------------------------
    peer_id      nodeId          = peer_id::generate();
    std::string groupName       = "default"; // logical mesh segregation
    uint8_t     protocolVersion = 1;

    // Pre-shared key. When non-empty, the mesh is *secured*: peer connections are
    // mutually authenticated and encrypted (X25519 + ChaCha20-Poly1305, per-
    // connection forward secrecy) and discovery announces carry a keyed MAC. All
    // nodes in the mesh must share the same value, and it should be high-entropy
    // (it is hashed to a key, not stretched — do not use a weak password). Empty
    // (the default) keeps the plaintext behavior. Requires the library to be
    // built with libsodium (MULTIMASTER_ENABLE_CRYPTO).
    std::string psk;

    // --- discovery (UDP multicast) -----------------------------------------
    std::string multicastAddr   = "239.255.42.99";
    uint16_t    multicastPort   = 45454;
    std::string multicastIface  = ""; // "" = kernel default; else local NIC IP
    ms          announceInterval{1000};
    ms          peerLostTimeout{5000}; // no announce for this long => Lost
    uint8_t     multicastTtl    = 1;   // 1 = stay on local subnet (intentional)

    // --- TCP transport ------------------------------------------------------
    uint16_t    listenPort      = 0;         // 0 = ephemeral, advertised via announce
    std::string bindAddr        = "0.0.0.0";

    // --- liveness / reconnect ----------------------------------------------
    ms          heartbeatInterval{1000};
    ms          heartbeatTimeout{3000};  // declare dead after this idle period
    ms          handshakeTimeout{2000};
    ms          reconnectBase{500};
    ms          reconnectMax{30000};

    // --- gossip -------------------------------------------------------------
    uint8_t     initialTtl      = 8;
    ms          dedupTtl{30000};
    std::size_t maxDedupEntries = 100'000;
    std::size_t fanout          = 0; // 0 = dial every discovered peer (near-full mesh)

    // --- membership (mesh-wide reachability via adjacency gossip) -----------
    // Each node periodically floods its direct-neighbor set; every node derives
    // the full membership (the connected component reachable from itself) and
    // fires onMemberJoined/onMemberLeft. membershipInterval is the keepalive
    // re-flood period; an entry not refreshed within membershipTimeout expires.
    ms          membershipInterval{2000};
    ms          membershipTimeout{8000};

    // --- backpressure / safety ---------------------------------------------
    enum class overflow { Disconnect, DropOldest, DropNewest };
    std::size_t maxOutboundQueueBytes = 8u << 20;  // 8 MiB per peer
    overflow    overflowPolicy        = overflow::Disconnect;
    std::size_t maxMessageBytes       = 16u << 20; // reject oversized frames (DoS guard)

    // --- fallback discovery -------------------------------------------------
    // Seed peers: bootstrap hints, dialed at startup and re-dialed only while
    // this node is isolated (no established peers). Useful when multicast is
    // unavailable (containers, some Wi-Fi APs).
    std::vector<seed_peer> seedPeers;

    // Static peers: explicit endpoints — typically across the internet, where
    // multicast cannot reach — to which a connection is *persistently*
    // maintained. Dialed at startup, reconnected with backoff whenever the link
    // drops (regardless of LAN mesh state), and never pruned as "lost". The host
    // is DNS-resolved fresh on every attempt, so endpoints behind changing IPs
    // (dynamic DNS, rolling cloud instances) keep working.
    std::vector<seed_peer> staticPeers;
};

} // namespace mm
