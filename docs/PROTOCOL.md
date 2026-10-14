# Chat Protocol — v0.5 (complete draft)

Shared specification for interoperability between groups (IF 2026/27).
Message names and fields come from the course slides (slide 124) and are
**fixed**. Rules marked **(slides)** are also fixed by the slides (slides 85,
123–125) and need no agreement. Everything marked **PROPOSAL** is our group's
position, already implemented in our server and client, but **not yet agreed**
with the other groups — it is open for discussion and will be revised after the
interop tests. This version becomes **v1.0** once the proposals are agreed.

What is fixed by the slides, what really needs agreement (only a handful of
points), and what breaks if two groups decide differently: see
[`DECISIONS.md`](DECISIONS.md).

Conformance scenarios: [`tests/interop/`](../tests/interop/) — plain text, run
against any server with `python3 scripts/interop.py <host> <port>`.

## 1. Transport

- TCP. Default port: **PROPOSAL** `5000`.
- **(slides, slide 85)** IPv4 and IPv6: *"Use getaddrinfo() and
  sockaddr_storage for IPv4 / IPv6 portability"*. Our server listens on both
  in a single dual-stack socket (`::` with `IPV6_V6ONLY=0`; on a machine
  without IPv6 it falls back to `0.0.0.0`). Clients should try every address
  `getaddrinfo()` returns (*"Retry another candidate"*), as ours does.

## 2. Framing

- **(slides, slide 123)** One JSON object per line ("newline-delimited JSON"),
  UTF-8 encoded, terminated by `\n` (0x0A).
- Senders MUST send `\n`; receivers MUST also accept `\r\n` (one `\r` removed).
- No pretty-printing: a message never contains a literal newline. (JSON escapes
  newlines inside strings as `\n`, so a delimiter is safe without extra escaping.)
- Empty lines (`\n` or `\r\n` alone) MUST be ignored by receivers — not an error.
- Messages may arrive split across several TCP reads, or several messages in one
  read. Receivers MUST buffer bytes and only parse complete lines.
- Maximum line length: **PROPOSAL** 4096 bytes **including** the `\n`.
  On a longer line the server replies `ERROR 400 "line too long"` and closes the
  connection. It does not try to resynchronise (after an oversized line there is
  no reliable way to know where the next message starts).
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

All ten types are implemented in our server and client: `NICK`, `MSG`, `OK`,
`CHAT`, `ERROR` since M1, `EVENT` since M2, `PRIV`, `LIST`, `QUIT`, `USERS`
since M3.

