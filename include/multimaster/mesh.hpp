#pragma once

#include "multimaster/config.hpp"
#include "multimaster/events.hpp"
#include "multimaster/peer_id.hpp"
#include "multimaster/span.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace mm {

class MeshImpl; // pimpl; keeps <sys/epoll.h> and sockets out of the public ABI

/// A node in a decentralized LAN mesh.
///
/// Lifecycle: construct with a MeshConfig, optionally setCallbacks(), then
/// start(). start() spawns a single internal IO thread that owns all sockets.
/// stop() (also called by the destructor) shuts that thread down gracefully.
///
/// All send methods and introspection accessors are thread-safe. Callbacks run
/// on the IO thread — see Callbacks for the full contract.
class Mesh {
public:
    explicit Mesh(MeshConfig cfg);
    ~Mesh();

    Mesh(Mesh&&) noexcept;
    Mesh& operator=(Mesh&&) noexcept;
    Mesh(const Mesh&)            = delete;
    Mesh& operator=(const Mesh&) = delete;

    /// Install event handlers. Must be called before start(); changing
    /// callbacks on a running mesh is not supported.
    void setCallbacks(Callbacks cb);

    /// Bind sockets and spawn the IO thread. Throws std::system_error on bind
    /// failure. Idempotent: a second call while running is a no-op.
    void start();

    /// Gracefully send Goodbye to peers, stop the IO thread, and join it.
    /// Idempotent and safe to call from the destructor. If invoked from within
    /// a callback (i.e. on the IO thread), the stop is scheduled asynchronously
    /// rather than self-joining.
    void stop();

    [[nodiscard]] bool isRunning() const noexcept;

    /// Flood opaque bytes to the entire mesh. Thread-safe; copies the payload.
    void broadcast(Bytes data);

    /// Deliver opaque bytes to a specific node (relayed across the mesh if not
    /// directly connected). Thread-safe; copies the payload.
    void send(PeerId dst, Bytes data);

    [[nodiscard]] PeerId   id() const noexcept;
    /// The actual bound TCP port (meaningful after start(), resolves ephemeral).
    [[nodiscard]] uint16_t listenPort() const noexcept;

    /// Non-blocking snapshots of mesh state.
    [[nodiscard]] std::vector<PeerId> connectedPeers() const;
    [[nodiscard]] std::vector<PeerId> knownPeers() const;

private:
    std::unique_ptr<MeshImpl> impl_;
};

} // namespace mm
