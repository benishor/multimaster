#pragma once

#include "multimaster/peer_id.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mm {

/// A static peer hint for environments where multicast is unavailable
/// (containers, some Wi-Fi APs). The node will additionally try to dial these.
struct SeedPeer {
    std::string host; // IPv4 dotted-quad or resolvable hostname
    uint16_t    port; // the peer's TCP listen port
};

/// All tunables for a Mesh node. Defaults are sensible for a small LAN mesh
/// (tens of nodes). Durations use steady_clock semantics internally.
struct MeshConfig {
    using ms = std::chrono::milliseconds;

    // --- identity -----------------------------------------------------------
    PeerId      nodeId          = PeerId::generate();
    std::string groupName       = "default"; // logical mesh segregation
    uint8_t     protocolVersion = 1;

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

    // --- backpressure / safety ---------------------------------------------
    enum class Overflow { Disconnect, DropOldest, DropNewest };
    std::size_t maxOutboundQueueBytes = 8u << 20;  // 8 MiB per peer
    Overflow    overflowPolicy        = Overflow::Disconnect;
    std::size_t maxMessageBytes       = 16u << 20; // reject oversized frames (DoS guard)

    // --- fallback discovery -------------------------------------------------
    std::vector<SeedPeer> seedPeers;
};

} // namespace mm
