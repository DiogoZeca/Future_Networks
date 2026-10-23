# Architecture

How the chat is built, and the reasoning behind the decisions that matter. For
the wire format see [`PROTOCOL.md`](PROTOCOL.md).

## Layers

Each layer only knows the one below it, and the pure layers (framing,
protocol, session, client list, UI) are unit-tested without a network.

```
 server: main.c (threads, timeouts) ── session.c (what each request means) ── roster.c (who is online)
 client: main.c (threads, keepalive) ── ui.c (commands ↔ messages) ── term.c (the input line)
                        │
                 protocol.c   lines  ↔ validated JSON messages
                 framing.c    bytes  ↔ lines
                 tlsio.c      the same operations, with or without TLS
                 net.c        sockets: connect, listen, send with a deadline, close cleanly
```

`session.c` never touches a socket: it receives a parsed message and returns
replies tagged with a destination (`SELF`, `ALL`, `OTHERS`, one nick). That is
what lets every protocol rule be tested in isolation.

## TCP is a byte stream

`recv()` does not return messages. One call can bring half a line, or three
lines glued together. The framing layer buffers bytes and extracts **every**
complete line in a loop (a `while`, not an `if`). Every request then goes
through the same checks, in order — and only the first one closes the
connection:

```
bytes arrive
   │
   ▼
1. FRAMING   a '\n' within 64 KiB?
   ├─ no  → 413 "line too long" and CLOSE   (we no longer know where the next message starts)
   └─ yes → a complete line ↓
2. JSON      valid JSON, valid UTF-8, a single object?
   ├─ no  → 400 "invalid JSON"
   └─ yes ↓
3. FIELDS    known type, right fields, within limits?
   ├─ no  → 400 "unknown type" / "invalid field" / "text too long"
   └─ yes ↓
4. SESSION   does it make sense now?
   ├─ before NICK → 401   ├─ nick taken → 409   ├─ PRIV to nobody → 404
   └─ ok → OK (+ CHAT, USERS, …)
```

## Server concurrency

**One thread per client.** Simple, and each slow client only blocks its own
thread. The shared state is the client list (`roster.c`), behind one lock.

**Broadcasting never sends while holding the list lock.** If it did, one
client that stops reading would block `send()` with the lock held, and nobody
could join, leave or talk. Instead:

```
1. lock(list); copy the recipients (a snapshot), take a reference on each; unlock(list)
2. for each recipient, outside the list lock:
      lock(that client's write lock)        → lines to one socket never interleave
      send with a TOTAL deadline of 5 s     → a client that does not read is cut off
      unlock
3. drop the references                      → the socket closes only when nobody holds it
```

Each piece is needed. Sending outside the lock without a deadline still froze
(the first version did). Without reference counting, a client leaving during a
broadcast could have its socket closed and the descriptor number reused by a
new connection — and the message would go to the wrong person.

**Nicknames are claimed atomically.** Checking "is it free?" and reserving it
happen under the same lock (`roster_claim`); with two separate steps, two
simultaneous `NICK ana` could both succeed.

**A joining client sees its `OK` first.** It is added to the chat only after
the `OK` has been sent, so no broadcast can overtake it.

## Connections: how they die, and how we notice

| What happens | How the server finds out |
|---|---|
| the client process exits or is killed | immediately: the client's kernel sends FIN (or RST) |
| Wi-Fi drops, cable pulled, machine suspended | nothing arrives — only a **timeout** can tell: `408` after 300 s idle (30 s before `NICK`) |
| the client stops reading | its buffers fill up; the 5 s send deadline cuts it off |

The client has the mirror problem: a server that vanishes without FIN would
leave it waiting forever. So after 120 s without sending anything, it sends a
**silent `LIST`** and hides the `USERS` reply; no answer within another 120 s
means the server is gone. Using `LIST` instead of a new `PING` message keeps it
compatible with every other group's server — and it also stops our users from
getting the `408` while they are just reading.

