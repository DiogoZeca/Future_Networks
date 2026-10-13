# Chat Protocol — v0.2 (draft)

Shared specification for interoperability between groups (IF 2026/27).
Message names and fields come from the course slides (slide 124) and are
**fixed**. Everything marked **PROPOSAL** is our group's position, already
implemented in our M1 server and client, but **not yet agreed** with the other
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

Our M1 implements `NICK`, `MSG`, `OK`, `CHAT`, `ERROR`. `PRIV`, `LIST`, `QUIT`,
`USERS` and `EVENT` arrive in M2/M3; until then our server answers them with
`ERROR 400 "unknown type"`.

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
| S→C types (`OK`, `CHAT`, `ERROR`) sent by a client | `ERROR 400 "unexpected type"`, in any state. |
| Unknown `type` | `ERROR 400 "unknown type"`. |
| Invalid JSON, missing field, wrong type, out of limits | `ERROR 400` with reason `invalid JSON` / `invalid field`. The connection stays open: the next line is handled normally. |
| Who is `from` in a `CHAT`? | Always the nick of the sender's session. A `"from"` field sent by a client is ignored. |
| Missing optional fields | `private` absent = `false`; `reason` absent = `""`. |
| What does the sender of `PRIV` receive? | Open (M3). |
| Exact `EVENT` text for join/leave | Open (M2). |
| Does `QUIT` get an `OK` before the server closes? | Open (M3). |

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

- v0.2 — framing details (empty lines, max length, one object per line); field
  rules; semantics and error codes as proposals from our M1 implementation;
  example sessions; conformance scenarios in `tests/interop/`.
- v0.1 — initial draft.
