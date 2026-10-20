# Chat Protocol — v0.10

Shared specification for interoperability between groups (IF 2026/27).
Message names and fields come from the course slides (slide 124) and are
**fixed**. Every other rule is marked with where it comes from:

- **(slides)** — fixed by the slides (slides 85, 123–125); needs no agreement.
- **AGREED** — agreed with another group (so far: group 1, our first
  contact; see the contact log in [`DECISIONS.md`](DECISIONS.md)).
- **PROPOSAL** — our group's position, implemented in our server and client,
  not yet agreed with anyone.

This version becomes **v1.0** once every point that can break
interoperability (`DECISIONS.md`, section A) is AGREED.

Conformance scenarios: [`tests/interop/`](../tests/interop/) — plain text, run
against any server with `python3 scripts/interop.py <host> <port>`.

## 1. Transport

- TCP. Default port: **AGREED** `5000` (group 1 uses 5000 too).
- Server discovery (M5): UDP broadcast, port `5001` — see §7.
- **TLS (M6): a SEPARATE port, `5443` by default — see §8.** The plaintext
  port keeps working unchanged; TLS is an extra door, never a replacement.
  Only our group chose this extension, so nothing here affects interop.
- **(slides, slide 85)** IPv4 and IPv6: *"Use getaddrinfo() and
  sockaddr_storage for IPv4 / IPv6 portability"*. Our server listens on both
  in a single dual-stack socket (`::` with `IPV6_V6ONLY=0`; on a machine
  without IPv6 it falls back to `0.0.0.0`). Clients should try every address
  `getaddrinfo()` returns (*"Retry another candidate"*), as ours does.

## 2. Framing

- **(slides, slide 123)** One JSON object per line ("newline-delimited JSON"),
  UTF-8 encoded, terminated by `\n` (0x0A).
- Senders MUST send `\n`; receivers MUST also accept `\r\n` (one `\r` removed).
- No pretty-printing: a message never contains a literal newline. Newlines
  inside strings are escaped by JSON (`\n`), so a delimiter is safe — even for
  multi-line messages (§3.1).
- Empty lines (`\n` or `\r\n` alone) MUST be ignored by receivers — not an error.
- Messages may arrive split across several TCP reads, or several messages in one
  read. Receivers MUST buffer bytes and only parse complete lines.
- Maximum line length: **AGREED** **64 KiB (65536 bytes) including the `\n`**.
  On a longer line the server replies **`ERROR 413 "line too long"`** and
  closes the connection: the server does not try to resynchronise. A client
  receiving an over-long line from a server MAY discard it up to the next
  `\n` and carry on (ours does).
- A line must contain exactly one JSON object: `{...}{...}` or trailing
  non-whitespace after the object is invalid (`ERROR 400 "invalid JSON"`).
  Whitespace around the object is allowed.

## 3. Messages (fixed by the slides)

| Direction | Type    | Example                                                        | Meaning                              |
|-----------|---------|----------------------------------------------------------------|--------------------------------------|
| C → S     | `NICK`  | `{"type":"NICK","nick":"ana"}`                                 | Register nickname; must be the first message |
| C → S     | `MSG`   | `{"type":"MSG","text":"hello"}`                                | Send to everyone                     |
| C → S     | `PRIV`  | `{"type":"PRIV","to":"rui","text":"hi"}`                       | Private message (`/msg`)             |
| C → S     | `LIST`  | `{"type":"LIST"}`                                              | Ask for connected users              |
| C → S     | `QUIT`  | `{"type":"QUIT"}`                                              | Leave cleanly                        |
| S → C     | `OK`    | `{"type":"OK"}`                                                | Last request accepted                |
| S → C     | `CHAT`  | `{"type":"CHAT","from":"ana","text":"hello","private":false}`  | A message to display                 |
| S → C     | `USERS` | `{"type":"USERS","nicks":["ana","rui"]}`                       | Reply to `LIST`                      |
| S → C     | `EVENT` | `{"type":"EVENT","text":"rui joined"}`                         | Join and leave notices               |
| S → C     | `ERROR` | `{"type":"ERROR","code":409,"reason":"nick taken"}`            | Request refused                      |

