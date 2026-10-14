# Decisions to agree with the other groups

Everything that **has to be discussed** with the other groups so that our
client works with their servers and our server works with their clients. The
message names and fields come from slide 124 and are **not up for
discussion**. What is discussed is the **semantics** the slide leaves open
(*"you may need to define semantics"*).

- The specification is in [`PROTOCOL.md`](PROTOCOL.md) (v0.5). This file says
  **what to ask**, and what happens if two groups decide differently.
- Every row has **our proposal**, already implemented and tested. A proposal is
  not an imposition: if most groups decide otherwise, we change. Each decision
  lives in a single place in our code (see the "In our code" column), so
  changing it is cheap.
- Priorities: **A** breaks interoperability if groups differ; **B** only
  changes what the user sees; **C** is internal and needs no agreement (but is
  worth mentioning); **D** belongs to later milestones and is worth agreeing
  early.
- When a point is agreed, mark it **AGREED** here and in `PROTOCOL.md`. Once
  all A points are agreed, `PROTOCOL.md` becomes v1.0.

---

## Contact log

| Group | Language | Who | Date | A points agreed | `interop.py` against their server | Their client against our server |
|---|---|---|---|---|---|---|
| | | | | | | |
| | | | | | | |
| | | | | | | |

---

## A — Break interoperability if groups differ

### A.1 Transport and framing

| # | Question | Our proposal | If groups differ | In our code | Test |
|---|---|---|---|---|---|
| A1 | Default port | `5000` | everyone has to type the port by hand; harmless but annoying on the day | `DEFAULT_PORT` (server/client `main.c`) | — |
| A2 | IPv4, IPv6 or both? | **IPv4**: the server listens on `0.0.0.0`; the client falls back to IPv4 if the name resolves to IPv6 first | a client that only tries the AAAA record cannot reach our server | `net.c` | `typical-problems.md` §4.3 |
| A3 | Delimiter | `\n`; also accept `\r\n` (strip one `\r`) | a Windows group sending `\r\n` gets `"invalid JSON"` from a server that does not strip the `\r` | `framing.c` | `test_framing` |
| A4 | Empty lines | ignored, no error | an extra `Enter` in `nc` gives an `ERROR` on one side and nothing on the other | `framing.c` | `test_framing` |
| A5 | Maximum line length | **4096 bytes, including the `\n`** | a 3000-byte message is accepted on one side and refused on the other | `RX_MAX_LINE` (`framing.h`) | `test_framing` #7, #21 |
| A6 | Line longer than the maximum | `ERROR 400 "line too long"` and **close** (no resynchronisation) | whoever tries to resynchronise parses the rest of the line as messages | `server/main.c` | M1.md hostile case B |
| A7 | Two objects on one line (`{..}{..}`), or trailing garbage | `ERROR 400 "invalid JSON"` | a server that accepts the first one silently loses the second | `protocol.c` | `test_protocol` #25 |

### A.2 Fields

