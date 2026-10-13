# Chat Protocol — v0.4 (draft)

Shared specification for interoperability between groups (IF 2026/27).
Message names and fields come from the course slides (slide 124) and are
**fixed**. Everything marked **PROPOSAL** is our group's position, already
implemented in our server and client, but **not yet agreed** with the other
groups — it is open for discussion and will be revised after the interop tests.

Conformance scenarios: [`tests/interop/`](../tests/interop/) — plain text, run
against any server with `python3 scripts/interop.py <host> <port>`.

## 1. Transport

- TCP. Default port: **PROPOSAL** `5000`.
- **PROPOSAL:** IPv4. Our server listens on `0.0.0.0` (IPv4 only); clients
  should connect over IPv4 (a client that resolves a hostname to an IPv6 address
  first must fall back to IPv4).

## 2. Framing

- One JSON object per line, UTF-8 encoded, terminated by `\n` (0x0A).
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

Implemented: `NICK`, `MSG`, `OK`, `CHAT`, `ERROR` (since M1) and `EVENT` (since M2).
`PRIV`, `LIST`, `QUIT` and `USERS` arrive in M3; until then our server answers
any of them with `ERROR 400 "unknown type"`.

General rules:
- Receivers MUST ignore unknown fields (allows optional extensions).
- Key order is not significant.
- Keys and `type` values are case-sensitive (`"Type"` ≠ `"type"`, `"nick"` ≠ `"NICK"`).
- Our encoder writes `"type"` first and produces exactly the bytes of the
  examples above, but receivers must not rely on that.

### 3.1 Field rules — PROPOSAL

| Field | Rule |
|---|---|
| `nick`, `from` | string, 1–32 **bytes**, no spaces, no control characters |
| `text` | string, 1–1024 bytes, no control characters (so no multi-line messages) |
| `reason` | string, 0–128 bytes, no control characters; optional (absent = `""`) |
| `code` | integer 100–999 |
| `private` | boolean; optional (absent = `false`) |

- "Control character" = byte < 0x20 or 0x7F. This also bounds the encoded size:
  JSON escapes `"` and `\` with 2 bytes, so no field grows more than 2×, and the
  largest possible message (a `CHAT` with both fields at their limits) is 2164
  bytes — always within one line.
- Lengths are in bytes, not characters (`"olá"` is 4 bytes). UTF-8 is not
  validated.
- No spaces in nicks so that `/msg <nick> text` (M3) can split on the first space.
- Open: are nicks case-sensitive (`Ana` vs `ana`)? Matters for unique nicks (M3).

## 4. Semantics

| Question | Our answer (**PROPOSAL**) |
|---|---|
| Which requests get an `OK`? | `NICK` gets `OK`. (`PRIV`, `QUIT`: to decide in M3.) |
| Does the sender of `MSG` receive its own `CHAT`? | **Yes.** It confirms the message reached the server, and clients must therefore **not** echo locally (it would appear twice). |
| A client request (`MSG`; later `PRIV`, `LIST`, `QUIT`) before a successful `NICK` | `ERROR 401 "nick required"`. The connection stays open. |
| `NICK` after a successful `NICK` | `ERROR 400 "nick already set"` (no nick change in M1). |
| S→C types (`OK`, `CHAT`, `ERROR`, `EVENT`) sent by a client | `ERROR 400 "unexpected type"`, in any state. |
| Unknown `type` | `ERROR 400 "unknown type"`. |
| Invalid JSON, missing field, wrong type, out of limits | `ERROR 400` with reason `invalid JSON` / `invalid field`. The connection stays open: the next line is handled normally. |
| Who is `from` in a `CHAT`? | Always the nick of the sender's session. A `"from"` field sent by a client is ignored. |
| Missing optional fields | `private` absent = `false`; `reason` absent = `""`. |
| What does the sender of `PRIV` receive? | Open (M3). |
| Exact `EVENT` text for join/leave | `"<nick> joined"` and `"<nick> left"` (the slide 124 example is `"rui joined"`). Clients should display `EVENT` as a notice, not as a chat line. |
| Who receives an `EVENT`? | Everyone **except** the client it is about: whoever joins does not need to be told they joined. |
| Does a client receive traffic before its `NICK` is accepted? | **No.** Between `accept()` and a successful `NICK` the connection exists but is not in the chat, so it gets no `CHAT` and no `EVENT`. The first line a client sees is always the answer to its own `NICK` (`OK` or `ERROR`): the client only joins the chat *after* its `OK` has been sent. Clients should still not rely on this when talking to other servers — wait for `OK`/`ERROR` and display anything that arrives before it. |
| Does leaving before a successful `NICK` produce an `EVENT`? | **No.** Nobody knew that client was there. |
| Does `QUIT` get an `OK` before the server closes? | Open (M3). |
| What if a client stops reading (suspended laptop, `Ctrl+Z`)? | Each message the server sends must be fully accepted within **5 s**. If not, that client is disconnected (the others get `"<nick> left"`). One stuck client must never freeze the chat for everyone (slide 125). A client that is only *slow* is not affected: the kernel buffers hold hundreds of KB. |
| Server full | `ERROR 503 "server full"`, then the server closes the connection. Our server accepts up to 64 clients. |

Rule of thumb: **strict when sending, tolerant when receiving.** A receiver never
crashes because of what a peer sent; a server answers with `ERROR` and carries on.

## 5. Error codes

| Code | Reason(s) | When | Status |
|------|-----------|------|--------|
| 400 | `invalid JSON` | not JSON, not an object, trailing data, embedded `\0` | PROPOSAL |
| 400 | `invalid field` | missing field, wrong JSON type, outside the limits of §3.1 | PROPOSAL |
| 400 | `unknown type` | `type` is not a known message type | PROPOSAL |
| 400 | `unexpected type` | a client sent a S→C type | PROPOSAL |
| 400 | `nick already set` | second `NICK` on the same connection | PROPOSAL |
| 400 | `line too long` | line over the maximum length; server then closes | PROPOSAL |
| 401 | `nick required` | request before a successful `NICK` | PROPOSAL |
| 404 | — | `PRIV` to an unknown nick | PROPOSAL (M3) |
| 409 | `nick taken` | `NICK` already in use | **from the slides** (M3) |
| 503 | `server full` | no room for another client; the server closes after sending it | PROPOSAL (M2) |

Clients should rely on the **code**; the reason is human-readable text and may
differ between implementations (our conformance scenarios compare codes only).

## 6. Example sessions

`>` client to server, `<` server to client. Mirrored by `tests/interop/`.

Normal session (`01_nick_ok`, `03_msg_chat`):

```
> {"type":"NICK","nick":"ana"}
< {"type":"OK"}
> {"type":"MSG","text":"hello"}
< {"type":"CHAT","from":"ana","text":"hello","private":false}
```

Request before `NICK` (`02_msg_antes_de_nick`) — the connection stays open:

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

Line too long — the server closes after the error:

```
> AAAA…(more than 4096 bytes before the \n)
< {"type":"ERROR","code":400,"reason":"line too long"}
  [connection closed]
```

## Changelog

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