All ten types are implemented in our server and client. Our server sends
exactly these fields — no extensions (the `"to"` field that v0.5 added to
private `CHAT`s was dropped in v0.6).

General rules:
- Receivers MUST ignore unknown fields (**AGREED**).
- Key order is not significant.
- Keys and `type` values are case-sensitive (`"Type"` ≠ `"type"`, `"list"` ≠
  `"LIST"`) — **(slides)**: the names are the slide's, and JSON compares names
  exactly.
- `CHAT` always carries `"private"` (`true` or `false`) — **AGREED**.
- Our encoder writes `"type"` first and produces exactly the bytes of the
  examples above, but receivers must not rely on that.

### 3.1 Field rules

| Field | Rule | Status |
|---|---|---|
| `text` (in `MSG`, `PRIV`) | string, **1–4096 bytes** (after UTF-8 encoding). **Multi-line allowed**: any character except NUL, so `\n` and `\t` are fine (JSON escapes them). Empty → `ERROR 400 "invalid field"`; over 4096 → `ERROR 400 "text too long"`, the connection stays open | 4096 and multi-line: **AGREED**. Empty refused: PROPOSAL (group 1 accepts it) |
| `nick` (in `NICK`), `to` (in `PRIV`) | string, 1–32 **bytes**, no spaces, no `"`, no `\`, no control characters | PROPOSAL (group 1 counts characters) |
| `from` (in `CHAT`), each element of `nicks` | string, 1–32 bytes, no spaces, no control characters (see "tolerant" below) | PROPOSAL |
| `text` (in `CHAT`, `EVENT`) | string; see "tolerant" below | PROPOSAL |
| `reason` | string, 0–128 bytes, no control characters; optional (absent = `""`) | PROPOSAL |
| `code` | integer 100–999 | (slides): a JSON number |
| `private` | boolean, always sent; receivers treat absent as `false` | AGREED |
| `nicks` | array of strings (may be empty) | (slides) |

- "Control character" = byte < 0x20 or 0x7F.
- Lengths are in bytes, not characters (`"olá"` is 4 bytes). UTF-8 is not
  validated.
- **No spaces in nicks** so that `/msg <nick> text` can split on the first space.
- **No `"` and no `\` in nicks**: JSON escapes them with 2 bytes. With them, a
  `USERS` with 64 nicks of 32 bytes could take 64 × (64 + 2) + 63 + 28 = 4315
  bytes; without them, at most 2267. (With the 64 KiB line both fit, but a
  nick with quotes is still asking for trouble in other groups' code.)
- **Tolerant receiving:** a server MUST apply the strict rules to what clients
  send. A client SHOULD accept more in what servers send, so that one group's
  choice does not make another group's client reject whole messages. Ours
  accepts nicks with `"`/`\`, and a `CHAT`/`EVENT` text or an `ERROR` reason
  of any length (empty included); whatever does not fit its buffers is cut at
  a UTF-8 character boundary and shown with " [...]".
- **Displaying text:** because `text` may contain control characters, a
  client MUST NOT write it raw to a terminal (`\033` sequences could clear
  the screen or fake other messages). Ours keeps `\n` (indenting the
  following lines) and `\t`, and shows other control characters as `?`.
- Size bound: with these rules the largest possible message is a `CHAT`
  whose text is 4096 control characters, each escaped as `\u00XX` (6 bytes):
  24660 bytes — always within one 64 KiB line.

## 4. Semantics

**Fixed by the slides (slides 123–124):** `NICK` must be the first message;
nicks are unique and a taken nick gets `ERROR 409 "nick taken"`; `OK` means
"last request accepted"; `USERS` is the reply to `LIST`; `ERROR` means
"request refused"; a private message arrives as a `CHAT` with
`"private":true`; `EVENT` carries join and leave notices (`"rui joined"`);
`QUIT` is how a client leaves cleanly.

**AGREED** with group 1 — read literally, *"OK — last request accepted"*:
every accepted request gets an `OK`, and the `OK` always comes **before** any
`CHAT` the request causes. `LIST` is answered by its `USERS` (*"Reply to
LIST"*). A refused request gets only its `ERROR`.

| Request | To the sender | To others |
|---|---|---|
| `NICK` | `OK`, then a welcome `EVENT` (PROPOSAL, see below) | `EVENT "<nick> joined"` |
| `MSG` | `OK`, then its own `CHAT` (`private:false`) | the same `CHAT` |
| `PRIV` | `OK` only — **no copy** | the `CHAT` (`private:true`) to the recipient only |
| `LIST` | `USERS` | — |
| `QUIT` | `OK`, then the server closes | `EVENT "<nick> left"` |

Clients do not need to wait for the `OK` (ours and group 1's do not): they
send and keep reading. The `OK` only matters after `NICK` (was the nick
accepted?) and, optionally, after `QUIT`.

| Question | Answer | Status |
|---|---|---|
| Does the sender of `MSG` receive its own `CHAT`? | **Yes.** Clients must therefore **not** echo `MSG` locally (it would appear twice). | AGREED |
| What does the sender of `PRIV` receive? | **Only the `OK`.** A `CHAT` is "a message that reached you"; a copy would change that meaning. A client that wants to show what it sent does it **locally** (ours shows `<ana -> rui> (privado) …`). | AGREED |
| `PRIV` to a nick that is not in the chat | `ERROR 404 "no such nick"` (no `OK`). If the recipient leaves at the very moment the message is sent, the message is lost silently (the request was still accepted). | AGREED (404) |
| `PRIV` to oneself | `OK`, then the `CHAT`, once. | PROPOSAL |
| A request (`MSG`, `PRIV`, `LIST`) before a successful `NICK` | `ERROR 401 "nick required"`. The connection stays open. | AGREED |
| `QUIT` before a successful `NICK` | Allowed: `OK`, then close. Leaving must always be possible. No `EVENT` (nobody knew the client). | PROPOSAL |
| Does `QUIT` get an `OK` before the server closes? | **Yes**, then the server closes (FIN right after the `OK`). Anything sent after the `QUIT` is ignored. An `EVENT` caused by another connection at the same instant may still arrive between the `OK` and the close. Clients should also cope with a server that closes without the `OK`. | AGREED |
| `NICK` already used by someone connected | `ERROR 409 "nick taken"`. The connection stays open and the client may try another nick. | (slides) for 409; staying open AGREED |
| Are nicks case-sensitive? | **No, for uniqueness:** `Ana`, `ana` and `ANA` are the same nick (ASCII letters only; bytes ≥ 0x80 compare exactly). Kept and shown as typed. The same comparison finds the recipient of a `PRIV`. | PROPOSAL (group 1 is case-sensitive — harmless: it only changes which nicks a server allows) |
| `NICK` after a successful `NICK` | `ERROR 400` (no nick change). | AGREED |
| S→C types (`OK`, `CHAT`, `ERROR`, `EVENT`, `USERS`) sent by a client | `ERROR 400 "unexpected type"`, in any state. | PROPOSAL |
| Unknown `type` | `ERROR 400 "unknown type"`; the connection stays open. | AGREED |
| Invalid JSON, missing field, wrong type, out of limits | `ERROR 400` (`invalid JSON` / `invalid field` / `text too long`). The connection stays open: the next line is handled normally. | AGREED |
| Who is `from` in a `CHAT`? | Always the nick of the sender's session. A `"from"` field sent by a client is ignored. | PROPOSAL |
| What does `USERS` contain? | Every client that has completed `NICK`, **including the one asking**, sorted alphabetically ignoring ASCII case. Clients still without a nick are not listed. | AGREED (group 1 also lists users of linked servers) |
| Exact `EVENT` text for join/leave | `"<nick> joined"` and `"<nick> left"` (the slide 124 example is `"rui joined"`). `QUIT` and a dropped connection give the same `"<nick> left"`. Clients display `EVENT` as a notice. | AGREED |
| Who receives a join/leave `EVENT`? | Everyone **except** the client it is about. | AGREED |
| Is there any help for whoever joins? | **Yes:** right after the `OK` to `NICK`, an `EVENT` **only to that client**: `"welcome, <nick>! commands: /list, /msg <nick> <text>, /quit"` (the commands of slide 123). It is the server-side `/help` for clients without one (`nc`, other groups' clients) — slide 124 has no `HELP` type, and every client already displays `EVENT`s. Clients must show an `EVENT` they do not recognise as a plain notice, and must not expect any particular line after the `OK` (other traffic can come first). | PROPOSAL |
| Does a client receive traffic before its `NICK` is accepted? | **No.** The first line a client sees is always the answer to its own `NICK` (`OK` or `ERROR`). Clients should still wait for `OK`/`ERROR` and display anything that arrives before it. | PROPOSAL |
| Does leaving before a successful `NICK` produce an `EVENT`? | **No.** Nobody knew that client was there. | PROPOSAL |
| What if a client stops reading (suspended laptop, `Ctrl+Z`)? | Each message the server sends must be fully accepted within **5 s**; otherwise that client is disconnected (the others get `"<nick> left"`). The mechanism and the value are each server's choice — what matters is that one stuck client never freezes the chat (slide 125). Group 1 uses a send queue (1 MiB / 1000 messages). | (slides): something must exist |
| Server full | `ERROR 503 "server full"`, then the server closes. Our server accepts up to 64 clients. | PROPOSAL |
| Idle connections (M4) | No **message** (complete line) for **300 s** → `ERROR 408 "idle timeout"` and close. Before a successful `NICK` the limit is **30 s from connecting**, not renewed by refused lines → `408 "nick timeout"`. **A client whose user is only reading must send something** before 300 s: ours sends a `LIST` every 120 s without sending anything else, and hides that `USERS` (no new message type needed). | AGREED (g1: 300 s, 408) / PROPOSAL (30 s, keepalive with `LIST`) |
| Server shutdown (Ctrl+C) (M4) | Every connection (also those without a `NICK`) receives `EVENT "server shutting down"`, then the server closes it. No `"<nick> left"` is sent. | PROPOSAL |
| How the server closes (M4) | Whenever the **server** closes (`QUIT`, 408, 413, 503, shutdown) it sends its last line, then FIN, and reads and discards whatever the client still sends until the client closes (max. 2 s). So the last line is never lost to a TCP reset. | PROPOSAL (needs no agreement: only our side changes) |

Rule of thumb: **strict when sending, tolerant when receiving.** A receiver never
crashes because of what a peer sent; a server answers with `ERROR` and carries on.

### 4.1 Connection states (server side)

```
            NICK ok (and free)                 MSG, PRIV → OK (+ CHAT)
 AWAITING_NICK ─────────────────▶ READY ◀────── LIST → USERS ─┐
   │  MSG/PRIV/LIST → 401          │ └─────────────────────────┘
   │  NICK taken    → 409          │  NICK → 400
   │  QUIT → OK                    │  QUIT → OK
   ▼                               ▼
 CLOSED ◀──────────────────────────┘     (the server closes the connection)
```

Errors never change the state, except a line over 64 KiB (413), which closes.

## 5. Error codes

| Code | Reason(s) | When | Status |
|------|-----------|------|--------|
| 400 | `invalid JSON` | not JSON, not an object, trailing data, embedded `\0`, **bytes that are not valid UTF-8** (RFC 8259 §8.1) | AGREED (code) |
| 400 | `invalid field` | missing field, wrong JSON type, outside the limits of §3.1, empty `text` | AGREED (code) |
| 400 | `text too long` | `text` of a `MSG`/`PRIV` over 4096 bytes; the connection stays open | AGREED |
| 400 | `unknown type` | `type` is not one of the ten message types | AGREED (code) |
| 400 | `unexpected type` | a client sent a S→C type | PROPOSAL |
| 400 | `nick already set` | second `NICK` on the same connection | AGREED (code) |
| 401 | `nick required` | `MSG`, `PRIV` or `LIST` before a successful `NICK` | AGREED |
| 404 | `no such nick` | `PRIV` to a nick that is not in the chat | AGREED |
| 408 | `idle timeout` / `nick timeout` | no message for 300 s / no successful `NICK` within 30 s of connecting; the server then **closes** | AGREED (g1: 408, 300 s) / PROPOSAL (30 s) |
| 409 | `nick taken` | `NICK` already in use | **(slides)** (code and reason) |
| 413 | `line too long` | line over 64 KiB; the server then **closes** | AGREED |
| 503 | `server full` | no room for another client; the server closes after sending it | PROPOSAL |

Clients rely on the **code**; the reason is human-readable text and may differ
between implementations (our conformance scenarios compare codes, plus the
`text too long` reason). Only 409 changes what a client does (try another
nick); the others are shown to the user.

The codes follow the HTTP meanings (400 Bad Request, 401 Unauthorized,
404 Not Found, 408 Request Timeout, 409 Conflict, 413 Content Too Large,
503 Service Unavailable). The distinction agreed with group 1: **413 = the
connection is no longer usable and is closed; 400 = request refused, carry
on.** 408 and 503 also close.

## 6. Example sessions

`>` client to server, `<` server to client. Mirrored by `tests/interop/`.

Normal session (`01_nick_ok`, `03_msg_chat`):

```
> {"type":"NICK","nick":"ana"}
< {"type":"OK"}
< {"type":"EVENT","text":"welcome, ana! commands: /list, /msg <nick> <text>, /quit"}
> {"type":"MSG","text":"hello"}
< {"type":"OK"}
< {"type":"CHAT","from":"ana","text":"hello","private":false}
```

Request before `NICK` (`02_msg_antes_de_nick`, `12_m3_antes_de_nick`) — the
connection stays open:

```
> {"type":"MSG","text":"too early"}
< {"type":"ERROR","code":401,"reason":"nick required"}
> {"type":"NICK","nick":"ana"}
< {"type":"OK"}
```

Invalid line (`04_json_invalido`) — the connection stays open:

```
> this is not json
< {"type":"ERROR","code":400,"reason":"invalid JSON"}
```

Unknown fields are ignored, and the client cannot choose `from` (`05_campos_extra`):

```
> {"type":"NICK","nick":"ana","version":2}
< {"type":"OK"}
> {"type":"MSG","text":"hi","from":"rui","zz":[1,2]}
< {"type":"OK"}
< {"type":"CHAT","from":"ana","text":"hi","private":false}
```

Nick taken (`07_nick_em_uso`) — `ana` is already connected; the second
connection tries again:

```
> {"type":"NICK","nick":"ANA"}
< {"type":"ERROR","code":409,"reason":"nick taken"}
> {"type":"NICK","nick":"rui"}
< {"type":"OK"}
```

Private message (`08_priv`, `09_priv_inexistente`) — `ana` gets only the `OK`,
`rui` gets the `CHAT`, `zed` receives nothing:

```
ana > {"type":"PRIV","to":"rui","text":"hi"}
ana < {"type":"OK"}
rui < {"type":"CHAT","from":"ana","text":"hi","private":true}
ana > {"type":"PRIV","to":"nobody","text":"hi"}
ana < {"type":"ERROR","code":404,"reason":"no such nick"}
```

Multi-line message (`13_varias_linhas`) and text too long (`14_text_too_long`):

```
> {"type":"MSG","text":"line 1\nline 2"}
< {"type":"OK"}
< {"type":"CHAT","from":"ana","text":"line 1\nline 2","private":false}
> {"type":"MSG","text":"xxxx…(4097 bytes)"}
< {"type":"ERROR","code":400,"reason":"text too long"}
  [the connection stays open]
```

List (`10_list`):

```
> {"type":"LIST"}
< {"type":"USERS","nicks":["ana","rui","zed"]}
```

Leaving (`11_quit`):

```
rui > {"type":"QUIT"}
rui < {"type":"OK"}
      [connection closed by the server]
ana < {"type":"EVENT","text":"rui left"}
```

Line too long — the server closes after the error:

```
> AAAA…(more than 65536 bytes before the \n)
< {"type":"ERROR","code":413,"reason":"line too long"}
  [connection closed]
```

(The rest of the line is read and discarded before the server closes, so the
413 always arrives — M4.)

Idle and shutdown (M4):

```
> {"type":"NICK","nick":"ana"}
< {"type":"OK"}
< {"type":"EVENT","text":"welcome, ana! commands: /list, /msg <nick> <text>, /quit"}
  [300 s without sending anything]
< {"type":"ERROR","code":408,"reason":"idle timeout"}
  [connection closed]

  [another connection, when the server is stopped with Ctrl+C]
< {"type":"EVENT","text":"server shutting down"}
  [connection closed]
```

## 7. Discovery (M5)

**(slides, slide 123)** *"The server answers a UDP broadcast (DISCOVER) with
its TCP port; the client finds servers on the LAN with no address typed in."*
Everything else in this section is ours (PROPOSAL) or taken from what group 1
told us (marked *as group 1*) — **none of it has been tested between groups
yet**; the open points are listed in `DECISIONS.md`, section "M5".

### 7.1 Transport

- UDP over **IPv4**, server port **`5001`** (*as group 1*; PROPOSAL until
  tested). IPv6 has no broadcast (the equivalent would be multicast to
  `ff02::1`): out of scope.
- One message per **datagram** — no framing, no `\n` needed. A datagram
  arrives whole or not at all.
- Maximum datagram: **512 bytes**, both ways. Larger datagrams are ignored.
- Datagrams MUST be valid UTF-8 JSON (RFC 8259 §8.1), like the TCP lines.

### 7.2 Request (client → broadcast)

```
{"type":"DISCOVER"}
```

- Sent to `255.255.255.255:5001` and/or the directed broadcast of each
  interface (e.g. `192.168.1.255:5001`), from any (ephemeral) port.
- Our client sends exactly these 19 bytes, without `\n`, **3 times** over
  1.5 s (UDP loses packets silently), and waits the whole 1.5 s.
- A server MUST also accept: a trailing `\n` / `\r\n`, whitespace around
  the object, extra fields, and the plain text `DISCOVER` (PROPOSAL, as
  tolerance). `type` is case-sensitive: `discover` is not a request.

### 7.3 Response (server → the requester only)

```
{"type":"SERVER","tcp_port":5000,"name":"diogolap","users":3}
```

| Field | Rule | Status |
|---|---|---|
| `type` | `"SERVER"` | *as group 1* |
| `tcp_port` | integer 1–65535: the TCP port of the chat (§1) | *as group 1* — the slide's *"its TCP port"* |
| `name` | string, 1–64 bytes, no control characters: a human name for the server (our default: the hostname) | PROPOSAL (optional) |
| `users` | integer ≥ 0: how many clients are **in the chat** (completed `NICK`) | PROPOSAL (optional) |

- Sent **unicast** to the source address and port of the request — never by
  broadcast.
- Our server ends it with `\n`; receivers MUST accept it with or without.
- **The server's address is the source IP of the response datagram.** The
  response carries no IP, and a client MUST ignore any `ip`/`host` field: a
  server with several interfaces (Wi-Fi, Ethernet, docker0, VPN) cannot know
  which of its addresses the client can reach; the datagram that reached the
  client can.
- A server MUST NOT answer anything that is not a request (other types,
  invalid JSON, oversized datagrams): no `ERROR` over UDP. Otherwise two
  servers could answer each other forever, and the server would amplify
  traffic towards a spoofed address.

### 7.4 What our client accepts (tolerant receiving)

Any JSON object, up to 512 bytes, with `type` absent or `"SERVER"` and the
port in **`tcp_port`** or, failing that, **`port`** (integer 1–65535).
`name` and `users` are optional and ignored if malformed; control characters
in `name` are shown as `?`. Duplicate responses (3 requests × several
broadcast addresses) are merged by (source IP, port). The list is shown
sorted (named servers first, by name ignoring case; then IP and port).

### 7.5 Example

```
client 192.168.1.23:41234  →  255.255.255.255:5001   {"type":"DISCOVER"}
client 192.168.1.23:41234  →  192.168.1.255:5001     {"type":"DISCOVER"}
server 192.168.1.10:5001   →  192.168.1.23:41234     {"type":"SERVER","tcp_port":5000,"name":"diogolap","users":3}\n
client: TCP connect to 192.168.1.10:5000, then NICK as usual (§3)
```

## 8. TLS (M6) — our extension

Slide 123 lists *"TLS with a self-signed certificate"* as one of four possible
extensions. **Only our group picked it**, so this section describes what we do,
not something agreed between groups. Nothing here changes the plaintext
protocol: a server with TLS enabled behaves on port 5000 exactly as before.

### Transport

| | |
|---|---|
| Port | `5443` by default (`-T`); **separate** from the plaintext port |
| Version | TLS 1.2 minimum; 1.3 when both sides support it |
| Certificate | self-signed, generated per machine by `scripts/gen-cert.sh` |
| Inside TLS | exactly the same newline-delimited JSON as §2 |

A client that speaks TLS to port 5000, or plaintext to 5443, gets a clean
error, not a hang.

### Authentication: fingerprint pinning

A self-signed certificate cannot be validated against any authority, so **chain
validation is disabled on purpose** and replaced by a check our client does
itself: the SHA-256 of the server certificate must match what the user passed
in `-f`.

```sh
./bin/client -s -f 52:59:68:E4:...:29 127.0.0.1 5443 ana
```

Without `-f` the connection is still encrypted, but **nobody is
authenticated** — a man in the middle would present its own certificate and be
accepted. The client says so explicitly and prints the fingerprint it saw.

The fingerprint travels out of band (the message where the test is arranged,
the board in class); `scripts/gen-cert.sh` prints it on the server side.

### If another group ever wants to interoperate over TLS

These would have to be agreed first — they are **proposals**, not decisions:

| # | Question | Our proposal |
|---|---|---|
| M6-1 | Default TLS port | `5443` |
| M6-2 | Minimum version | TLS 1.2 |
| M6-3 | Does `DISCOVER` advertise a `tls_port`? | yes, as an optional field (**not implemented**: discovery finds only the plaintext port, and our client refuses `-s` without a host instead of guessing) |
| M6-4 | How is the fingerprint distributed? | out of band |

## Changelog

- v0.10 — M6: TLS on a separate port (§8, §1), with self-signed certificates
  and SHA-256 fingerprint pinning instead of chain validation. The plaintext
  protocol is unchanged: this is our extension, not an agreement — so it does
  NOT make this v1.0, which stays reserved for the version agreed between
  groups (see the top of this file).

- v0.9 — M5: server discovery over UDP (§7): `{"type":"DISCOVER"}` by
  broadcast to port 5001, answered only to the requester with
  `{"type":"SERVER","tcp_port":N,"name":...,"users":...}`; the server's
  address is the source IP of the answer; anything else gets no answer; our
  client also reads `port`. Not yet tested between groups.

- v0.8 — M4: `ERROR 408` (`idle timeout` after 300 s without a message, as
  group 1; `nick timeout` 30 s after connecting without a `NICK`); clients
  that only read should send a `LIST` before 300 s; `EVENT "server shutting
  down"` before the server closes on Ctrl+C; bytes that are not valid UTF-8
  → `400 "invalid JSON"`; the server never loses its last line to a reset.

- v0.7 — fixes after the first cross-test: a welcome `EVENT` with the
  commands, only to the client that joined, right after the `OK` to `NICK`
  (PROPOSAL). `interop.py` skips `EVENT`s with another text while it waits
  for a specific one, so the scenarios pass against servers with or without
  it.

- v0.6 — first agreement with another group (group 1): `OK` to every accepted
  request (`MSG` and `PRIV` too), always before the `CHAT`; `PRIV` without a
  copy to the sender and without the `"to"` field (the sender echoes
  locally); line up to 64 KiB, `ERROR 413` above it (was 4096, `400`);
  `text` up to 4096 bytes, multi-line allowed, `ERROR 400 "text too long"`
  above it (was 1024, no control characters); clients must sanitise text
  before displaying it; tolerant receiving of long/empty texts. Status of
  every rule marked as (slides) / AGREED / PROPOSAL.
- v0.5 — M3: `PRIV`, `LIST`, `QUIT`, `USERS` implemented; `404 "no such
  nick"`; `409` keeps the connection open; case-insensitive (ASCII) nick
  uniqueness; `USERS` includes the requester, sorted; `QUIT` → `OK` + close,
  also before `NICK`; nicks without `"` or `\`; state diagram (§4.1);
  IPv4 **and** IPv6 (slide 85).
- v0.4 — M2 review: the first line after `NICK` is its answer; a client that
  does not read for 5 s is disconnected; `ERROR 503 "server full"`.
- v0.3 — M2: `EVENT` implemented (join/leave text, who receives it); a
  connection only joins the chat after a successful `NICK`.
- v0.2 — framing details (empty lines, max length, one object per line); field
  rules; semantics and error codes as proposals from our M1 implementation;
  example sessions; conformance scenarios in `tests/interop/`.
- v0.1 — initial draft.
