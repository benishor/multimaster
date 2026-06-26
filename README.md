# multimaster

A zero-dependency **C++20** library for decentralized distribution of opaque-byte
data between peers on a **LAN**. Nodes discover each other automatically via UDP
multicast, form a self-healing gossip mesh over TCP, reconnect when links drop,
and surface callbacks for every meaningful event.

- **Auto-discovery** — periodic UDP multicast beacons; no central server, no config.
- **Gossip mesh** — messages flood across the mesh with TTL + dedup loop protection.
  Peers need not be fully connected; relays carry traffic onward.
- **Self-healing** — dropped connections reconnect with exponential backoff; dead
  peers are pruned. Dial races are resolved deterministically.
- **Flexible messaging** — send opaque bytes by `broadcast()` (everyone) or
  `send(peerId, …)` (one node, relayed if not directly connected). You serialize
  however you like.
- **Zero runtime dependencies** — raw POSIX sockets + a single epoll IO thread.
  Linux-first. Only `pthreads` is linked.

## Build

```sh
cmake -S . -B build -DMULTIMASTER_BUILD_TESTS=ON -DMULTIMASTER_BUILD_EXAMPLES=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires CMake ≥ 3.20 and a C++20 compiler (tested with GCC 15).

## Use it in your project

Either `add_subdirectory(multimaster)` or install and `find_package`:

```cmake
find_package(multimaster REQUIRED)
target_link_libraries(your_app PRIVATE multimaster::multimaster)
```

## Quick start

```cpp
#include <multimaster/multimaster.hpp>

int main() {
    mm::mesh_config cfg;         // sensible LAN defaults
    cfg.groupName = "my-app";    // only nodes sharing a group join the same mesh

    mm::mesh node(cfg);

    mm::callbacks cb;
    cb.onPeerConnected = [](mm::peer_id p) { /* peer joined */ };
    cb.onPeerLost      = [](mm::peer_id p) { /* peer gone   */ };
    cb.onMessage       = [](mm::peer_id from, mm::bytes data) {
        // `data` is valid only during this callback — copy to retain.
    };
    node.set_callbacks(std::move(cb));

    node.start();                // binds sockets, spawns the IO thread

    const char msg[] = "hello mesh";
    node.broadcast(mm::bytes(reinterpret_cast<const std::byte*>(msg), sizeof msg));

    // ... run your app ...
    node.stop();                 // also called by ~mesh (RAII)
}
```

See [`examples/chat.cpp`](examples/chat.cpp) (LAN broadcast chat) and
[`examples/ping.cpp`](examples/ping.cpp) (targeted send/reply).

## API surface

All public headers live under [`include/multimaster/`](include/multimaster):

| Header            | Contents                                                       |
|-------------------|----------------------------------------------------------------|
| `mesh.hpp`        | `mesh` — the node facade (start/stop, broadcast/send, peers).   |
| `config.hpp`      | `mesh_config`, `seed_peer` — all tunables.                      |
| `events.hpp`      | `callbacks`, `error`, `error_category`.                         |
| `peer_id.hpp`     | `peer_id` — 128-bit node id.                                    |
| `span.hpp`        | `bytes` = `std::span<const std::byte>`.                         |
| `multimaster.hpp` | umbrella header.                                                |

### Threading contract

`broadcast`, `send`, and the introspection accessors are thread-safe and callable
from any thread. **All callbacks run on the mesh's single internal IO thread**,
serialized — never concurrently. Callbacks must not block; they may call
`broadcast`/`send`. The `bytes` handed to `onMessage` is valid only for the
duration of that callback.

### Delivery semantics

Best-effort, unordered, **at-most-once per node** (the dedup cache guarantees no
duplicate delivery). There is no built-in acknowledgement or retransmission — if
you need reliability or ordering, layer it on top of the opaque payload.

## When multicast isn't available

In containers or on some Wi-Fi networks multicast may be blocked. Set
`mesh_config::seedPeers` to a static list of `{host, port}` entries; nodes will
dial those directly in addition to (or instead of) multicast discovery.

## Notes & limits

- **LAN-scoped by design**: multicast TTL defaults to 1 (stays on the local
  subnet). Cross-subnet meshes require `seedPeers`.
- Linux-only (epoll, `accept4`, `eventfd`).
- Tuned for tens of nodes.

## Architecture

For the internals — components, threading model, wire protocol, gossip routing,
and self-healing reconnection — see [docs/architecture.md](docs/architecture.md).