| # | Question | Our proposal | If groups differ | In our code | Test |
|---|---|---|---|---|---|
| A8 | Nick length | **1–32 bytes** (bytes, not characters) | a 40-byte nick is accepted by one server and another group's client rejects its `CHAT` | `PROTO_NICK_MAX` | `test_protocol` #26 |
| A9 | Characters forbidden in a nick | **space, `"`, `\` and control characters** (< 0x20, 0x7F) | spaces break `/msg nick text`; with `"`/`\` the worst `USERS` (64 × 32 bytes) exceeds 4096 bytes and does not fit in one line (calculation in `PROTOCOL.md` §3.1) | `proto_valid_nick()` | `test_protocol` #61, #59 |
| A10 | And nicks we **receive** from a server with looser rules? | the client accepts `"` and `\` in `from`/`USERS` (strict when sending, tolerant when receiving) | a strict client rejects whole messages from a more permissive server | `valid_peer_nick()` | `test_protocol` #61 |
| A11 | Text length | **1–1024 bytes**, no control characters (so no multi-line messages) | a 2000-byte message is accepted by one server and rejected by another group's client | `PROTO_TEXT_MAX` | `test_protocol` #26 |
| A12 | UTF-8 | not validated; lengths count bytes | a group counting characters accepts a 32-letter accented nick (64 bytes) that we reject | `protocol.c` (D3) | `interop` 06 |
| A13 | Unknown fields | **ignored** | a server that rejects extra fields breaks with any extension (including our `"to"`, A20) | `protocol.c` | `interop` 05 |
| A14 | Case of keys and of `type` | **case-sensitive** (`"Type"` ≠ `"type"`, `"list"` ≠ `"LIST"`) | one side accepts, the other rejects | `protocol.c` | `test_protocol` #15, #32 |
| A15 | Optional fields | `private` absent = `false`; `reason` absent = `""` | a server that omits `private` breaks a client that requires it | `protocol.c` | `test_protocol` #30 |
| A16 | `ERROR` `code` | integer 100–999 | `"409"` (a string) is rejected by us | `protocol.c` | `test_protocol` #30 |

### A.3 Replies: who receives what

| # | Question | Our proposal | If groups differ | In our code | Test |
|---|---|---|---|---|---|
| A17 | **Which requests get an `OK`?** | general rule: **every request gets ONE direct answer**. `NICK` → `OK`; `MSG` → its own `CHAT`; `PRIV` → the copy of the `CHAT`; `LIST` → `USERS`; `QUIT` → `OK`. Or an `ERROR` | a client expecting `OK` after each `MSG` waits forever; one that does not expect it prints `[ok]` after every line | `session.c` (D6, D13) | `interop` 01, 03 |
| A18 | Does the sender of `MSG` receive its **own** `CHAT`? | **yes** — it confirms the message arrived; so the client does **not** echo locally | **duplicated** messages (local echo + server) or **missing** ones (no echo at all) | `SESS_ALL` (D5) | `interop` 03 |
| A19 | What does the sender of `PRIV` receive? | **a copy** of the private `CHAT` (same rule as A18) | same as A18, for private messages | `handle_priv()` (D14) | `interop` 08 |
| A20 | `"to"` field in a private `CHAT` (our extension) | `{"type":"CHAT","from":"ana","to":"rui","text":"hi","private":true}` — without `"to"` the sender's copy does not say who it was for. Never present in a public `CHAT` | harmless for anyone who ignores unknown fields (A13); only the "who it was for" is lost | `protocol.c`, `ui.c` | `test_protocol` #60 |
| A21 | `PRIV` to oneself | delivered **once** | harmless (may appear twice) | `handle_priv()` | `test_session` #66 |
| A22 | `QUIT`: does the server answer and close? | **`OK`, then close** (FIN right after the `OK`); anything sent after the `QUIT` is ignored | a client waiting for `OK` before leaving hangs on a server that only closes; one that does not expect it prints `[ok]` | `case MT_QUIT` (D17) | `interop` 11 |
| A23 | `QUIT` before `NICK` | **allowed**: `OK`, then close | `401` on one side, `OK` on the other | `case MT_QUIT` | `interop` 12 |
| A24 | The first request must be `NICK`. What if it is not? | `MSG`, `PRIV`, `LIST` → **`ERROR 401 "nick required"`**, connection stays open | a server that closes the connection breaks a client that sends `LIST` too early | `session.c` | `interop` 02, 12 |
| A25 | Is there traffic before `NICK` is accepted? | **no**: the connection is not in the chat; the first line the client receives is **always** the answer to its `NICK` | a client that takes the first line as the answer reads a `CHAT` as if it were `OK` (our client no longer assumes this) | `roster_join()` after the `OK` (D9) | `probe_m2.py` §6 |
| A26 | `NICK` repeated on the same connection | `ERROR 400 "nick already set"` (no nick change) | a server that changes the nick leaves everyone else unsure who is who | `session.c` | `test_session` #36 |
| A27 | A client sends a S→C type (`OK`, `CHAT`, `USERS`, ...) | `ERROR 400 "unexpected type"` | harmless as long as it is an `ERROR` (only the reason differs) | `session.c` | `test_session` #37, #70 |
| A28 | Does an invalid line close the connection? | **no**: `ERROR 400` and the next line is handled normally (except "line too long") | a client with a bug in one message loses the connection on one side but not on the other | `session.c` | `interop` 04 |
| A29 | The `from` of a `CHAT` | always the session's nick; a `"from"` sent by the client is ignored | a server that accepts it lets anyone impersonate anyone | `session.c` | `interop` 05 |

### A.4 Unique nicks

| # | Question | Our proposal | If groups differ | In our code | Test |
|---|---|---|---|---|---|
| A30 | **Are nicks case-sensitive?** | **No, for uniqueness**: `Ana`, `ana` and `ANA` are the same nick. ASCII letters only (`Zé` ≠ `ZÉ`). The nick is stored and shown as typed | one server accepts `Ana` and `ana` at the same time (easy impersonation), another gives 409 | `proto_nick_eq()` (D18) | `interop` 07; `test_protocol` #62 |
| A31 | Does `PRIV` find the recipient the same way? | **yes**, same comparison; the `"to"` in the `CHAT` carries the nick as registered (`/msg ANA` → `"to":"ana"`) | `/msg Rui` works on one server and gives 404 on another | `proto_nick_eq()` | `test_session` #64 |
| A32 | After a `409`, does the connection stay open? | **yes**: the state does not change and the client may try another nick | a client retrying on a connection the server already closed fails; one that gives up at the first 409 loses the chance to try another nick | `session.c` (D19) | `interop` 07 |
| A33 | When is the nick of someone who leaves freed? | when the connection leaves the list (`QUIT` or drop); it can be reused right after | harmless | `roster_remove()` | `probe_m3.py` §6 |

### A.5 Error codes

Clients should decide on the **code**; the `reason` is free text to display.

| # | Code | When (our proposal) | Reason we send | Status |
|---|---|---|---|---|
| A34 | 400 | invalid JSON; missing/wrong/out-of-limits field; unknown type; S→C type from a client; second `NICK`; line too long | `invalid JSON`, `invalid field`, `unknown type`, `unexpected type`, `nick already set`, `line too long` | PROPOSAL |
| A35 | 401 | `MSG`/`PRIV`/`LIST` before an accepted `NICK` | `nick required` | PROPOSAL |
| A36 | 404 | `PRIV` to a nick that is not in the chat | `no such nick` | PROPOSAL |
| A37 | 409 | `NICK` already in use | `nick taken` | **from slide 124** |
| A38 | 503 | server full; the server closes right after | `server full` | PROPOSAL |
| A39 | — | Use other codes? (e.g. 413 for a long line, 422 for an invalid field) | — | proposal: **no**, only these five; the fewer codes, the less to disagree on |

---

## B — Only change what the user sees

| # | Question | Our proposal | If groups differ | In our code |
|---|---|---|---|---|
| B1 | `EVENT` text | `"<nick> joined"` and `"<nick> left"` (the slide example is `"rui joined"`); `QUIT` and a dropped connection give the same `"left"` | the text looks different; nothing breaks. A client that **parses** the text to keep a local user list does break — which is why the list comes from `LIST` | `fill_event()` (D10) |
| B2 | Who receives the `EVENT` | everyone **except** the client it is about | whoever joins sees a notice about themselves; harmless | `SESS_OTHERS` (D10) |
| B3 | Does `USERS` include the requester? | **yes** | a client counting "N people online" shows N±1 | `handle_list()` (D16) |
| B4 | `USERS` order | **alphabetical, ignoring case** | tests comparing the exact list fail; users do not notice | `handle_list()` (D16) |
| B5 | Does `USERS` list clients without a `NICK` yet? | **no** (they have no nick) | — | `roster_nicks()` |
| B6 | Server full | `ERROR 503`, then close; the client limit is each group's choice (ours: 64) | a close with no explanation | `server/main.c` (D12) |
| B7 | Client that stops reading | disconnected if a send does not complete within 5 s; the value is each group's choice — what matters is that **no one can freeze the chat** | a server without a deadline freezes when one client is suspended | `SEND_TIMEOUT_MS` (D11) |
| B8 | `reason` texts | those in table A.5 | nothing breaks: clients use the code | `protocol.h`, `session.h` |

---

## C — Internal (no agreement needed, but worth mentioning)

| # | What | Our choice |
|---|---|---|
| C1 | Server concurrency | one thread per client (not `select`/`poll`) |
| C2 | Maximum clients | 64 (`ROSTER_MAX`) |
| C3 | Send deadline | 5 s per message, total deadline (not `SO_SNDTIMEO`) |
| C4 | Client commands | `/list`, `/msg <nick> <text>`, `/quit`, `/help`, `//text` for a line starting with `/`; unknown commands are rejected locally (never sent to the chat) |
| C5 | Client-side validation | the client validates with the same rules as the server before sending |
| C6 | Client waiting for the `OK` to its `NICK` | 5 s; shows whatever arrives before the `OK` instead of taking it as the answer |
| C7 | Ctrl+D in the client | sends `QUIT`, then closes the sending side (FIN) |
| C8 | Language | C11 + POSIX + cJSON |

