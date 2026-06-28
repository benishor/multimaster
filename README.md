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
- **Optional encryption** — set a pre-shared key (`mesh_config::psk`) to mutually
  authenticate peers and encrypt all traffic (X25519 + ChaCha20-Poly1305, with
  per-connection forward secrecy). Off by default; see [Security](#security).
- **Minimal dependencies** — raw POSIX sockets + a single epoll IO thread.
  Linux-first. Only `pthreads` is linked, plus **libsodium** when encryption is
  enabled (the default build; it can be turned off, see [Security](#security)).

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

`multimaster` builds as a **static** library (`libmultimaster.a`), so it is
linked directly into your binary — no shared object to ship alongside it.

### Portable (statically linked) executables

To produce binaries you can copy to other machines/distros, configure with
`-DMULTIMASTER_STATIC_BINARIES=ON`. This links `libstdc++`/`libgcc` statically
into the example executables (and the same helper is available for your own
targets) while keeping `glibc` dynamic — so hostname resolution for
seed/static peers (`getaddrinfo`) keeps working:

```sh
cmake -S . -B build -DMULTIMASTER_STATIC_BINARIES=ON
cmake --build build
ldd build/mm_chat   # no libstdc++.so / libgcc_s.so — only libc remains
```

> A fully static binary (`-static`) is intentionally not the default: glibc's
> `getaddrinfo`/NSS does not work reliably when statically linked, which would
> break dialing static/seed peers by hostname (numeric IPs would still work).

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

### Direct peers vs mesh members

The library distinguishes your **direct TCP neighbors** from the **whole-mesh
membership**:

- `onPeerConnected` / `onPeerDisconnected` / `onPeerLost` and `connected_peers()`
  concern only the nodes you are *directly* connected to.
- `onMemberJoined` / `onMemberLeft` and `members()` concern **every node
  reachable anywhere in the mesh**, including ones you only reach through relays
  or a static-peer bridge. This is learned via lightweight *membership gossip*:
  each node periodically floods its direct-neighbor set, every node derives the
  reachable component, and a member is dropped when its only path disappears or
  its info expires (`membershipInterval` / `membershipTimeout`).

So in `B —— A —— C` (A bridges B and C), B fires `onPeerConnected(A)` but
`onMemberJoined(A)` *and* `onMemberJoined(C)`; `members()` on B returns `{A, C}`.

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

## Connecting beyond the LAN

Multicast discovery only covers the local subnet (and is often blocked in
containers or on some Wi-Fi networks). Two config fields let you reach peers by
explicit `{host, port}` — including across the internet. Both DNS-resolve the
host fresh on every dial attempt, so endpoints behind changing IPs keep working.

- **`seedPeers`** — *bootstrap hints*. Dialed at startup and re-dialed only while
  this node is isolated (has no established peers). Use these to break into a mesh
  when multicast isn't available.

- **`staticPeers`** — *persistent connections*. Dialed at startup and
  **continuously maintained**: if the link drops it reconnects with backoff
  regardless of LAN mesh state, and the peer is never pruned as "lost". Use these
  for stable, always-on peers reachable over the internet.

```cpp
mm::mesh_config cfg;
cfg.staticPeers.push_back({"mesh.example.com", 45000}); // always keep this link up
cfg.seedPeers.push_back({"10.0.0.5", 45000});           // bootstrap fallback only
```

A peer found both via multicast and via a static/seed entry is de-duplicated
automatically (the connection-handshake logic keeps a single link per node).

## Security

By default the mesh is **unauthenticated and unencrypted** — any host that can
reach a node (on the subnet, or a static peer's TCP port) may join and read or
inject traffic. To lock the mesh down, give every node the **same pre-shared
key**:

```cpp
mm::mesh_config cfg;
cfg.psk = "a-high-entropy-shared-secret"; // same on every node
```

With a non-empty `psk`:

- **Mutual authentication** — each connection runs an X25519 key exchange bound
  to the PSK; a peer that doesn't hold the key fails the handshake and is dropped
  (`error_category::Crypto`). Mismatched secured/plaintext nodes refuse to pair.
- **Encryption + integrity** — every frame is sealed with ChaCha20-Poly1305.
  Ephemeral per-connection keys give **forward secrecy**.
- **Authenticated discovery** — multicast announces carry a keyed MAC, so peers
  without the key can't forge them or enumerate the mesh.

The PSK is hashed to a key, not stretched — use a high-entropy secret, and share
it with every node (it complements `groupName`, which only segregates meshes and
is *not* a secret). Key rotation is manual: change the PSK everywhere and restart.

Encryption requires **libsodium**. It is found automatically and enabled by
default (`-DMULTIMASTER_ENABLE_CRYPTO=ON`). Build with
`-DMULTIMASTER_ENABLE_CRYPTO=OFF` to drop the dependency entirely; a mesh
configured with a `psk` then refuses to start.

## Notes & limits

- **LAN-scoped discovery by design**: multicast TTL defaults to 1 (stays on the
  local subnet). Reach other subnets / the internet via `seedPeers` /
  `staticPeers`.
- Hostname resolution for seed/static peers is synchronous on the IO thread;
  endpoints are few and dialed only at startup / on reconnect intervals.
- Linux-only (epoll, `accept4`, `eventfd`).
- Tuned for tens of nodes.

## Architecture & internals

- [docs/architecture.md](docs/architecture.md) — the design tour: components,
  threading model, wire protocol, gossip routing, and self-healing reconnection.
- [docs/how-the-mesh-works.md](docs/how-the-mesh-works.md) — the complete,
  byte- and algorithm-level reference: every wire field, routing pseudocode,
  timing tables, worked end-to-end examples, and a failure-scenario catalog.
