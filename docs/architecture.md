# multimaster — Architecture

This document explains how `multimaster` is built: its components, the threading
model, the wire protocol, and the algorithms behind discovery, the gossip mesh,
and self-healing reconnection. It complements the user-facing [README](../README.md),
which covers the public API and how to consume the library.

---

## 1. Goals & constraints

`multimaster` distributes opaque-byte messages between peers on a LAN.

- **Decentralized** — no broker, no coordinator. Every node is identical.
- **Self-organizing** — peers discover each other and form a mesh automatically.
- **Self-healing** — dropped links reconnect; vanished peers are pruned.
- **Zero runtime dependencies** — raw POSIX sockets, a single `epoll` loop, the
  C++20 standard library, and `pthreads`. Linux-first.

These constraints drive every design choice below. In particular, the
"single IO thread + everything else is single-threaded state" model (§3) is what
keeps a lock-free, dependency-free implementation tractable and correct.

---

## 2. Component map

The public surface lives in `include/multimaster/`; the implementation in `src/`.
`mesh` is a thin pimpl over `mesh_impl`, so `<sys/epoll.h>` and sockets never
leak into consumer translation units.

```
                         ┌──────────────────────────────┐
   public API  ─────────▶│            mesh               │  (mesh.hpp, pimpl)
                         └───────────────┬──────────────┘
                                         │ owns
                         ┌───────────────▼──────────────┐
                         │           mesh_impl           │  composition root,
                         │  - IO thread                  │  PeerManagerDelegate,
                         │  - command mailbox + eventfd  │  callback marshaling
                         │  - state snapshots            │
                         └───┬───────────┬───────────┬───┘
              owns/drives    │           │           │
               ┌─────────────▼──┐  ┌─────▼──────┐  ┌─▼───────────┐
               │   event_loop   │  │ discovery  │  │  listener   │
               │ epoll+timers+  │  │ multicast  │  │ TCP accept  │
               │   eventfd      │  │ announce   │  └─────┬───────┘
               └──────▲─────────┘  └─────┬──────┘        │ accepted fd
                      │ registers fds    │ candidate     │
                      │                  ▼ peers         ▼
                      │            ┌──────────────────────────────┐
                      │            │         peer_manager          │
                      │            │  - per-peer records + FSM     │
                      └────────────┤  - dial-race resolution       │
                       drives conns│  - reconnect/backoff/liveness │
                                   │  - is the Forwarder           │
                                   └───┬───────────────────┬──────┘
                                owns   │                   │ uses
                            ┌──────────▼─────┐      ┌──────▼────────┐
                            │ peer_connection│ ...  │ gossip_router │
                            │ framed TCP I/O │      │ dedup+TTL+    │
                            │ handshake FSM  │      │ flood/relay   │
                            └────────────────┘      └───────────────┘
```

### Responsibilities

| Component | File(s) | Responsibility |
|---|---|---|
| `mesh` | `mesh.hpp`, `mesh.cpp` | Public facade; pimpl forwarding to `mesh_impl`. |
| `mesh_impl` | `mesh_impl.{hpp,cpp}` | Owns the IO thread, the command mailbox, and the subsystems. Implements `peer_manager_delegate` to translate internal events into user `callbacks` and to publish thread-safe state snapshots. |
| `event_loop` | `event_loop.{hpp,cpp}` | `epoll` reactor + steady-clock timer queue + `eventfd` cross-thread wakeup. The heartbeat of the IO thread. |
| `discovery` | `discovery.{hpp,cpp}` | Sends periodic UDP multicast announces; receives peers' announces and surfaces candidates. |
| `listener` | `listener.{hpp,cpp}` | TCP accept socket; hands accepted sockets to `peer_manager`. |
| `peer_connection` | `peer_connection.{hpp,cpp}` | One TCP connection: framed partial read/write, outbound queue, handshake state machine, heartbeat. |
| `peer_manager` | `peer_manager.{hpp,cpp}` | Authoritative mesh state: per-peer records & FSM, dial-race resolution, reconnect/backoff, liveness, peer events. Also the `forwarder` the router drives. |
| `gossip_router` | `gossip_router.{hpp,cpp}` | Forwarding policy: dedup cache, TTL, broadcast flood, targeted relay. Holds no sockets. |
| `socket` | `socket.{hpp,cpp}` | RAII fd wrapper + option helpers (`SOCK_CLOEXEC`, nonblock, reuse, multicast). |
| `buffer` | `buffer.hpp` | Growable byte buffer with a consume cursor for partial I/O. |
| `wire` | `wire.{hpp,cpp}` | Pure encode/decode of announce datagrams and TCP frames. No I/O; unit-testable in isolation. |
| `peer_id` | `peer_id.{hpp,cpp}` | 128-bit node identifier + hashing + hex (de)serialization. |

