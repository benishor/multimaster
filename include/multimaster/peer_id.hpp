#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace mm {

/// A 128-bit node identifier, unique per running mesh node. Generated randomly
/// at startup (peer_id::generate) unless the caller pins one in mesh_config.
///
/// The total ordering (operator<=>) is meaningful: it is used as the
/// deterministic tie-break when two nodes dial each other simultaneously.
struct peer_id {
    std::array<std::byte, 16> bytes{};

    bool operator==(const peer_id&) const = default;
    auto operator<=>(const peer_id&) const = default;

    /// Lowercase hex, 32 chars, no separators.
    [[nodiscard]] std::string to_string() const;

    /// Parse 32 hex chars (optionally with dashes, which are ignored).
    /// Returns nullopt on any malformed input.
    [[nodiscard]] static std::optional<peer_id> from_string(std::string_view);

    /// Cryptographically-unpredictable-enough random id (std::random_device).
    [[nodiscard]] static peer_id generate();

    /// The all-zero id, which on the wire denotes "broadcast" as a destination.
    [[nodiscard]] bool is_zero() const;
};

} // namespace mm

template <>
struct std::hash<mm::peer_id> {
    std::size_t operator()(const mm::peer_id& id) const noexcept;
};