---

## D — Later milestones: agree early

Not part of M3, but the same kind of problem: if each group decides alone, it
is only discovered on interop day.

| # | Milestone | Question | Initial idea (to discuss, not implemented yet) |
|---|---|---|---|
| D1 | M4 | Idle timeout: how long without traffic before the server disconnects a client? | ≥ 5 min; a quiet chat is not a dead client |
| D2 | M4 | Does the server warn before disconnecting an idle client? With which code? | `ERROR 408 "idle timeout"`, then close (408 = HTTP *Request Timeout*) |
| D3 | M4 | Is there a *keepalive*? | slide 124 has no `PING`; a new type would be `unknown type` on other servers. Proposal: **no** new type; a client that wants to keep the connection alive sends a `LIST` |
| D4 | M4 | Ctrl+C on the server: are clients told? | `EVENT "server shutting down"` to everyone, then close |
| D5 | M5 | UDP request format | `{"type":"DISCOVER"}` on one line, broadcast |
| D6 | M5 | Reply format | `{"type":"SERVER","port":5000,"name":"<group>"}`; the IP is the packet's **source** address, never a JSON field (`typical-problems.md` §4.4) |
| D7 | M5 | Discovery UDP port | one for the whole class (e.g. `5001`) |
| D8 | M5 | How long does the client wait for replies? Several servers? | 1 s; list every server that replies |
| D9 | M6 | Which extension does each group pick? | if two groups pick the same one (e.g. `/join` rooms), they agree the formats between them |
| D10 | M6-A | File transfer: header format and length prefix | to be agreed between the groups that pick it |
| D11 | M6-B | TLS: same port or a different one? | a different port (e.g. `5443`), so clients without TLS do not break |