**Closing without losing the last message.** When the server closes (after
`QUIT`, `413`, `408`, shutdown), it sends the reply, then `shutdown(SHUT_WR)`
(FIN), then reads and discards whatever the client still sends until it
closes — at most 2 s. Calling `close()` with unread data makes the kernel send
RST, and an RST can make the client discard the reply it had not read yet.

**Shutdown (`Ctrl+C`).** The signal handler only sets a flag and writes one
byte to a pipe that every thread's `poll()` also watches. Each thread tells its
client `server shutting down` and closes cleanly; the server waits at most 7 s
for all of them and exits with 0.

## Discovery

UDP, because only UDP can broadcast — opening a TCP connection requires already
knowing the address you are trying to discover. The client sends
`{"type":"DISCOVER"}` to `255.255.255.255` and to each interface's broadcast
address, three times over 1.5 s (UDP may drop packets silently). Servers reply
**unicast to the asker** (the client listens on an ephemeral port, not 5001),
and the client uses the reply's **source IP** as the server address — the
server may have several interfaces or sit behind NAT, and the source address
is one that is known to work. On the server, the UDP socket lives in the same
`poll()` as `accept()`; no extra thread.

## TLS

- **A separate port.** Port 5000 stays plaintext for the other groups' clients
  and for `nc`; TLS is an extra door, like HTTP and HTTPS.
- **Pinning instead of a CA.** The certificate is self-signed, so chain
  validation would always fail. The client checks that the SHA-256 of the
  certificate the server presents matches the fingerprint it was given, and
  aborts before sending `NICK` otherwise. The handshake proves the server holds
  the private key, so presenting our (public) certificate is not enough for an
  impostor. Without a fingerprint the link is encrypted but not authenticated:
  a man in the middle would read everything (demonstrated with a fake
  `openssl s_server`).
- **TLS is not transparent to concurrency.** With plain TCP the two directions
  of a socket are independent, so the client needed no lock at all. An `SSL*`
  object is shared by both directions and must never be used by two threads at
  once. Every `SSL_read`/`SSL_write`/`SSL_shutdown` is done under a lock, and
  the sockets are non-blocking after the handshake so that **no call ever
  waits while holding that lock**. The client ended up with two locks: one
  that keeps whole lines in order (may be held while waiting for buffer space)
  and one that protects the `SSL*` (held only for each call).
- **The handshake has a deadline.** It runs in the client's own thread with
  a deadline, so a port scanner that connects and says nothing cannot block the
  server.

## Known limits

- **One thread per client** does not scale to thousands; the next step would
  be `epoll` with a fixed pool of threads.
- **Broadcast is O(N) per message**, so O(N²) when everybody talks — inherent
  to a single room; rooms/channels would cut it.
- **64 clients** is a compile-time constant (`ROSTER_MAX`), chosen so that a
  `USERS` reply with everyone always fits in one line.
- **One server, no persistence, no user authentication.** State lives in
  memory; TLS authenticates the server, not the users.
- Discovery is IPv4 only (IPv6 has no broadcast); the chat itself is IPv4 and
  IPv6 on a single dual-stack socket.

## Testing approach

- **Unit tests** for every pure layer (framing, protocol, session, roster,
  discovery datagrams, UI), with no network.
- **End-to-end tests** (pytest) start the real server and client on real
  sockets: slow readers, abrupt disconnects, idle timeouts, races on the same
  nickname, discovery, TLS with right/wrong/missing fingerprints.
- **Mutation checks**: each important fix was undone on purpose to confirm a
  test fails without it.
- **Sanitizers**: the whole suite runs under AddressSanitizer/UBSan, and the
  concurrency and TLS tests under ThreadSanitizer. ThreadSanitizer cannot see
  inside the prebuilt OpenSSL library, so the TLS locking rests on OpenSSL's
  documentation, not on that tool.