---

## 3. Threading model

There is exactly **one internal thread** — the IO thread — created by
`mesh_impl::start()` and running `event_loop::run()`.

**Everything that touches mesh state runs on that thread**: all socket I/O,
timers, the handshake/dial-race logic, the dedup cache, peer records, and every
user callback. Because that state is only ever touched by one thread, it needs
**no locks**. This is the central simplification.

### Crossing the thread boundary

User threads call `broadcast` / `send` from anywhere. The hand-off is a
mutex-guarded command mailbox plus an `eventfd` wakeup:

```
user thread                         IO thread (event_loop::run)
-----------                         ---------------------------
broadcast(bytes)
  copy payload into a command
  lock mailbox; push_back; unlock
  event_loop::wakeup()  ───────────▶ epoll wakes on the eventfd
                                     drain_mailbox():
                                       lock; swap out whole deque; unlock
                                       process each command lock-free
```

The lock is held only to push/swap the deque — never during I/O. The `eventfd`
counter coalesces: many posts produce at most one extra loop iteration, and the
loop drains the entire backlog per wake. See `mesh_impl::post` / `drain_mailbox`
and `event_loop::wakeup` / `drain_wake`.

### Callback contract (load-bearing, documented in `events.hpp`)

- Callbacks run **on the IO thread, serialized** — never concurrently.
- Callbacks **must not block** (they stall the entire mesh).
- Callbacks **may** call `broadcast` / `send` (those only enqueue + wake).
- The `bytes` passed to `on_message` views the IO thread's read buffer and is
  valid **only for the duration of that callback** — copy it to retain.
- Calling `stop()` from within a callback is detected via a thread-id compare and
  handled asynchronously (no self-join deadlock); see `mesh_impl::stop`.

### State snapshots

`connected_peers()` / `known_peers()` must be callable from any thread without
blocking the IO thread. The IO thread publishes immutable
`shared_ptr<const vector<peer_id>>` snapshots (`connected_snapshot` /
`known_snapshot`), and accessors return a copy of the current snapshot under a
short mutex. No query is ever posted to the IO thread.

---

## 4. Wire protocol

All integers are network byte order (big-endian). Encoding/decoding is entirely
in `wire.cpp`; nothing else parses bytes off the wire.

### 4.1 Discovery datagram (UDP multicast)

Emitted every `announceInterval` (jittered ±20% to avoid mesh-wide sync), and
once immediately at startup.

```
offset size  field
0      4     magic = 0x4D4D4341 ('M''M''C''A')
4      1     protocolVersion
5      1     flags
6      2     tcpListenPort
8      16    nodeId
24     1     groupNameLen (G)
25     G     groupName (UTF-8)
```

A receiver drops the datagram unless magic, `protocolVersion`, and `groupName`
all match its own, and ignores its own `nodeId` (multicast loopback echo).

### 4.2 TCP frame

Every frame is length-prefixed so the reader can frame without parsing the body:

```
offset size  field
0      4     length  = number of bytes after this field
4      1     frameType
5      ...   body (type-specific)
```

`frame_type`: `Hello=1`, `HelloAck=2`, `Heartbeat=3`, `Data=4`, `Goodbye=5`.

**Hello / HelloAck body** — exchanged once per connection in both directions:
```
16  nodeId
1   protocolVersion
2   tcpListenPort   (lets the acceptor learn how to dial this peer back)
1   groupNameLen
G   groupName
8   nonce           (random; disambiguates rare same-direction dial dups)
```

**Heartbeat body** — empty. The frame's arrival is the liveness signal.

