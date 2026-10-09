# ApSync TODO

`include/sync/apsyncnet.h` is a session, channel and snapshot layer over SpudNet:
one copy of an application hosts, others join, and they exchange opaque messages
on ordered channels (one order for everyone, set by the host) and latest channels
(newest value per key), with a snapshot for whoever joins late. It replaced the
Aprend scene replicator on 2026-10-08 and knows nothing about scenes, nodes or
BIM. What the bytes mean belongs to its caller.

## State of the code

- **macOS has run** (2026-10-08). It builds with the `apricotfields-macos-metal`
  preset with no errors or warnings, as written, and `apsync_tests` passes: 53
  runs in a row, and in ctest beside the other three. Windows and Linux have not
  been compiled. `src/sync/apsyncnet.cpp` and `apsync_internal.hpp` are back in
  `CMakeLists.txt`.
- **One test program, in ctest:** `tests/apsync_tests.c` runs a host and its
  joiners in one process over 127.0.0.1. Loopback is not a network, so it checks
  what ApSync does with messages and nothing about a slow, lossy or dropped link.
  It also sends nothing large: backlog, a peer that stops reading, a snapshot of
  more than one piece and the silence time-out are all untried.
- **It needed one thing SpudNet didn't have:** `spudnet_wait_set_wake`, added to
  `spudnet.h` and `spudnetwaitset.c` the same day. A wait set could only be
  interrupted by `_abort`, which is permanent, and ApSync's thread waits on every
  connection in one set and has to be told when the caller queues a message.
- **And one thing changed for it:** `spudnet_startup` became a `spudnet_instance`,
  so that a session can start the network for itself. Before, ApSync had to
  require that the process had made a once-only call.
- **Nobody calls it yet.**

## How it leans on SpudNet

`SPUDNET_TODO.md` says the Windows and Linux backends have never run and that
macOS has only run over loopback. ApSync is written to need as little of what is
unconfirmed as it can. Each line is something to revisit when the SpudNet item
behind it is settled.

| SpudNet's state | What ApSync does about it | Where |
|---|---|---|
| Only macOS has run; several results (`SYSTEM_TIMED_OUT` among them) have never been seen from any backend | Reads only two results as meaningful: success, and `TIMED_OUT` from a `SPUDNET_NO_WAIT` call. Anything else ends that connection; which failure it was only picks the words in the event | `flush_peer`, `receive_peer`, `accept_peers` |
| TCP keep-alive is a missing piece, and the system takes minutes to give a silent peer up | Its own heartbeat and silence time-out, set by the caller (`heartbeat_interval_ms`, `peer_timeout_ms`) | `run_timers` |
| Which clock a time limit counts on across a sleep differs by backend | Uses no SpudNet time limit for liveness: `std::chrono::steady_clock` throughout. Whether that counts sleep still differs by system, so a session may or may not survive one | `run_timers`, `next_timeout` |
| "Being named is a reason to try, not a promise"; drain until `TIMED_OUT` | Every receive and accept is `SPUDNET_NO_WAIT` in a loop that ends on `TIMED_OUT` | `receive_peer`, `accept_peers` |
| `SPUDNET_WAIT_FOR_ROOM` is level-triggered | Asked for only while a peer's queue couldn't be emptied; the socket is removed and re-added to change it | `set_waiting_for_room` |
| An object destroyed while still in a set isn't caught | A socket or listener is removed from the set before every destroy | `reap_peers`, `release_network`, `accept_peers` |
| The wait set is `poll()`: cost grows with members | Nothing. `max_peers` is the caller's; hundreds are fine, and epoll/kqueue would arrive behind the same header | - |
| Only Windows can stop a lookup | The lookup runs on a detached thread that does nothing else; the session waits with `resolve_timeout_ms` and never joins it | `resolve_host` |
| The platform's networking is started per `spudnet_instance`, not once per process | Each session makes an instance of its own and destroys it last. Nothing has to be called before a session is made | `create_session`, `destroy_session` |
| On Linux an instance needs a thread-safe libcurl, 7.84 or later | A session can't be made without one: `APRESULT_UNSUPPORTED` | `create_session` |
| `spudnet_error.text` is only filled by libcurl and NSError | Event messages carry the platform's number and which numbering it is in, plus the text when there is some | `describe` |
| A wildcard listener and a particular-address one on the same port start on Apple and fail elsewhere | Listen addresses go to SpudNet as given, one listener each. A host that lists `"0.0.0.0"` and `"127.0.0.1"` gets `APRESULT_APSYNC_ADDRESS_IN_USE` on Windows and Linux | `apsync_session_host` |
| IPv6 is untried beyond `::1` | `"::"` is one more listen address; a joiner tries a name's addresses in the resolver's order, one at a time | `connect_to_host` |

Open, and SpudNet's to answer:

- **Instances are new.** `spudnet_startup` became a `spudnet_instance` on
  2026-10-08, header and backends (`SPUDNET_TODO.md`, "Instances"), and ApSync
  was moved to it the same day. Run on macOS only. A joiner that gives up on a slow lookup destroys its session's
  instance while a thread is still in the system resolver, which SpudNet now
  says is in order; on Windows that leans on the resolver's own `WSAStartup`.
