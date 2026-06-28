#pragma once

#include "multimaster/peer_id.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mm {

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

/// 128-bit globally-unique-per-message id (originator-derived, no coordination).
struct message_id {
    std::array<std::byte, 16> bytes{};
    bool operator==(const message_id&) const = default;
};

// ---------------------------------------------------------------------------
// discovery datagram (UDP multicast)
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kAnnounceMagic = 0x4D4D4341; // 'M''M''C''A'

/// Announce flag bits.
inline constexpr std::uint8_t kAnnounceFlagSecure = 0x01; // sender runs a secured mesh

struct announce {
    std::uint8_t protocolVersion = 0;
    std::uint8_t flags           = 0;
    std::uint16_t tcpListenPort  = 0;
    peer_id        nodeId;
    std::string   groupName;
};

/// Serialize an announce to a datagram payload.
std::vector<std::byte> encode_announce(const announce&);

/// Parse a datagram. Returns nullopt on bad magic/length/format.
std::optional<announce> decode_announce(std::span<const std::byte>);

// ---------------------------------------------------------------------------
// TCP frames
// ---------------------------------------------------------------------------

enum class frame_type : std::uint8_t {
    Hello          = 1,
    HelloAck       = 2,
    Heartbeat      = 3,
    Data           = 4,
    Goodbye        = 5,
    Membership     = 6, // adjacency gossip for mesh-wide membership
    AuthConfirm    = 7, // PSK handshake confirmation (secured mesh only)
    IdentityRecord = 8, // signed node name gossip (identity mesh only)
};

/// Body of a hello / HelloAck frame. When `secure` is set the frame additionally
/// carries the sender's per-connection X25519 ephemeral public key; when
/// `hasIdentity` is set it also carries the sender's long-lived Ed25519 identity
/// public key and self-declared name (proven by the signature in AuthConfirm).
struct hello {
    peer_id        nodeId;
    std::uint8_t  protocolVersion = 0;
    std::uint16_t tcpListenPort   = 0;
    std::string   groupName;
    std::uint64_t nonce = 0;
    bool                        secure = false;
    std::array<std::byte, 32>   ephPubKey{};
    bool                        hasIdentity = false;
    std::array<std::byte, 32>   idPubKey{};
    std::string                 nodeName;
};

/// A decoded application-data frame. `payload` is a view into the caller's input
/// buffer and is only valid until that buffer is consumed/mutated.
struct data_view {
    peer_id                      src;
    peer_id                      dst; // zero => broadcast
    message_id                   msgId;
    std::uint8_t                ttl = 0;
    std::span<const std::byte>  payload;
};

/// Body of a Membership frame: one node's view of its own direct neighbors,
/// flooded across the mesh so every node can derive the full membership. Ordered
/// per origin by `version` (monotonic; seeded high enough to survive restarts).
struct membership_record {
    peer_id               origin;
    std::uint64_t         version = 0;
    std::vector<peer_id>  neighbors;
};

/// Body of an IdentityRecord frame: a node's signed self-declared name, flooded
/// across the mesh so every node can learn (and verify) names of non-direct
/// members. Ordered per origin by `version` (monotonic, like membership). The
/// signature is over "mm-idrec-v1" || version (8B big-endian) || name, verifiable
/// against `idPubKey`; the origin's peer_id is the hash of `idPubKey`.
struct identity_record {
    std::array<std::byte, 32> idPubKey{};
    std::uint64_t             version = 0;
    std::string               name;
    std::array<std::byte, 64> signature{};
};

/// Result of one decode attempt against a stream buffer.
struct parsed_frame {
    frame_type                type{};
    hello                     hello_msg;  // valid for hello / HelloAck
    data_view                 data;       // valid for Data
    membership_record         membership; // valid for Membership
    std::array<std::byte, 32> auth_tag{};      // valid for AuthConfirm (PSK tag)
    bool                      has_auth_sig = false; // AuthConfirm carries an identity sig
    std::array<std::byte, 64> auth_sig{};       // valid for AuthConfirm when has_auth_sig
    identity_record           identity;         // valid for IdentityRecord
};

enum class decode_status { NeedMore, Ok, Error };

// --- frame encoders (each returns a complete length-prefixed frame) ---------
std::vector<std::byte> encode_hello(frame_type helloOrAck, const hello&);
std::vector<std::byte> encode_heartbeat();
std::vector<std::byte> encode_goodbye();
/// Encode an AuthConfirm frame carrying the 32-byte PSK confirmation tag and,
/// when `hasSig`, a 64-byte Ed25519 identity signature over the handshake.
std::vector<std::byte> encode_auth_confirm(const std::array<std::byte, 32>& tag,
                                          bool hasSig = false,
                                          const std::array<std::byte, 64>& sig = {});
/// Encode a Data frame. `payload` is copied into the returned buffer.
std::vector<std::byte> encode_data(const peer_id& src, const peer_id& dst,
                                  const message_id& msgId, std::uint8_t ttl,
                                  std::span<const std::byte> payload);
/// Encode a Membership frame from an adjacency record.
std::vector<std::byte> encode_membership(const membership_record&);
/// Encode an IdentityRecord (signed node-name gossip) frame.
std::vector<std::byte> encode_identity(const identity_record&);

/// Attempt to decode one frame from the front of `in`.
///  - NeedMore: not enough bytes yet; `consumed` untouched.
///  - Ok: `out` filled, `consumed` set to the frame's total byte length.
///  - error: malformed or exceeds maxMessageBytes; caller should drop the peer.
decode_status try_decode_frame(std::span<const std::byte> in, std::size_t maxMessageBytes,
                            parsed_frame& out, std::size_t& consumed);

} // namespace mm

template <>
struct std::hash<mm::message_id> {
    std::size_t operator()(const mm::message_id& id) const noexcept;
};
