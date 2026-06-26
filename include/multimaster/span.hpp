#pragma once

#include <cstddef>
#include <span>

namespace mm {

/// Opaque, read-only view of bytes handed to/from the mesh. The library never
/// interprets payload contents; serialization is entirely up to the caller.
///
/// NOTE on lifetime: a Bytes passed into a callback (e.g. Callbacks::onMessage)
/// views the IO thread's internal read buffer and is valid ONLY for the
/// duration of that callback. Copy it if you need to retain the data.
using Bytes = std::span<const std::byte>;

} // namespace mm
