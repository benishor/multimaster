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
struct MessageId {
    std::array<std::byte, 16> bytes{};
    bool operator==(const MessageId&) const = default;
};

// ---------------------------------------------------------------------------
// Discovery datagram (UDP multicast)
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kAnnounceMagic = 0x4D4D4341; // 'M''M''C''A'

struct Announce {
    std::uint8_t protocolVersion = 0;
    std::uint8_t flags           = 0;
    std::uint16_t tcpListenPort  = 0;
    PeerId        nodeId;
    std::string   groupName;
};

/// Serialize an Announce to a datagram payload.
std::vector<std::byte> encodeAnnounce(const Announce&);

/// Parse a datagram. Returns nullopt on bad magic/length/format.
std::optional<Announce> decodeAnnounce(std::span<const std::byte>);

// ---------------------------------------------------------------------------
// TCP frames
// ---------------------------------------------------------------------------

enum class FrameType : std::uint8_t {
    Hello     = 1,
    HelloAck  = 2,
    Heartbeat = 3,
    Data      = 4,
    Goodbye   = 5,
};

/// Body of a Hello / HelloAck frame.
struct Hello {
    PeerId        nodeId;
    std::uint8_t  protocolVersion = 0;
    std::uint16_t tcpListenPort   = 0;
    std::string   groupName;
    std::uint64_t nonce = 0;
};

/// A decoded application-data frame. `payload` is a view into the caller's input
/// buffer and is only valid until that buffer is consumed/mutated.
struct DataView {
    PeerId                      src;
    PeerId                      dst; // zero => broadcast
    MessageId                   msgId;
    std::uint8_t                ttl = 0;
    std::span<const std::byte>  payload;
};

/// Result of one decode attempt against a stream buffer.
struct ParsedFrame {
    FrameType type{};
    Hello     hello;    // valid for Hello / HelloAck
    DataView  data;     // valid for Data
};

enum class DecodeStatus { NeedMore, Ok, Error };

// --- frame encoders (each returns a complete length-prefixed frame) ---------
std::vector<std::byte> encodeHello(FrameType helloOrAck, const Hello&);
std::vector<std::byte> encodeHeartbeat();
std::vector<std::byte> encodeGoodbye();
/// Encode a Data frame. `payload` is copied into the returned buffer.
std::vector<std::byte> encodeData(const PeerId& src, const PeerId& dst,
                                  const MessageId& msgId, std::uint8_t ttl,
                                  std::span<const std::byte> payload);

/// Attempt to decode one frame from the front of `in`.
///  - NeedMore: not enough bytes yet; `consumed` untouched.
///  - Ok: `out` filled, `consumed` set to the frame's total byte length.
///  - Error: malformed or exceeds maxMessageBytes; caller should drop the peer.
DecodeStatus tryDecodeFrame(std::span<const std::byte> in, std::size_t maxMessageBytes,
                            ParsedFrame& out, std::size_t& consumed);

} // namespace mm

template <>
struct std::hash<mm::MessageId> {
    std::size_t operator()(const mm::MessageId& id) const noexcept;
};
