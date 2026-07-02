// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

  // Compare over the raw bytes. Equivalent to defaulting these on a
  // std::array<std::byte,16>, but spelled explicitly because libc++ (the NDK's
  // standard library) does not synthesise a usable operator<=> for an array of
  // std::byte, which would leave the defaulted spaceship implicitly deleted.
  // memcmp gives the same lexicographic total order (std::byte is unsigned).
  bool operator==(const peer_id& o) const noexcept {
    return std::memcmp(bytes.data(), o.bytes.data(), bytes.size()) == 0;
  }
  std::strong_ordering operator<=>(const peer_id& o) const noexcept {
    const int c = std::memcmp(bytes.data(), o.bytes.data(), bytes.size());
    return c < 0   ? std::strong_ordering::less
           : c > 0 ? std::strong_ordering::greater
                   : std::strong_ordering::equal;
  }

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

}  // namespace mm

template <>
struct std::hash<mm::peer_id> {
  std::size_t operator()(const mm::peer_id& id) const noexcept;
};