**Extension (PROPOSAL):** a private `CHAT` carries an extra field `"to"` with
the recipient's nick, so that the sender's copy can be shown as
"ana → rui":
`{"type":"CHAT","from":"ana","to":"rui","text":"hi","private":true}`.
A public `CHAT` never has `"to"` (exactly the slide's bytes). Receivers that do
not know `"to"` ignore it (rule below), so this breaks nobody.

General rules:
- Receivers MUST ignore unknown fields (allows optional extensions).
- Key order is not significant.
- Keys and `type` values are case-sensitive (`"Type"` ≠ `"type"`, `"list"` ≠ `"LIST"`).
- Our encoder writes `"type"` first and produces exactly the bytes of the
  examples above, but receivers must not rely on that.

### 3.1 Field rules — PROPOSAL

| Field | Rule |
|---|---|
| `nick` (in `NICK`), `to` (in `PRIV`) | string, 1–32 **bytes**, no spaces, no `"`, no `\`, no control characters |
| `from`, `to` (in `CHAT`), each element of `nicks` | string, 1–32 bytes, no spaces, no control characters (see "tolerant" below) |
| `text` | string, 1–1024 bytes, no control characters (so no multi-line messages) |
| `reason` | string, 0–128 bytes, no control characters; optional (absent = `""`) |
| `code` | integer 100–999 |
| `private` | boolean; optional (absent = `false`) |
| `nicks` | array of strings (may be empty) |

- "Control character" = byte < 0x20 or 0x7F.
- Lengths are in bytes, not characters (`"olá"` is 4 bytes). UTF-8 is not
  validated.
- **No spaces in nicks** so that `/msg <nick> text` can split on the first space.
- **No `"` and no `\` in nicks** (new in v0.5): JSON escapes them with 2 bytes.
  With them, a `USERS` with 64 nicks of 32 bytes could take
  64 × (64 + 2) + 63 + 28 = **4315 bytes** — more than one line. Without them,
  the worst `USERS` is 64 × (32 + 2) + 63 + 28 = **2267 bytes**.
- **Tolerant receiving:** a server MUST apply the strict nick rule to what
  clients send. A client SHOULD accept the looser rule in what servers send
  (so that one group allowing quotes in nicks does not make another group's
  client reject whole messages).
- Size bound: with these rules the largest possible message is a private
  `CHAT` with every field at its limit and the text full of `"`: 2172 bytes —
  always within one line.

## 4. Semantics

**Fixed by the slides (slides 123–124):** `NICK` must be the first message;
nicks are unique and a taken nick gets `ERROR 409 "nick taken"`; `OK` means
"last request accepted" (so an accepted `NICK` gets `OK`); `USERS` is the
reply to `LIST`; `ERROR` means "request refused"; a private message arrives as
a `CHAT` with `"private":true`; `EVENT` carries join and leave notices
(`"rui joined"`); `QUIT` is how a client leaves cleanly. Everything else in this
section is **PROPOSAL**.

Rule of thumb for requests (**PROPOSAL**): **every request gets exactly one
direct answer** — either the success answer below or an `ERROR`.

| Request | Success answer (to the sender) | Also sent to others |
|---|---|---|
| `NICK` | `OK` | `EVENT "<nick> joined"` |
| `MSG` | its own `CHAT` (`private:false`) | the same `CHAT` |
| `PRIV` | a copy of the `CHAT` (`private:true`) | the `CHAT` to the recipient only |
| `LIST` | `USERS` | — |
| `QUIT` | `OK`, then the server closes | `EVENT "<nick> left"` |

| Question | Our answer (**PROPOSAL**) |
|---|---|
| Does the sender of `MSG` receive its own `CHAT`? | **Yes.** It confirms the message reached the server, and clients must therefore **not** echo locally (it would appear twice). |
| A client request (`MSG`, `PRIV`, `LIST`) before a successful `NICK` | `ERROR 401 "nick required"`. The connection stays open. |
| `QUIT` before a successful `NICK` | Allowed: `OK`, then close. Leaving must always be possible. No `EVENT` (nobody knew the client). |
| `NICK` already used by someone connected | `ERROR 409 "nick taken"`. The connection stays open and the client may try another nick. |
| Are nicks case-sensitive? | **No, for uniqueness:** `Ana`, `ana` and `ANA` are the same nick (ASCII letters only; bytes ≥ 0x80 compare exactly). The nick is kept and shown as typed. The same comparison finds the recipient of a `PRIV`. |
| `NICK` after a successful `NICK` | `ERROR 400 "nick already set"` (no nick change). |
| S→C types (`OK`, `CHAT`, `ERROR`, `EVENT`, `USERS`) sent by a client | `ERROR 400 "unexpected type"`, in any state. |
| Unknown `type` | `ERROR 400 "unknown type"`. |
| Invalid JSON, missing field, wrong type, out of limits | `ERROR 400` with reason `invalid JSON` / `invalid field`. The connection stays open: the next line is handled normally. |
| Who is `from` in a `CHAT`? | Always the nick of the sender's session. A `"from"` field sent by a client is ignored. |
| Missing optional fields | `private` absent = `false`; `reason` absent = `""`; `to` absent = `""`. |
| What does the sender of `PRIV` receive? | A copy of the `CHAT` sent to the recipient (same bytes, with `private:true` and `"to"`). Like `MSG`: the copy is the confirmation, no local echo. |
| `PRIV` to a nick that is not in the chat | `ERROR 404 "no such nick"`. If the recipient leaves at the very moment the message is sent, the message is lost silently (the sender still gets its copy). |
| `PRIV` to oneself | Delivered once (no duplicate copy). |
| What does `USERS` contain? | Every client that has completed `NICK`, **including the one asking**, sorted alphabetically ignoring ASCII case. Clients still without a nick are not listed. |
| Does `QUIT` get an `OK` before the server closes? | **Yes.** Then the server closes the connection (it sends FIN right after the `OK`). Anything the client sent after the `QUIT` is ignored. Clients must also cope with a server that closes without the `OK`. |
| Exact `EVENT` text for join/leave | `"<nick> joined"` and `"<nick> left"` (the slide 124 example is `"rui joined"`). `QUIT` and a dropped connection give the same `"<nick> left"`. Clients should display `EVENT` as a notice, not as a chat line. |
| Who receives an `EVENT`? | Everyone **except** the client it is about: whoever joins does not need to be told they joined. |
| Does a client receive traffic before its `NICK` is accepted? | **No.** Between `accept()` and a successful `NICK` the connection exists but is not in the chat, so it gets no `CHAT` and no `EVENT`. The first line a client sees is always the answer to its own `NICK` (`OK` or `ERROR`). Clients should still not rely on this when talking to other servers — wait for `OK`/`ERROR` and display anything that arrives before it. |
| Does leaving before a successful `NICK` produce an `EVENT`? | **No.** Nobody knew that client was there. |
| What if a client stops reading (suspended laptop, `Ctrl+Z`)? | Each message the server sends must be fully accepted within **5 s**. If not, that client is disconnected (the others get `"<nick> left"`). One stuck client must never freeze the chat for everyone (slide 125). |
| Server full | `ERROR 503 "server full"`, then the server closes the connection. Our server accepts up to 64 clients. |

Rule of thumb: **strict when sending, tolerant when receiving.** A receiver never
crashes because of what a peer sent; a server answers with `ERROR` and carries on.

### 4.1 Connection states (server side)

```
            NICK ok (and free)                 MSG, PRIV, LIST
 AWAITING_NICK ─────────────────▶ READY ◀────────────────────┐
   │  MSG/PRIV/LIST → 401          │ └────────────────────────┘
   │  NICK taken    → 409          │  NICK → 400 "nick already set"
   │  QUIT → OK                    │  QUIT → OK
   ▼                               ▼
 CLOSED ◀──────────────────────────┘     (the server closes the connection)
```

Errors never change the state (except `line too long`, which closes).

## 5. Error codes

| Code | Reason(s) | When | Status |
|------|-----------|------|--------|
| 400 | `invalid JSON` | not JSON, not an object, trailing data, embedded `\0` | PROPOSAL |
| 400 | `invalid field` | missing field, wrong JSON type, outside the limits of §3.1 | PROPOSAL |
| 400 | `unknown type` | `type` is not one of the ten message types | PROPOSAL |
| 400 | `unexpected type` | a client sent a S→C type | PROPOSAL |
| 400 | `nick already set` | second `NICK` on the same connection | PROPOSAL |
| 400 | `line too long` | line over the maximum length; server then closes | PROPOSAL |
| 401 | `nick required` | `MSG`, `PRIV` or `LIST` before a successful `NICK` | PROPOSAL |
| 404 | `no such nick` | `PRIV` to a nick that is not in the chat | PROPOSAL |
| 409 | `nick taken` | `NICK` already in use (case-insensitive) | **slides** (code and reason) |
| 503 | `server full` | no room for another client; the server closes after sending it | PROPOSAL |

Clients should rely on the **code**; the reason is human-readable text and may
differ between implementations (our conformance scenarios compare codes only).
Only 409 changes what a client does (try another nick); the other codes are
shown to the user, so a different choice by another group changes what the
user reads but breaks nothing.
The codes follow the HTTP meanings (400 Bad Request, 401 Unauthorized,
404 Not Found, 409 Conflict, 503 Service Unavailable).

## 6. Example sessions

`>` client to server, `<` server to client. Mirrored by `tests/interop/`.

Normal session (`01_nick_ok`, `03_msg_chat`):

```
> {"type":"NICK","nick":"ana"}
< {"type":"OK"}
> {"type":"MSG","text":"hello"}
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

Private message (`08_priv`) — seen by `ana` (sender) and `rui` (recipient);
`zed` receives nothing:

```
ana > {"type":"PRIV","to":"rui","text":"hi"}
rui < {"type":"CHAT","from":"ana","to":"rui","text":"hi","private":true}
ana < {"type":"CHAT","from":"ana","to":"rui","text":"hi","private":true}
ana > {"type":"PRIV","to":"nobody","text":"hi"}
ana < {"type":"ERROR","code":404,"reason":"no such nick"}
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
> AAAA…(more than 4096 bytes before the \n)
< {"type":"ERROR","code":400,"reason":"line too long"}
  [connection closed]
```

## Changelog

- v0.5 — M3: `PRIV`, `LIST`, `QUIT`, `USERS` implemented; "one direct answer
  per request"; `PRIV` copy to the sender and the optional `"to"` in `CHAT`;
  `404 "no such nick"`; `409` keeps the connection open; case-insensitive
  (ASCII) nick uniqueness; `USERS` includes the requester, sorted; `QUIT` →
  `OK` + close, also before `NICK`; nicks may no longer contain `"` or `\`
  (worst-case `USERS` must fit in a line), looser rule when receiving; state
  diagram (§4.1); example sessions for M3; [`DECISIONS.md`](DECISIONS.md).
  Following the slides: IPv4 **and** IPv6 (slide 85) instead of IPv4 only;
  rules fixed by the slides are marked as such and need no agreement.
- v0.4 — M2 review: the first line after `NICK` is now guaranteed to be its
  answer (the client joins the chat only after the `OK`); a client that does not
  read for 5 s is disconnected; `ERROR 503 "server full"`; fixed the list of
  implemented types (§3).
- v0.3 — M2: `EVENT` implemented (join/leave text, who receives it); a
  connection only joins the chat after a successful `NICK`; `EVENT` added to
  the S→C types a client may not send.
- v0.2 — framing details (empty lines, max length, one object per line); field
  rules; semantics and error codes as proposals from our M1 implementation;
  example sessions; conformance scenarios in `tests/interop/`.
- v0.1 — initial draft.
