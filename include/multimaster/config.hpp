// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "multimaster/peer_id.hpp"

namespace mm {

/// A peer endpoint addressed by host + TCP port. Used both for seed peers
/// (one-shot bootstrap hints) and for static peers (persistently maintained
/// connections, typically across the internet).
struct seed_peer {
  std::string
      host;       // IPv4 dotted-quad or resolvable hostname (DNS re-resolved)
  uint16_t port;  // the peer's TCP listen port
};

/// An entry in the node-identity allowlist: an Ed25519 public key (64 hex
/// chars) this node is willing to admit, plus an optional human-readable local
/// label that overrides whatever name the keyholder advertises.
struct trusted_node {
  std::string publicKeyHex;  // 64-hex-char Ed25519 public key
  std::string label;         // optional local display label (may be empty)
};

/// All tunables for a mesh node. Defaults are sensible for a small LAN mesh
/// (tens of nodes). Durations use steady_clock semantics internally.
struct mesh_config {
  using ms = std::chrono::milliseconds;

  // --- identity -----------------------------------------------------------
  peer_id nodeId = peer_id::generate();
  std::string groupName = "default";  // logical mesh segregation
  uint8_t protocolVersion = 1;

  // Pre-shared key. When non-empty, the mesh is *secured*: peer connections are
  // mutually authenticated and encrypted (X25519 + ChaCha20-Poly1305, per-
  // connection forward secrecy) and discovery announces carry a keyed MAC. All
  // nodes in the mesh must share the same value, and it should be high-entropy
  // (it is hashed to a key, not stretched — do not use a weak password). Empty
  // (the default) keeps the plaintext behavior. Requires the library to be
  // built with libsodium (MULTIMASTER_ENABLE_CRYPTO).
  std::string psk;

  // --- node identity (self-certifying; complements the PSK) ---------------
  // When an identity is configured this node gets a long-lived Ed25519 keypair
  // and its `nodeId` becomes a hash of the public key, so the id *proves*
  // ownership of the key — peers can no longer impersonate one another.
  // Identity is layered on the secured handshake and therefore requires a
  // non-empty `psk` (a secured mesh); configuring it without one is a
  // start-time error.

  // Path to this node's identity seed (32 bytes, stored hex, mode 0600).
  // Loaded if present, otherwise generated and written. Non-empty enables
  // identity. The derived nodeId persists across restarts.
  std::string identityFile;
  // Inline identity seed (64 hex chars). Overrides identityFile when set;
  // convenient for tests and ephemeral nodes. Also enables identity.
  std::string identitySeedHex;

  // This node's self-declared nickname, signed by its identity key and gossiped
  // across the mesh. Advisory only (not unique); nodeId stays canonical.
  std::string nodeName;

  // Allowlist of admissible node public keys (+ optional local labels). When
  // non-empty, only peers whose identity key is listed are admitted — removing
  // a key revokes that node without rotating the PSK. Empty ⇒ any peer with a
  // valid (self-consistent, signed) identity is accepted.
  std::vector<trusted_node> trustedKeys;

  // When this node has an identity, reject peers that present none. Set false
  // to allow a mixed/transitional mesh of identity and legacy nodes.
  bool requireIdentity = true;

  // --- discovery (UDP multicast) -----------------------------------------
  std::string multicastAddr = "239.255.42.99";
  uint16_t multicastPort = 45454;
  std::string multicastIface = "";  // "" = kernel default; else local NIC IP
  ms announceInterval{1000};
  ms peerLostTimeout{5000};  // no announce for this long => Lost
  uint8_t multicastTtl = 1;  // 1 = stay on local subnet (intentional)

  // --- TCP transport ------------------------------------------------------
  uint16_t listenPort = 0;  // 0 = ephemeral, advertised via announce
  std::string bindAddr = "0.0.0.0";

  // --- liveness / reconnect ----------------------------------------------
  ms heartbeatInterval{1000};
  ms heartbeatTimeout{3000};  // declare dead after this idle period
  ms handshakeTimeout{2000};
  ms reconnectBase{500};
  ms reconnectMax{30000};

  // --- gossip -------------------------------------------------------------
  uint8_t initialTtl = 8;
  ms dedupTtl{30000};
  std::size_t maxDedupEntries = 100'000;
  std::size_t fanout = 0;  // 0 = dial every discovered peer (near-full mesh)

  // --- membership (mesh-wide reachability via adjacency gossip) -----------
  // Each node periodically floods its direct-neighbor set; every node derives
  // the full membership (the connected component reachable from itself) and
  // fires onMemberJoined/onMemberLeft. membershipInterval is the keepalive
  // re-flood period; an entry not refreshed within membershipTimeout expires.
  ms membershipInterval{2000};
  ms membershipTimeout{8000};

  // --- backpressure / safety ---------------------------------------------
  enum class overflow { Disconnect, DropOldest, DropNewest };
  std::size_t maxOutboundQueueBytes = 8u << 20;  // 8 MiB per peer
  overflow overflowPolicy = overflow::Disconnect;
  std::size_t maxMessageBytes = 16u
                                << 20;  // reject oversized frames (DoS guard)

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

}  // namespace mm