- **Whether a refusal arrives.** A newcomer turned away is sent the reason and
  then closed (`finish_sending`, then `destroy`). If the system drops unsent bytes
  on close, or resets because the newcomer's own bytes were unread, the joiner
  sees `APSYNC_REASON_CLOSED` or `NETWORK` instead of the refusal. The test accepts
  any of the three for that reason.
- **If `spudnet_error` moves to SpudCore** (SpudFiles has the same gap), `describe`
  is the one function that reads it.

## Not built

The rule for adding any of it is ApArchive's: a new function, or a new field at
the end of a struct, with zero meaning "as before". A change to what goes over
the wire raises `APSYNC_WIRE_VERSION`, and peers of two versions refuse each
other rather than guess.

### 1. A second transport: WebSocket

Plain TCP is unencrypted and proves nothing about who a peer is, so today ApSync
is for a local network its users trust. Reaching a host across the internet wants
TLS and something that gets through routers, which in SpudNet is the WebSocket
client.

- **What fits already.** A frame is self-contained and length-prefixed, so one
  frame is one binary message; nothing relies on frames arriving joined or split.
  Snapshots are cut into 256 KiB frames, so no message is ever larger than
  `max_message_size` plus a little - which matters because SpudNet's oversize
  detection on Apple is the least certain part of its WebSocket.
- **What doesn't.** SpudNet has a WebSocket client and no server, so a WebSocket
  session needs a relay that isn't ApSync: every peer, the host included, would
  connect outward. `WAIT_FOR_ROOM` is TCP only and a WebSocket send with
  `SPUDNET_NO_WAIT` does nothing, so a WebSocket link can't use the "send what
  fits, wait for room" queue as it is; it needs sends with a time limit, or a
  thread of its own for sending. End of connection is a result
  (`SPUDRESULT_SPUDNET_WS_CLOSED`) and not 0 bytes.
- **Where it goes.** The socket in `apsync_peer` becomes a link with four
  operations - send, receive, join the wait set, close - and only `flush_peer`,
  `receive_peer`, `set_waiting_for_room` and `reap_peers` call them.
  `APSYNC_JOIN_DESC` gains a URL and the TLS desc's fields at its end.
- **watchOS** has neither transport, so it stays `APRESULT_UNSUPPORTED` either way.
- **Linux** finds out whether its libcurl has WebSocket only at run time; a session
  asked for one would fail at connect, as an event.

### 2. An unreliable channel

A third `APSYNC_CHANNEL_MODE` for values that are better lost than late, which a
fast game needs and TCP can't give: one lost packet holds back everything behind
it. It waits on UDP in SpudNet ("Missing pieces"). Latest channels cover the
editor-style case meanwhile, because a stale value is replaced before it is sent
rather than after.

### 3. Carrying on after a dropped connection

A joiner whose connection ends starts again with a full snapshot. For a large
model on a flaky link that is the whole document again.

- HELLO already carries a `resume sequence`, always 0 and ignored.
- The host would keep its ordered log past the point its application has polled
  (a size or count limit in the desc), and answer a HELLO that names a sequence
  still in it with the messages after, in place of a snapshot request.
- Latest values aren't in any log; a resumed peer gets them the way a newcomer
  does, at the next send.

### 4. Snapshots that don't fit in memory

`apsync_session_send_snapshot` takes one block and `APSYNC_EVENT_SNAPSHOT` hands
one over, so the host holds two copies for a moment and the joiner one.

- **Sending:** begin / write / end calls on the host. The wire is already in
  pieces, so nothing changes there.
- **Receiving:** pieces as events, for a joiner that asks for them in its desc.
- **Or not through ApSync at all:** the snapshot names a document the joiner
  fetches by SpudNet's HTTP transfer, and ApSync carries only the reference and
  the sequence. Works today, in the caller.
- **Compression** is the caller's, with ApArchive's codecs if it likes.

### 5. Smaller things

- **Latest values for a newcomer.** Not kept by the host; peers re-send on
  `APSYNC_EVENT_PEER_JOINED`. Keeping them needs a rule for when a value dies with
  its sender, which is the caller's to have.
- **Who may join.** Only the application id and version are checked. The host
  application sees the newcomer's peer info with the snapshot request and can drop
  it; there is no challenge or credential, and over plain TCP one would prove
  little.
- **Numbers for the caller:** round-trip time, bytes waiting per peer.
- **A flooding peer** holds ApSync's thread for as long as it keeps sending,
  because a receive drains until nothing is left. A limit per pass is safe on TCP
  and not on a WebSocket, so it waits for the link split in item 1.
- **Accept failures.** A listener is given up after eight failed accepts in a row,
  because SpudNet reports one result for all of them. If it comes to tell a
  connection that went away from a listener that is broken, only the second
  should count.
- **If the host leaves, the session ends.** Moving the host's role to another peer
  is not planned.
