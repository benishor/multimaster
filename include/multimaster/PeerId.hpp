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
/// at startup (PeerId::generate) unless the caller pins one in MeshConfig.
///
/// The total ordering (operator<=>) is meaningful: it is used as the
/// deterministic tie-break when two nodes dial each other simultaneously.
struct PeerId {
    std::array<std::byte, 16> bytes{};

    bool operator==(const PeerId&) const = default;
    auto operator<=>(const PeerId&) const = default;

    /// Lowercase hex, 32 chars, no separators.
    [[nodiscard]] std::string toString() const;

    /// Parse 32 hex chars (optionally with dashes, which are ignored).
    /// Returns nullopt on any malformed input.
    [[nodiscard]] static std::optional<PeerId> fromString(std::string_view);

    /// Cryptographically-unpredictable-enough random id (std::random_device).
    [[nodiscard]] static PeerId generate();

    /// The all-zero id, which on the wire denotes "broadcast" as a destination.
    [[nodiscard]] bool isZero() const;
};

} // namespace mm

template <>
struct std::hash<mm::PeerId> {
    std::size_t operator()(const mm::PeerId& id) const noexcept;
};