**Data body** — the gossip envelope:
```
16  srcNodeId      (original sender)
16  dstNodeId      (all-zero ⇒ broadcast; otherwise the target node)
16  messageId
1   ttl
4   payloadLen
P   payload        (opaque application bytes)
```

Fixed Data header overhead is 58 bytes — acceptable at LAN scale.

### 4.3 Framing & safety

`try_decode_frame` peeks the 4-byte length, returns `NeedMore` until the whole
frame is buffered, and returns `Error` if `length` exceeds `maxMessageBytes`
(a DoS guard) or the body is malformed. Decoding never consumes input; the
caller (`peer_connection::parse_inbound`) consumes exactly `consumed` bytes after
a successful decode. Partial reads and writes are handled by `buffer`'s
append/consume cursor and the `EPOLLOUT` toggling in `flush_outbound`.

---

## 5. Discovery → connection lifecycle

```
multicast announce ─▶ peer_manager::on_discovered ─▶ peer_record (Discovered)
                                                          │ maybe_dial
                                                          ▼
seed peers ───────▶ start_dial / start_dial_addr ──▶ peer_connection (ConnectingOut)
TCP accept ───────▶ accept_connection ────────────▶ peer_connection (Handshaking, inbound)
                                                          │ Hello/HelloAck exchange
                                                          ▼
                                       peer_manager::on_peer_handshake
                                                          │ dial-race resolution
                                                          ▼
                                          established (estab_ map) ─▶ on_peer_connected
```

When multicast is unavailable (containers, some Wi-Fi), `seedPeers` provides
static `{host, port}` dial targets; `dial_seeds` also re-fires while the node is
isolated (`estab_` empty), so a node can recover into the mesh.

### Per-peer state machine (`peer_manager::peer_state`)

```
Discovered → Connecting → Connected → Disconnected → (Connecting | Lost)
```

- **Disconnected** — the link dropped but the peer is still announcing; reconnect
  is scheduled.
- **Lost** — no announce for `peerLostTimeout`; reconnection stops, `on_peer_lost`
  fires, routing state is pruned.

---

## 6. Reconnection & liveness

Driven by the periodic `tick()` (every `heartbeatInterval`) plus epoll edge
events:

- **Immediate death** — `EPOLLHUP` / `EPOLLERR` / `EPOLLRDHUP` or `recv()==0`
  closes the connection at once.
- **Heartbeat** — each connection sends a `Heartbeat` only when otherwise idle
  (`maybe_heartbeat`); if nothing is received for `heartbeatTimeout` (default 3×
  the interval) the link is declared dead. `SO_KEEPALIVE` and `TCP_NODELAY` are
  set as backstops; writes use `MSG_NOSIGNAL` (no global `SIGPIPE` handler).
- **Backoff** — on drop, `schedule_reconnect` arms a timer with exponential
  backoff + jitter (`reconnectBase` → `reconnectMax`). The attempt counter resets
  on a successful handshake. Reconnection only proceeds while the peer is still
  being announced (or is a seed).
- **Handshake timeout** — connections stuck in `ConnectingOut`/`Handshaking`
  past `handshakeTimeout` are dropped.

### Dial-race resolution

Two nodes that discover each other simultaneously will each dial the other,
producing two TCP connections. Both sides must deterministically keep the
**same** one. The rule (`keep_existing`): keep the connection **initiated by the
numerically-lower `nodeId`**. Both endpoints compute this identically from
`(self id, peer id, who-dialed)`, so they converge; the loser receives a
`Goodbye` and is closed. The `nonce` in `Hello` disambiguates the rare case of
two connections in the same direction.

> This path is subtle: the surviving connection is installed in `estab_` *before*
> the loser is closed, so closing the loser is not mistaken for a peer
> disconnect. A regression here previously caused both sides to keep *different*
> connections (writes vanished); it is now covered by the smoke test.

---

## 7. Gossip routing

`gossip_router` decides, for each Data frame, whether to **deliver** it locally,
**forward** it onward, or **drop** it. It owns no sockets — it calls back into the
`forwarder` interface implemented by `peer_manager`
(`forward_except`, `forward_to`, `deliver_local`).

### Message identity & dedup