---

## The open questions that matter most

If there is only time for five questions, these break the most (in order):

1. **A18/A19** — does the sender receive its own `CHAT` (and a copy of its `PRIV`)?
2. **A17/A22** — which requests get an `OK`? (especially `QUIT`)
3. **A34–A39** — the error codes (400/401/404/409/503), and only these?
4. **A5/A8/A9/A11** — maximum line length and field limits (nicks without `"` and `\`)
5. **A30/A32** — are nicks case-insensitive? Does a `409` keep the connection open?

---

## Message to send to the groups

> Hi! We are the group doing the chat in C (repository Future_Networks). For
> M3 we have a complete semantics proposal, already implemented and tested, in
> our `docs/PROTOCOL.md` (v0.5), and the list of what still needs agreeing in
> `docs/DECISIONS.md`. The five questions that break interoperability the most
> are:
>
> 1. Does the sender of a `MSG` receive its own `CHAT`? And does the sender of a `PRIV` receive a copy? (us: yes and yes, no local echo)
> 2. Which requests get an `OK`? (us: `NICK` and `QUIT`; `MSG`/`PRIV` are confirmed by the `CHAT`, `LIST` by the `USERS`)
> 3. Error codes: 400 / 401 (before `NICK`) / 404 (`PRIV` to someone not connected) / 409 (nick taken) / 503 (full)?
> 4. Lines up to 4096 bytes; nicks 1–32 bytes with no spaces, `"` or `\`; text 1–1024 bytes?
> 5. Are `Ana` and `ana` the same nick? And after a `409`, does the connection stay open to try another one?
>
> We also have plain-text test scenarios that run against any server
> (`python3 scripts/interop.py <ip> <port>`). Shall we set up a cross-test
> session: our client against your server and your client against ours?
