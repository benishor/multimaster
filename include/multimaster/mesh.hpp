#pragma once

#include "multimaster/config.hpp"
#include "multimaster/events.hpp"
#include "multimaster/peer_id.hpp"
#include "multimaster/span.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace mm {

class mesh_impl; // pimpl; keeps <sys/epoll.h> and sockets out of the public ABI

/// A node in a decentralized LAN mesh.
///
/// Lifecycle: construct with a mesh_config, optionally set_callbacks(), then
/// start(). start() spawns a single internal IO thread that owns all sockets.
/// stop() (also called by the destructor) shuts that thread down gracefully.
///
/// All send methods and introspection accessors are thread-safe. callbacks run
/// on the IO thread — see callbacks for the full contract.
class mesh {
public:
    explicit mesh(mesh_config cfg);
    ~mesh();

    mesh(mesh&&) noexcept;
    mesh& operator=(mesh&&) noexcept;
    mesh(const mesh&)            = delete;
    mesh& operator=(const mesh&) = delete;

    /// Install event handlers. Must be called before start(); changing
    /// callbacks on a running mesh is not supported.
    void set_callbacks(callbacks cb);

    /// Bind sockets and spawn the IO thread. Throws std::system_error on bind
    /// failure. Idempotent: a second call while running is a no-op.
    void start();

    /// Gracefully send Goodbye to peers, stop the IO thread, and join it.
    /// Idempotent and safe to call from the destructor. If invoked from within
    /// a callback (i.e. on the IO thread), the stop is scheduled asynchronously
    /// rather than self-joining.
    void stop();

    [[nodiscard]] bool is_running() const noexcept;

    /// Flood opaque bytes to the entire mesh. Thread-safe; copies the payload.
    void broadcast(bytes data);

    /// Deliver opaque bytes to a specific node (relayed across the mesh if not
    /// directly connected). Thread-safe; copies the payload.
    void send(peer_id dst, bytes data);

    [[nodiscard]] peer_id   id() const noexcept;
    /// The actual bound TCP port (meaningful after start(), resolves ephemeral).
    [[nodiscard]] uint16_t listen_port() const noexcept;

    /// Non-blocking snapshots of mesh state.
    /// connected_peers() — directly connected TCP neighbors.
    /// known_peers()     — peers this node has directly learned of.
    /// members()         — every node reachable anywhere in the mesh, learned via
    ///                     membership gossip (includes peers reachable only via
    ///                     relays / static-peer bridges; not direct neighbors).
    [[nodiscard]] std::vector<peer_id> connected_peers() const;
    [[nodiscard]] std::vector<peer_id> known_peers() const;
    [[nodiscard]] std::vector<peer_id> members() const;

private:
    std::unique_ptr<mesh_impl> impl_;
};

} // namespace mm
