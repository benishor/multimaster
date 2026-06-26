# How the mesh works — complete reference

This is the exhaustive, byte- and algorithm-level description of `multimaster`:
how a node finds peers, forms and heals connections, and moves messages across
the mesh. It is the deep companion to [architecture.md](architecture.md) (which
is the higher-level tour) and the [README](../README.md) (which is the user
guide). Everything here reflects the actual implementation in `src/`.

**Contents**

1. [Mental model](#1-mental-model)
2. [Identity & addressing](#2-identity--addressing)
3. [Configuration reference](#3-configuration-reference)
4. [Execution model: the single IO thread](#4-execution-model-the-single-io-thread)
5. [Wire protocol (exhaustive)](#5-wire-protocol-exhaustive)
6. [Discovery](#6-discovery)
7. [Connection establishment & the handshake](#7-connection-establishment--the-handshake)
8. [Dial-race resolution](#8-dial-race-resolution)
9. [Liveness, failure detection & reconnection](#9-liveness-failure-detection--reconnection)
10. [Static (WAN) peer maintenance](#10-static-wan-peer-maintenance)
11. [Gossip routing](#11-gossip-routing)
12. [Backpressure & safety limits](#12-backpressure--safety-limits)
13. [Timers & periodic tasks](#13-timers--periodic-tasks)
14. [Data structures & invariants](#14-data-structures--invariants)
15. [Lifecycle: startup & shutdown](#15-lifecycle-startup--shutdown)
16. [End-to-end worked example](#16-end-to-end-worked-example)
17. [Failure-scenario catalog](#17-failure-scenario-catalog)

---

## 1. Mental model

A *mesh* is the set of nodes that share a **group name** and can reach each other
over TCP. Each node:

- announces itself periodically by **UDP multicast** so LAN peers can find it;
- additionally dials any configured **seed** or **static** peers by `host:port`;
- maintains a **TCP connection** to each peer it has paired with;
- exchanges length-prefixed **frames** over those connections;
- **floods** application messages across the mesh, using a per-message dedup
  cache and a hop-count (TTL) to stop loops.

There is no server, no leader, no shared state. Every node runs the identical
logic. A node's view of the mesh is just its table of peer records plus its live
connections.

The whole runtime is **single-threaded** (one IO thread). That single fact
removes all internal locking and makes the rest of this document a description of
sequential code reacting to socket events and timers.

---

## 2. Identity & addressing

### `peer_id` — node identity (`peer_id.hpp`)

A `peer_id` is **128 bits** (`std::array<std::byte,16>`), generated randomly at
construction via `peer_id::generate()` (uses `std::random_device`), unless the
caller pins one in `mesh_config::nodeId`.

- Rendered as 32 lowercase hex chars (`to_string`), parsed back by `from_string`
  (dashes allowed and ignored).
- Has a **total order** (`operator<=>`). This order is load-bearing: it is the
  deterministic tie-break in [dial-race resolution](#8-dial-race-resolution).
- Hashed with FNV-1a over the 16 bytes (`std::hash<peer_id>`), so it can key
  `unordered_map`.
- The **all-zero** id is reserved: on the wire, a Data frame with `dstNodeId`
  all-zero means *broadcast*. `peer_id::is_zero()` tests for it.

### `group name` — mesh segregation

A UTF-8 string (`mesh_config::groupName`, default `"default"`). It appears in
both the discovery datagram and the Hello handshake. **A peer is only accepted if
its group matches**, so multiple independent meshes can coexist on the same LAN /
multicast group. Mismatch → discovery datagram ignored, or handshake rejected.

### `message_id` — message identity (`wire.hpp`)

A 128-bit id minted by the *originator* of each application message, used for
dedup. Construction (`gossip_router::next_id`):

```
message_id.bytes[0..8)   = originator nodeId.bytes[0..8)   // namespace by origin
message_id.bytes[8..16)  = ++counter, big-endian            // per-node sequence
```

`counter` is seeded at startup from `random_device` (high and low 32 bits xored),
so ids minted after a restart do not collide with ones still circulating from
before the restart. Combined with the origin-namespaced prefix, ids are globally
unique without any coordination.

---

## 3. Configuration reference

Every knob in `mesh_config` (`config.hpp`), its default, and what it controls.
Durations are `std::chrono::milliseconds` and are interpreted against
`steady_clock`.

| Field | Default | Meaning |
|---|---|---|
| `nodeId` | random | This node's 128-bit id. |
| `groupName` | `"default"` | Logical mesh name; only matching peers pair. |
| `protocolVersion` | `1` | Wire version; mismatch ⇒ peer rejected. |
| `multicastAddr` | `239.255.42.99` | Multicast group for discovery. |
| `multicastPort` | `45454` | UDP port for announces. |
| `multicastIface` | `""` | Local NIC IP to bind multicast to; `""` = kernel default. |
| `announceInterval` | `1000 ms` | Period between multicast announces (jittered ±20%). |
| `peerLostTimeout` | `5000 ms` | No announce for this long ⇒ peer transitions to **Lost**. |
| `multicastTtl` | `1` | IP multicast TTL; `1` keeps announces on the local subnet. |
| `listenPort` | `0` | TCP listen port; `0` = ephemeral (resolved & advertised). |
| `bindAddr` | `0.0.0.0` | TCP bind address. |
| `heartbeatInterval` | `1000 ms` | Idle-link heartbeat period; also the `tick()` period. |
| `heartbeatTimeout` | `3000 ms` | No bytes received for this long ⇒ link declared dead. |
| `handshakeTimeout` | `2000 ms` | Connect/handshake must complete within this. |
| `reconnectBase` | `500 ms` | Base of exponential reconnect backoff. |
| `reconnectMax` | `30000 ms` | Cap of reconnect backoff. |
| `initialTtl` | `8` | Hop budget stamped on originated messages. |
| `dedupTtl` | `30000 ms` | How long a `message_id` stays in the dedup cache. |
| `maxDedupEntries` | `100000` | Hard cap on dedup-cache size (oldest evicted). |
| `fanout` | `0` | Max peers to *dial* (`0` = dial everyone; near-full mesh). |
| `maxOutboundQueueBytes` | `8 MiB` | Per-connection outbound buffer cap. |
| `overflowPolicy` | `Disconnect` | What to do when that cap is hit. |
| `maxMessageBytes` | `16 MiB` | Reject frames whose length exceeds this (DoS guard). |
| `seedPeers` | empty | Bootstrap `{host,port}` hints (dialed only while isolated). |
| `staticPeers` | empty | Persistently maintained `{host,port}` peers (WAN). |

---

## 4. Execution model: the single IO thread

`mesh_impl::start()` spawns exactly one thread running `event_loop::run()`. From
that moment, **all** mesh state — sockets, peer records, the dedup cache, timers,
and every user callback — is touched only by that thread. No mutexes guard mesh
state. The only synchronization is at the thread boundary (below).

### 4.1 The reactor (`event_loop`)

Built around three fds:

- an **epoll** instance (`epoll_create1(EPOLL_CLOEXEC)`);
- an **eventfd** (`EFD_NONBLOCK | EFD_CLOEXEC`) registered in epoll for `EPOLLIN`,
  used as the cross-thread wakeup;
- the registered I/O fds (listener, discovery socket, each peer connection), each
  associated with an `io_handler*` via `epoll_event.data.ptr`.

The run loop:

```
running = true
while running:
    timeout = next_timeout_ms()          # see below
    n = epoll_wait(epfd, events, 64, timeout)
    for each ready event (until !running):
        if ptr == wake_sentinel:
            drain_wake()                 # read the eventfd counter empty
            wake_handler()               # mesh_impl::drain_mailbox
        else:
            ((io_handler*)ptr)->on_io_events(mask)
    fire_due_timers()
```

Notes:
- The loop **bails out of the current event batch the instant `running` becomes
  false** (e.g. a Stop command tore down connections), so it never dispatches to
  a handler that was just freed.
- `EINTR` from `epoll_wait` is retried; any other negative return ends the loop.

### 4.2 Timers

A `std::multimap<time_point, timer>` ordered by due time. `add_timer(delay, cb)`
returns a `timer_id`; `cancel_timer(id)` marks it (checked at fire time).

`next_timeout_ms()` returns:
- `-1` (block forever) if no timers;
- `0` if the earliest timer is already due;
- otherwise the milliseconds until it is due, **capped at 1000 ms** so newly
  added timers and cancellations are noticed promptly.

`fire_due_timers()` pops and invokes every timer whose time has passed, skipping
cancelled ones, re-reading `now` after each callback (a callback may re-arm a
timer or take time).

Periodic behaviors (announce, `tick`, dedup sweep) are implemented as one-shot
timers that **re-arm themselves** at the end of their callback.

### 4.3 Crossing the thread boundary (`mesh_impl`)

Public `broadcast`/`send` may be called from any thread. They:

```
1. copy the payload into a command {Broadcast|Targeted|Stop, dst, bytes}
2. lock mailbox mutex; mailbox.push_back(command); unlock
3. event_loop::wakeup()   # write 1 to the eventfd
```

The IO thread, woken on the eventfd, runs `drain_mailbox()`:

```
lock mailbox mutex; swap whole deque into a local; unlock
for each command:
    Broadcast -> peer_manager::originate_broadcast(payload)
    Targeted  -> peer_manager::originate_targeted(dst, payload)
    Stop      -> remember stopRequested
if stopRequested: peer_manager::shutdown(); event_loop::stop()
```

The lock is held only to move the deque; never during I/O. The eventfd counter
**coalesces**: many posts before the loop wakes produce a single drain that
empties the entire backlog.

### 4.4 Callback contract

Because callbacks run on the IO thread, serialized:

- They **must not block** — doing so stalls the whole mesh.
- They **may** call `broadcast`/`send` (those only enqueue + wake; if called from
  the IO thread itself the wake is a harmless self-signal).
- The `bytes` handed to `on_message` aliases the IO thread's read buffer and is
  valid **only for the duration of the callback** — copy to retain.
- Calling `stop()` from inside a callback is detected by thread-id compare and
  performed asynchronously (the loop exits and is joined later), never
  self-joining.

### 4.5 State snapshots for other threads

`connected_peers()` / `known_peers()` must not block the IO thread. The IO thread
publishes immutable `shared_ptr<const vector<peer_id>>` snapshots whenever the
relevant set changes (`connected_snapshot` / `known_snapshot`); accessors copy
the current snapshot under a short mutex. No query is ever posted to the loop.

---

## 5. Wire protocol (exhaustive)

All multi-byte integers are **network byte order (big-endian)**. All encode/decode
lives in `wire.cpp`; no other code parses bytes off the wire.

### 5.1 Discovery datagram (UDP multicast)

```
offset  size  field
0       4     magic = 0x4D4D4341  ('M','M','C','A')
4       1     protocolVersion
5       1     flags                 (reserved, currently 0)
6       2     tcpListenPort
8       16    nodeId
24      1     groupNameLen  (G, 0..255)
25      G     groupName (UTF-8)
              total size = 25 + G bytes
```

**Receiver validation** (`discovery::on_io_events` → `decode_announce`), datagram
dropped unless **all** hold:
1. `magic == 0x4D4D4341`;
2. fully parses without overrun;
3. `protocolVersion == ours`;
4. `groupName == ours`;
5. `nodeId != ours` (ignore multicast loopback echo of our own announce).

A surviving datagram yields a *candidate* `(announce, source sockaddr_in)` handed
to `peer_manager::on_discovered`.

### 5.2 TCP frame envelope

Every TCP frame:

```
offset  size  field
0       4     length   = number of bytes that follow this field
4       1     frameType
5       ...   body (frameType-specific, length-1 bytes)
```

`frame_type` values: `Hello=1`, `HelloAck=2`, `Heartbeat=3`, `Data=4`,
`Goodbye=5`, `Membership=6`.

### 5.3 Hello / HelloAck body

Exchanged once per connection in each direction (the dialer sends `Hello`, the
accepter sends `HelloAck`; both carry the same fields):

```
16  nodeId
1   protocolVersion
2   tcpListenPort     # lets the accepter learn how to dial this peer back
1   groupNameLen (G)
G   groupName
8   nonce             # random per connection; disambiguates same-direction dups
```

### 5.4 Heartbeat body

Empty. The frame's *arrival* is the liveness signal (it refreshes the receiver's
`lastRecv` timestamp). Sent only when a link is otherwise idle.

### 5.5 Goodbye body

Empty. A graceful "I am closing this connection" sent by the loser of a dial race
and during shutdown. On receipt the connection is closed immediately.

### 5.6 Data body (the gossip envelope)

```
16  srcNodeId       # original sender (never rewritten by relays)
16  dstNodeId       # all-zero ⇒ broadcast; otherwise the target node
16  messageId
1   ttl             # decremented at each hop
4   payloadLen
P   payload         # opaque application bytes
```

Total Data frame on the wire = `4 (length) + 1 (type) + 16 + 16 + 16 + 1 + 4 + P`
= **58 + P bytes**. The 58-byte header overhead is acceptable at LAN scale.

### 5.7 Membership body (the adjacency-gossip envelope)

```
16  originId          # the node whose neighbor set this is
8   version           # monotonic per origin (restart-safe; ordering tag)
2   neighborCount (N)
N*16 neighbors        # originId's direct neighbors
```

Flooded across the mesh so every node can derive the full membership; see §11.6.
Loop-free because a record `(origin, version)` is accepted/forwarded only if
`version` is newer than the last one stored for that origin.

### 5.8 Framing & partial I/O

TCP is a byte stream: one `recv` may contain part of a frame, several frames, or a
frame split across reads. The connection handles this with a `buffer` (an
append/consume cursor) and `try_decode_frame`:

`try_decode_frame(input, maxMessageBytes, out, consumed)` returns:
- **`NeedMore`** — fewer than 4 bytes, or fewer than `4 + length` bytes buffered.
  Nothing consumed; wait for more data.
- **`Error`** — `length < 1`, or `length > maxMessageBytes`, or the body is
  malformed (overruns, unknown frame type). The caller drops the connection.
- **`Ok`** — `out` is filled and `consumed = 4 + length`.

Decode never consumes input; the caller consumes exactly `consumed` bytes after a
successful decode (and, for Data, **after** delivering — because the decoded
`payload` span aliases the buffer that `consume()` may compact). The read path
(`do_read`) drains the socket in 64 KiB chunks until `EAGAIN`, then loops
`try_decode_frame` until `NeedMore`.

The write path (`flush_outbound`) sends from the outbound `buffer` until `EAGAIN`,
then arms `EPOLLOUT`; when the buffer drains it disarms `EPOLLOUT`. `MSG_NOSIGNAL`
suppresses `SIGPIPE` on writes to a closed peer.

---

## 6. Discovery

### 6.1 Socket setup (`discovery::start`)

```
sock = UDP, nonblocking, CLOEXEC
setsockopt SO_REUSEADDR, SO_REUSEPORT   # allow many nodes per host (and tests)
bind 0.0.0.0:multicastPort
setsockopt IP_ADD_MEMBERSHIP (multicastAddr, multicastIface or INADDR_ANY)
setsockopt IP_MULTICAST_TTL  = multicastTtl   # default 1 => local subnet only
setsockopt IP_MULTICAST_LOOP = 1              # needed for same-host nodes & tests
[setsockopt IP_MULTICAST_IF if multicastIface set]
register sock in epoll for EPOLLIN
announce() once immediately, then schedule_announce()
```

Any failure here is reported via `on_error(error_category::Discovery, ...)` but is
**non-fatal**: the node keeps running and can still use seed/static peers. This is
what makes the library usable in containers where multicast is blocked.

### 6.2 Announce loop

`announce()` encodes the datagram (§5.1) with this node's id, group, version, and
*resolved* TCP listen port, and `sendto`s it to the multicast group.

`schedule_announce()` arms a timer for `announceInterval ± 20%` jitter, whose
callback announces again and reschedules. Jitter prevents a roomful of nodes from
synchronizing their announces into periodic bursts.

### 6.3 Receiving

On `EPOLLIN`, `recvfrom` is looped until `EAGAIN`. Each datagram is validated
(§5.1) and, if it survives, `peer_manager::on_discovered(announce, src)` is
called. The dial-back address is the **source IP of the datagram** combined with
the **`tcpListenPort` advertised inside it** (not the UDP source port).

---

## 7. Connection establishment & the handshake

A `peer_connection` is one TCP socket plus its state. It is created three ways and
converges on the same handshake.

### 7.1 Outbound dial (`peer_connection::start_connect`)

```
state = ConnectingOut
sock = TCP, nonblocking, CLOEXEC; TCP_NODELAY; SO_KEEPALIVE
rc = connect(addr)
if rc != 0 and errno != EINPROGRESS: fail
register sock in epoll for EPOLLOUT     # writable ⇒ connect resolved
```

When `EPOLLOUT` fires (`on_connect_complete`):

```
check SO_ERROR via getsockopt; nonzero ⇒ fail
state = Handshaking
re-register epoll for EPOLLIN           # now we also need to read the peer's Hello
send Hello                              # (may re-arm EPOLLOUT if the write blocks)
```

> The explicit re-register to `EPOLLIN` here is essential: the socket was
> registered `EPOLLOUT`-only for the connect, and without this it would never
> become readable, so the peer's Hello would never arrive. (This was a real bug,
> now covered by the smoke test.)

### 7.2 Inbound accept (`listener` → `peer_manager::accept_connection`)

The listener's TCP socket is bound to `bindAddr:listenPort` (ephemeral port
resolved via `getsockname` and then advertised). On `EPOLLIN` it `accept4`s
(nonblocking, CLOEXEC) in a loop until `EAGAIN`, wrapping each accepted fd in a
`peer_connection` constructed in `Handshaking` state. An inbound connection sends
its `HelloAck` **immediately** on construction.

### 7.3 Handshake state machine

```
ConnectingOut ──(EPOLLOUT, SO_ERROR ok)──▶ Handshaking ──(valid Hello/Ack)──▶ Established
   (outbound only)                              │
                                                └──(bad group/version, timeout,
                                                    or peer hangup)──▶ Dead
```

Both sides send their Hello/HelloAck and wait to receive the peer's. On receiving
it (`parse_inbound`), the connection:
1. rejects (→ Dead) if `protocolVersion` or `groupName` mismatch;
2. records the peer's `nodeId`, advertised `tcpListenPort`, and `nonce`;
3. transitions to `Established` and notifies `peer_manager::on_peer_handshake`.

A connection that does not reach `Established` within `handshakeTimeout` is
dropped by `tick()`.

### 7.4 What `peer_manager` does on handshake

`on_peer_handshake` (single-threaded, so no races):
1. Drop self-connections (`pid == ourId`).
2. If this connection came from a static-peer dial, bind the static target to
   `pid` and flag the record `isStatic` (see §10).
3. Create/refresh the `peer_record` for `pid`: store dial-back address (peer IP +
   advertised listen port), `lastSeen = now`, fire `peer_discovered` the first
   time we ever learn of this peer.
4. Run **dial-race resolution** (§8): if an established connection to `pid`
   already exists and should be kept, this new connection loses and is sent
   `Goodbye`.
5. Otherwise install this connection as the live link (`estab_[pid] = conn`), set
   state `Connected`, reset the reconnect attempt counter, close any superseded
   connection, and fire `peer_connected` (first time only).

---

## 8. Dial-race resolution

Two nodes that discover each other simultaneously each dial the other, producing
**two** TCP connections. Both sides must deterministically converge on keeping the
**same** one, or messages would split across two half-dead links.

**Rule:** keep the connection **initiated by the numerically-lower `nodeId`**.

A connection is "initiated by us" iff it is *outbound* (`!inbound`).
`keep_existing(existing, new, peer)`:

```
if existing.inbound == new.inbound:    # same direction ⇒ duplicate; keep older
    return keep existing
weAreLower   = (ourId < peer)
winnerInbound = !weAreLower            # if peer is lower, peer initiated ⇒ inbound on us
return existing.inbound == winnerInbound
```

Both endpoints compute the identical decision from `(ourId, peerId, direction)`,
so they pick the same physical connection. The loser is sent `Goodbye` and closed.
The `nonce` in Hello disambiguates the rare case of two connections in the *same*
direction (e.g. a stale half-open one lingering).

**Worked example.** Nodes A (`id=0x10…`) and B (`id=0x90…`), A < B.
- Connection X: A dials B → on A it's *outbound*, on B it's *inbound*.
- Connection Y: B dials A → on B it's *outbound*, on A it's *inbound*.
- Rule: keep the one A initiated = **X**.
- On A: `weAreLower=true → winnerInbound=false` ⇒ keep the *outbound* connection = X. ✓
- On B: `weAreLower=false → winnerInbound=true` ⇒ keep the *inbound* connection = X. ✓

Both keep X; Y is closed on both ends. The winner is installed in `estab_`
*before* the loser is closed, so closing the loser is not mistaken for a peer
disconnect.

---

## 9. Liveness, failure detection & reconnection

### 9.1 Heartbeats

`tick()` runs every `heartbeatInterval`. For each `Established` connection it calls
`maybe_heartbeat(now, heartbeatInterval)`, which sends an empty `Heartbeat` frame
only if the link has been **idle** (`now - lastSend >= heartbeatInterval`). Any
outbound Data resets that idle timer, so heartbeats are suppressed on busy links.

### 9.2 Failure detection

A connection is declared dead on the earliest of:
- **socket-level**: `EPOLLHUP` / `EPOLLERR` / `EPOLLRDHUP`, or `recv()==0`
  (peer closed), or a non-transient `recv`/`send` error;
- **application-level**: `now - lastRecv > heartbeatTimeout` (checked in `tick`);
- **protocol**: a malformed/oversized frame, or a group/version mismatch at
  handshake;
- **handshake**: still not `Established` after `handshakeTimeout`;
- **explicit**: a received `Goodbye`.

`SO_KEEPALIVE` is also enabled as an OS-level backstop.

### 9.3 Closing & reaping

Death routes through `mark_dead → connection_listener::on_peer_closed`. A
connection **never deletes itself**; instead `peer_manager` schedules a
**deferred reap** (a 0 ms timer) that removes the dead connections from its
owning vector after the current event batch. This guarantees no use-after-free
when a connection dies in the middle of dispatch.

`on_peer_closed` also: detaches the connection from `estab_` if it was the live
link, transitions the record to `Disconnected`, fires `peer_disconnected` (if it
was connected), and triggers reconnection.

### 9.4 Reconnect backoff

For non-static peers, `schedule_reconnect` arms a one-shot timer:

```
attempt = min(reconnectAttempts, 20)
ms      = reconnectBase << attempt          # exponential
ms      = min(ms, reconnectMax)
ms     += random in [0, ms/5]               # jitter
reconnectAttempts++
add_timer(ms, on_reconnect_timer(id))
```

`on_reconnect_timer` re-dials **only if the peer is still believed present** —
i.e. `now - lastSeen <= peerLostTimeout` (it is still being announced). The
attempt counter resets to 0 on a successful handshake.

### 9.5 Disconnected vs Lost

```
Discovered → Connecting → Connected → Disconnected → (Connecting | Lost)
```

- **Disconnected**: the link dropped but the peer is still announcing (or is a
  seed); reconnection is in progress.
- **Lost**: `tick()` found `now - lastSeen > peerLostTimeout` and the peer is not
  connected. It fires `peer_lost`, prunes the record, and stops reconnecting.
  **Static peers are exempt** and are never marked Lost (§10).

---

## 10. Static (WAN) peer maintenance

Internet peers can't be discovered by multicast, so they're configured explicitly
in `staticPeers` and kept connected **persistently**. Each is tracked as a
`static_target`:

```
static_target { host; port; learnedId?; attempts; nextAttempt; dialing }
```

`maintain_static_peers()` runs at startup and on every `tick()`:

```
for each target t:
    if t.learnedId is set and estab_ contains it:   # already connected
        t.attempts = 0; continue
    if t.dialing != null:                            # a dial is in flight
        continue
    if now < t.nextAttempt:                          # still backing off
        continue
    # arm backoff for the next attempt, then dial now
    ms = min(reconnectBase << min(t.attempts,20), reconnectMax) + jitter
    t.nextAttempt = now + ms ; t.attempts++
    start_static_dial(t)
```

`start_static_dial`:
1. **Re-resolves DNS** (`resolve_endpoint`): numeric IPs go straight through;
   hostnames go through `getaddrinfo` *fresh each time* (so dynamic-DNS / rolling
   cloud IPs keep working). Failure → `on_error(Discovery)`, retry next tick.
2. Creates an outbound `peer_connection`, records `conn → target` so the result
   can be tied back, sets `t.dialing = conn`.

When that connection handshakes, `on_peer_handshake` sets `t.learnedId = pid`,
clears `t.dialing`, resets attempts, and flags the record `isStatic`. From then on:
- the peer is **never pruned as Lost** (`tick`'s Lost sweep skips `isStatic`);
- when it drops, it is **redialed by endpoint** via `maintain_static_peers`
  (re-resolving DNS), **not** via the id-keyed reconnect path — so it recovers
  even if the peer restarted with a new id behind the same hostname;
- duplicates (e.g. the same peer also found by multicast) collapse to one link via
  ordinary dial-race resolution.

(`seedPeers`, by contrast, are dialed by `dial_seeds` at startup and re-dialed only
while the node is isolated — `estab_` empty — making them pure bootstrap hints.)

---

## 11. Gossip routing

`gossip_router` decides, for every Data frame, whether to **deliver** it locally,
**forward** it onward, or **drop** it. It owns no sockets; it calls the
`forwarder` interface implemented by `peer_manager`:

- `forward_except(frame, exceptFd)` — send to every established peer except the
  one on `exceptFd` (`-1` = send to all). Targets are snapshotted before sending
  because a send may close a connection.
- `forward_to(dst, frame)` — send to `dst` if it is a direct neighbor; returns
  whether such a link existed.
- `deliver_local(from, payload)` — hand the payload to `on_message`.

### 11.1 Dedup cache

An `unordered_set<message_id>` plus a FIFO `deque<{id, time}>` for eviction.
`seen(id)` returns true (and does nothing) if the id is already present;
otherwise it inserts and returns false. Bounded two ways:

- **Size**: if the deque exceeds `maxDedupEntries`, the oldest ids are evicted
  immediately (storm protection).
- **Time**: `evict_sweep()` (a timer re-arming every `dedupTtl / 4`) drops entries
  older than `dedupTtl`.

A frame whose id is already `seen` is **dropped entirely** — not delivered, not
forwarded. This is the primary loop protection.

### 11.2 TTL

The originator stamps `ttl = initialTtl`. Each relay forwards with `ttl - 1` and
only if the *incoming* `ttl > 1`. TTL is a secondary hop bound; dedup is what
actually prevents loops, so TTL mainly caps worst-case fan-out depth.

### 11.3 Broadcast (`dstNodeId == 0`)

Receiving a broadcast Data frame on the link with fd `L` (`gossip_router::on_data`):

```
if seen(msgId): return                      # duplicate ⇒ drop
insert msgId into dedup
if src != self: deliver_local(src, payload) # never deliver our own echo
if ttl > 1:
    forward_except(encode_data(src, 0, msgId, ttl-1, payload), exceptFd = L)
```

Originating a broadcast (`originate_broadcast`):

```
id = next_id(); seen(id)                    # pre-seed so our own copy is ignored
forward_except(encode_data(self, 0, id, initialTtl, payload), exceptFd = -1)
```

**Worked example.** Line topology `A — B — C — D`, A broadcasts `M`
(`initialTtl=8`):
1. A seeds `id(M)`, sends to B (its only neighbour).
2. B: not seen → insert → deliver to app → `ttl 8→7`, forward to all-except-A = C.
3. C: not seen → insert → deliver → `ttl 7→6`, forward to all-except-B = D.
4. D: not seen → insert → deliver → `ttl 6→5`, forward to all-except-C = ∅.
Every node delivered exactly once; no node re-forwards M because the second time
they'd see it (if the graph had cycles) `seen` drops it.

### 11.4 Targeted (`dstNodeId == T`)

Receiving a targeted Data frame on fd `L`:

```
if seen(msgId): return
insert msgId
if dst == self:
    deliver_local(src, payload)             # arrived; do NOT forward
    return
# relay only — we are an intermediate node, no local delivery
if ttl > 1:
    frame = encode_data(src, dst, msgId, ttl-1, payload)
    if not forward_to(dst, frame):          # direct link to T?
        forward_except(frame, exceptFd = L) # else diffuse toward it
```

Originating a targeted message (`originate_targeted`):

```
if dst == self: deliver_local(self, payload); return
id = next_id(); seen(id)
frame = encode_data(self, dst, id, initialTtl, payload)
if not forward_to(dst, frame):              # direct neighbour?
    forward_except(frame, exceptFd = -1)    # else flood; dedup tames it
```

So a targeted message is a **unicast** when the destination is a direct neighbour,
and a **deduplicated flood** otherwise — intermediate nodes relay it but never
hand it to their application. Only the addressed node delivers it.

### 11.5 Delivery semantics & guarantees

- **Best-effort**: no acknowledgements, no retransmission at the library layer.
- **At-most-once per node**: the dedup cache guarantees a given message is
  delivered to a given node at most once.
- **Unordered**: messages can arrive in any order; different paths have different
  latencies.
- **Reachability**: a broadcast reaches every node in the same connected
  component as the sender (subject to TTL). A targeted message reaches its
  destination if a path within TTL exists.

If you need reliability or ordering, build it on top of the opaque payload (e.g. a
sequence number + ack of your own).

### 11.6 Membership (mesh-wide reachability)

Data routing only relays *messages*; it does not tell a node who else is in the
mesh. That is the job of the **membership** subsystem (`membership.{hpp,cpp}`),
which gossips topology so every node — even across a static-peer bridge — learns
the full set of reachable nodes.

**What each node maintains:**
- `selfNeighbors_` — its own direct neighbors (the keys of `estab_`).
- `view_` — the latest `membership_record` received per origin: `{version,
  neighbors, lastSeen}`.
- `members_` — the set currently reachable from self (excluding self).
- `selfVersion_` — a monotonic counter, **seeded from the wall clock at startup**
  so records minted after a restart outrank any still circulating from the prior
  incarnation. Bumped on every self-flood.

**Flooding & dedup.** A node floods its own record (`origin=self`,
`version=++selfVersion_`, `neighbors=selfNeighbors_`):
- immediately whenever its neighbor set changes (`set_local_neighbors`), and
- as a keepalive every `membershipInterval` (in `tick`).

On receiving a record for `origin O` with version `V`: if `O == self` ignore;
else if `V` is newer than the stored version for `O`, store it (refresh
`lastSeen`), recompute membership, and **re-flood to all neighbors except the
inbound link**; otherwise drop. Because every flood carries a strictly higher
version, each `(origin, version)` is forwarded at most once per node — the floods
terminate without a TTL.

**Deriving membership (`recompute_and_fire`).** Build an undirected graph: an edge
`u—v` for every `v` in `u`'s neighbor list (from `selfNeighbors_` and every
`view_` entry). BFS from `self`; the visited set minus `self` is the reachable
membership. Diff against the previous `members_` → fire `onMemberJoined` for new
nodes and `onMemberLeft` for departed ones, and publish the `members()` snapshot.

**Expiry (`tick`).** Records not refreshed within `membershipTimeout` are dropped,
then membership is recomputed. This is how a member whose only bridge died
eventually disappears: its record stops being refreshed (the keepalives flowed
through the dead bridge), expires, and BFS no longer reaches it.

**Latency.** A *join*, or a change in direct links, propagates within one flood
hop per mesh hop (near-immediate, since changes flood eagerly). A *leave* that
relies on expiry (e.g. a bridge node dying) is observed after up to
`membershipTimeout`. A graceful direct disconnect updates the neighbor set
immediately, but a node only fully drops from `members_` once no path remains in
any live record — bounded by `membershipTimeout`.

**Worked example** — `B —— A —— C` (A bridges; B and C are not direct):
1. B and C each connect to A. A's neighbor set becomes `{B, C}`; A floods
   `A:{B,C}`.
2. B receives `A:{B,C}` → graph edges `B—A`, `A—C` → BFS from B reaches `{A, C}`
   → B fires `onMemberJoined(A)` and `onMemberJoined(C)`. B's `connected_peers()`
   is still just `{A}`; its `members()` is `{A, C}`.
3. C departs. A detects the direct drop, sets its neighbors to `{B}`, floods
   `A:{B}`. C's own record (`C:{A}`) stops being refreshed and expires after
   `membershipTimeout`; once gone, BFS from B no longer reaches C → B fires
   `onMemberLeft(C)`.

---

## 12. Backpressure & safety limits

- **Per-connection outbound cap** (`maxOutboundQueueBytes`, default 8 MiB).
  `send_raw` enforces it before appending. On overflow, `overflowPolicy`:
  - `Disconnect` (default) — treat a chronically slow peer as dead and drop it,
    preventing one slow consumer from ballooning memory mesh-wide;
  - `DropNewest` — drop the frame being enqueued, report `Backpressure`;
  - `DropOldest` — drop buffered backlog to make room.
- **Max message size** (`maxMessageBytes`, default 16 MiB). `try_decode_frame`
  rejects any frame whose `length` exceeds it *before* allocating, and the
  offending connection is dropped — a guard against a malicious/buggy peer.
- **Dedup cache** — dual-bounded (size + time), §11.1.
- **Steady clock everywhere** — all timeouts/backoff use `steady_clock`, immune to
  wall-clock jumps.
- **fd hygiene** — every socket is `SOCK_CLOEXEC`; the ephemeral listen port is
  resolved before it is advertised.

---

## 13. Timers & periodic tasks

| Timer | Period / delay | Re-arms? | Effect |
|---|---|---|---|
| Announce | `announceInterval` ±20% | yes | Multicast this node's presence. |
| Tick | `heartbeatInterval` | yes | Heartbeats, liveness/handshake timeouts, Lost sweep, static-peer maintenance, membership keepalive + expiry, seed re-dial if isolated. |
| Dedup sweep | `dedupTtl / 4` | yes | Evict dedup-cache entries older than `dedupTtl`. |
| Membership keepalive | `membershipInterval` (driven by Tick) | yes | Re-flood this node's adjacency; expire records older than `membershipTimeout`. |
| Reconnect | backoff `reconnectBase…reconnectMax` (+jitter) | no (re-armed on each drop) | Re-dial a dropped non-static peer. |
| Reap | `0 ms` | no | Free connections marked dead during the current batch. |

---

## 14. Data structures & invariants

In `peer_manager` (all IO-thread-only):

- `conns_ : vector<unique_ptr<peer_connection>>` — owns every live connection
  object. Connections are removed here (and freed) only by the deferred `reap()`.
- `records_ : unordered_map<peer_id, peer_record>` — one record per known peer:
  dial-back address, state, `lastSeen`, reconnect attempts, `announced`,
  `isStatic`, and the current `conn` pointer (or null).
- `estab_ : unordered_map<peer_id, peer_connection*>` — the **single** live link
  per peer.
- `static_targets_ : vector<static_target>` and
  `static_dial_conn_ : unordered_map<peer_connection*, size_t>` — static peer
  bookkeeping (§10).

Invariants:
- At most **one** entry in `estab_` per `peer_id` (dial-race guarantees this).
- A pointer in `estab_`, `record.conn`, `static_target.dialing`, or
  `static_dial_conn_` always refers to a connection still owned by `conns_` —
  every death clears these references in `on_peer_closed` *before* `reap()` frees
  the object.
- Every message delivered to the application passed a `seen()` check first
  (at-most-once per node).

---

## 15. Lifecycle: startup & shutdown

### Startup (`mesh_impl::start`)

```
1. create peer_manager
2. create + bind the listener; resolve the ephemeral port via getsockname;
   store/advertise it as self.listenPort           # throws on bind failure
3. create discovery (multicast)
4. set the event_loop wake handler = drain_mailbox
5. peer_manager.start():  router.start();           # arm dedup sweep
                          register static targets;
                          dial_seeds();
                          maintain_static_peers();
                          schedule_tick()
6. discovery.start():     join group; announce now; schedule announces
7. spawn the IO thread running event_loop.run()
```

All registration happens before the loop runs, so there is no concurrency during
construction.

### Shutdown (`mesh_impl::stop`)

```
post a Stop command; event_loop::wakeup()
if called on the IO thread (from a callback):
    return  # don't self-join; the loop will exit and be joined later
else:
    join the IO thread
```

On the IO thread, the Stop command triggers `peer_manager::shutdown()` (send
`Goodbye` to established peers, clear all tables) then `event_loop::stop()`. `stop`
is idempotent and is called by `~mesh` (RAII). The run loop's mid-batch `running`
check ensures no freed connection is touched after `shutdown`.

---

## 16. End-to-end worked example

Three machines, group `"team"`. A and B on the same LAN; C is a cloud host that A
lists in `staticPeers` (`c.example.com:45000`).

1. **A, B start.** Each binds an ephemeral TCP port, joins the multicast group,
   and announces. A also begins maintaining its static target C.
2. **LAN discovery.** A hears B's announce (and vice versa). Both create records,
   fire `on_peer_discovered`, and dial each other. The two connections race; the
   lower-id node's outbound link wins (§8); both fire `on_peer_connected`. A↔B is
   up.
3. **Static dial.** A's `maintain_static_peers` resolves `c.example.com`, dials
   `:45000`, handshakes, marks C's record `isStatic`, fires `on_peer_connected`.
   A↔C is up. (C, if also configured with A, would have dialed too; the duplicate
   collapses via dial-race.) The mesh is now A–B and A–C (A is the bridge).
4. **Broadcast.** B calls `broadcast("hi")`. B seeds the id and sends to A. A:
   unseen → deliver to A's app → forward to all-except-B = C. C: unseen → deliver
   to C's app. B's message reached both A and C; A did not echo it back to B.
5. **Targeted.** C calls `send(B_id, "ping")`. C has no direct link to B, so it
   floods (to A). A: relay (not for A) → has a direct link to B → unicast to B. B
   delivers "ping". A and C's app layers never saw it.
6. **C drops** (network blip). A detects `EPOLLRDHUP`/heartbeat-timeout, fires
   `on_peer_disconnected`, and — because C is static — `maintain_static_peers`
   redials with backoff, re-resolving DNS. C is **not** marked Lost. When C
   returns, A↔C re-establishes and fires `on_peer_connected` again.
7. **B leaves** (process exit). A sees the socket close, fires
   `on_peer_disconnected`; B stops announcing; after `peerLostTimeout` A's `tick`
   fires `on_peer_lost` for B and prunes it.

---

## 17. Failure-scenario catalog

| Scenario | Handling |
|---|---|
| Multicast blocked (container, some Wi-Fi) | Discovery setup error is reported, not fatal; use `seedPeers`/`staticPeers`. |
| Both peers dial simultaneously | Dial-race resolution keeps one link deterministically (§8); loser gets `Goodbye`. |
| Peer crashes / cable pulled | `EPOLLHUP`/`recv==0` or heartbeat timeout → disconnect → reconnect (or static maintenance). |
| Peer silently hangs | `heartbeatTimeout` (no bytes received) declares it dead. |
| Slow consumer | Per-peer outbound cap + `overflowPolicy` (default: disconnect). |
| Oversized/garbage frame | `maxMessageBytes` / malformed-frame check drops the connection (`on_error(Protocol)`). |
| Message storm / loops | Dedup cache (forward at most once per link) + TTL + size/time-bounded cache. |
| Static peer's IP changed | DNS re-resolved on every static dial attempt. |
| Static peer restarts with new id | Redialed by endpoint; new id learned at handshake; stale record dropped. |
| Connection dies mid-dispatch | Deferred reap (0 ms timer) frees it after the batch — no use-after-free. |
| Wall-clock jump (NTP) | All timing uses `steady_clock`; unaffected. |
| Two independent meshes on one LAN | Separated by `groupName` (and/or `multicastPort`). |
| `stop()` from inside a callback | Detected; performed asynchronously, no self-join deadlock. |
