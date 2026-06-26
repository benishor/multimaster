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
    Hello     = 1,
    HelloAck  = 2,
    Heartbeat = 3,
    Data      = 4,
    Goodbye   = 5,
};

/// Body of a hello / HelloAck frame.
struct hello {
    peer_id        nodeId;
    std::uint8_t  protocolVersion = 0;
    std::uint16_t tcpListenPort   = 0;
    std::string   groupName;
    std::uint64_t nonce = 0;
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

/// Result of one decode attempt against a stream buffer.
struct parsed_frame {
    frame_type type{};
    hello     hello_msg; // valid for hello / HelloAck
    data_view  data;     // valid for Data
};

enum class decode_status { NeedMore, Ok, Error };

// --- frame encoders (each returns a complete length-prefixed frame) ---------
std::vector<std::byte> encode_hello(frame_type helloOrAck, const hello&);
std::vector<std::byte> encode_heartbeat();
std::vector<std::byte> encode_goodbye();
/// Encode a Data frame. `payload` is copied into the returned buffer.
std::vector<std::byte> encode_data(const peer_id& src, const peer_id& dst,
                                  const message_id& msgId, std::uint8_t ttl,
                                  std::span<const std::byte> payload);

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