Each originated message gets a 128-bit `message_id` = low 8 bytes of the
originator's `nodeId` ‖ a per-node monotonic counter (counter seeded randomly at
startup to avoid reuse across restarts). The dedup cache is an
`unordered_set<message_id>` plus a FIFO deque for eviction, bounded two ways:

- **Time** — entries older than `dedupTtl` are swept periodically (`evict_sweep`).
- **Size** — a hard cap of `maxDedupEntries`; the oldest are evicted first
  (storm protection).

A frame whose id is already in the cache is **dropped entirely** — neither
delivered nor forwarded. This is the primary loop-protection; TTL is a secondary
hop bound.

### Broadcast (`dstNodeId == 0`)

1. Dedup check → drop if seen; otherwise insert.
2. Deliver locally via `on_message` (unless we are the originator — no self-echo).
3. If `ttl > 1`, decrement and **forward to every established peer except the
   inbound link** (split-horizon).

### Targeted (`dstNodeId == someId`)

1. Dedup check → drop if seen; otherwise insert.
2. If `dst == self`: deliver locally and **do not forward**.
3. Otherwise relay only (no local delivery): if `dst` is a direct neighbor,
   unicast to it; else flood-except-inbound so the message diffuses toward the
   target. Dedup keeps the diffusion from storming.

### Delivery semantics

Best-effort, unordered, **at-most-once per node** (dedup guarantees no duplicate
delivery). There is no acknowledgement or retransmission in the library — layer
reliability/ordering on top of the opaque payload if you need it.

---

## 8. Backpressure & resource bounds

- **Per-connection outbound cap** — `maxOutboundQueueBytes`. On overflow the
  `overflowPolicy` decides: `Disconnect` (default — treat a chronically slow peer
  as dead), `DropOldest`, or `DropNewest`. This stops one slow peer from causing
  unbounded memory growth mesh-wide.
- **Max message size** — `maxMessageBytes` rejects oversized frames before
  allocating, dropping the offending connection.
- **Dedup cache** — dual-bounded as in §7.
- **Timers** — all use `steady_clock` (immune to wall-clock changes).
- **fds** — all sockets are `SOCK_CLOEXEC`; the ephemeral listen port is resolved
  via `getsockname` before it is announced.

---

## 9. Shutdown

`stop()` posts a `Stop` command and wakes the loop. On the IO thread,
`peer_manager::shutdown` sends `Goodbye` to established peers and tears down all
connections, then `event_loop::stop` ends `run()`; the caller joins the thread.
`stop()` is idempotent and RAII-safe (`~mesh` calls it). The event loop also
bails out of the current epoll batch the moment it stops, so it never touches a
connection that `shutdown` just freed.

---

## 10. Testing

- **`tests/wire_test.cpp`** — encode/decode round-trips, `NeedMore` on partial
  input, oversized-frame rejection, sequential frames in one buffer.
- **`tests/dedup_test.cpp`** — router policy against a mock `forwarder`: dedup,
  TTL exhaustion, no self-echo, targeted deliver-vs-relay, direct-vs-diffuse,
  size-bounded eviction.
- **`tests/smoke_test.cpp`** — three real `mesh` nodes in one process over
  `127.0.0.1`: broadcast reaches all, targeted reaches exactly one, and stopping
  a node drives `on_peer_disconnected` then `on_peer_lost`.

The suite is dependency-free (a ~40-line harness in `tests/test_harness.hpp`) and
runs clean under ASan + UBSan with leak detection.

---

## 11. Known limitations & future directions

- **Linux-only** — uses `epoll`, `eventfd`, `accept4`. A kqueue/IOCP backend
  behind `event_loop` would broaden support.
- **LAN-scoped** — multicast TTL defaults to 1; cross-subnet meshes need
  `seedPeers`.
- **Tuned for tens of nodes.** Targeted routing currently floods when the
  destination is not a direct neighbor; a learned next-hop table (observed from
  broadcast source paths) would make large-mesh unicast cheaper.
- **No transport security** — payloads and framing are plaintext. Encryption/auth
  would belong as a layer around `peer_connection`'s byte streams.
- Per-link 16-byte node ids could be compressed to small per-connection handles
  to cut Data-frame overhead.
