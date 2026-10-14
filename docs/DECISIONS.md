# Decisions to agree with the other groups

What still **has to be agreed** with the other groups so that our client works
with their servers and our server works with their clients — and, just as
important, what does **not** need agreeing because the slides already decide
it.

- The specification is in [`PROTOCOL.md`](PROTOCOL.md) (v0.5). This file says
  **what to ask**, and what happens if two groups decide differently.
- Every row has **our proposal**, already implemented and tested. A proposal is
  not an imposition: if most groups decide otherwise, we change. Each decision
  lives in a single place in our code (see the "In our code" column), so
  changing it is cheap.
- When a point is agreed, mark it **AGREED** here and in `PROTOCOL.md`. Once
  all **A** points are agreed, `PROTOCOL.md` becomes v1.0.

| Section | What it is | Needs agreement? |
|---|---|---|
| **S** | Fixed by the slides | **No** — only confirm everyone reads the slide the same way |
| **A** | Open, and breaks interoperability if groups differ | **Yes** — the only real negotiation |
| **B** | Open, but only changes what the user sees (or what our tests compare) | Nice to have |
| **C** | Internal to our implementation | No (worth mentioning) |
| **D** | Later milestones (M4–M6): what the slides fix and what is still open | Agree early |

### Why the list is short: strict when sending, tolerant when receiving

A point only needs agreement if it can **break** another group. Anything that
we never send, and that we accept gracefully when someone else sends it, needs
no agreement: whatever the other group decides, nothing breaks. For example,
our client never sends an empty line or two objects on one line, and our
server copes when someone else does — so how each server reacts to them is
its own business (section C), not something to negotiate.

---

## Contact log

| Group | Language | Who | Date | S confirmed | A points agreed | `interop.py` against their server | Their client against our server |
|---|---|---|---|---|---|---|---|
| | | | | | | | |
| | | | | | | | |
| | | | | | | | |

---

## S — Fixed by the slides (no agreement needed, just confirm)

| # | What is fixed | Slide | What we do |
|---|---|---|---|
| S1 | The 10 message types, their fields and directions: `NICK`, `MSG`, `PRIV`, `LIST`, `QUIT` (C→S); `OK`, `CHAT`, `USERS`, `EVENT`, `ERROR` (S→C) | 124 | exactly the slide's bytes (`test_protocol` #31, #58, #59) |
| S2 | **Newline-delimited JSON**: one JSON object per line, ending in `\n` | 123 (M1) | we send `\n`; we also accept `\r\n` and ignore empty lines (tolerant receiving — no agreement needed) |
| S3 | Keys and `type` values are exactly as written (`"type"`, `"NICK"`) — JSON compares names exactly, so they are case-sensitive | 124 + JSON | `"Type"` or `"nick"` as a type → `ERROR 400` |
| S4 | `code` in `ERROR` is a JSON number (`"code":409`) | 124 | integer 100–999 |
| S5 | `NICK` **must be the first message** | 124 | our client always sends `NICK` first; our server answers anything else before it with an `ERROR` (code: B5) |
| S6 | Nicknames are **unique** | 123 (M3) | `ERROR 409` |
| S7 | Nick in use → **`ERROR 409 "nick taken"`** | 124 | exactly that code and reason |
| S8 | `OK` means **"last request accepted"**, so an accepted `NICK` gets `OK` | 124 | `NICK` → `OK` (which *other* requests get `OK` is open: A1) |
| S9 | `USERS` is **the reply to `LIST`** | 124 | `LIST` → `USERS` (no `OK`) |
| S10 | `ERROR` means **"request refused"** | 124 | every refused request gets exactly one `ERROR` |
| S11 | A private message arrives as a **`CHAT` with `"private":true`**; `PRIV` is what `/msg` sends | 123, 124 | `/msg rui hi` → `PRIV` → `CHAT … "private":true` to rui |
| S12 | `EVENT` = **join and leave notices**; example text `"rui joined"` | 123 (M2), 124 | `"<nick> joined"` / `"<nick> left"` |
| S13 | `QUIT` = **"leave cleanly"** | 124 | the client sends `QUIT` on `/quit` and on Ctrl+D (what the server answers: B2) |
| S14 | **IPv4 and IPv6**: *"Use getaddrinfo() and sockaddr_storage for IPv4 / IPv6 portability"*; the client tries every candidate address (*"Retry another candidate"*) | 85 (sockets) | the server listens on both in a single dual-stack socket; the client tries each address `getaddrinfo()` returns |
| S15 | A slow or stuck client must not block everyone: *"Use timeouts, or a send queue per client"* | 125 | 5 s send deadline (the value is each group's choice: C3) |

**How to use this table with another group:** "We read slides 123–125 like
this — do you too?" If someone reads one differently, it moves to section A.
The most likely to be read differently is **S8**: *"Last request accepted"*,
taken literally, could mean an `OK` after **every** accepted request,
including each `MSG`. That is question A1.

---

## A — Open, and breaks interoperability if groups differ

These are the only points that really need negotiating.

| # | Question | Our proposal | If groups differ | In our code | Test |
|---|---|---|---|---|---|
| A1 | **Which requests get an `OK`?** (the slide only says `OK` = "last request accepted") | **every request gets ONE direct answer**: `NICK` → `OK`; `MSG` → its own `CHAT`; `PRIV` → the copy of the `CHAT`; `LIST` → `USERS`; `QUIT` → `OK`. Or an `ERROR` | a client that waits for an `OK` after each `MSG` **hangs** on a server that does not send it. (Against a server that does send it, our client just prints `[ok]`.) | `session.c` (D6, D13) | `interop` 01, 03 |
| A2 | Does the sender of `MSG` receive its **own** `CHAT`? | **yes** — it confirms the message arrived; so clients do **not** echo locally | **duplicated** messages (local echo + server copy) or **missing** ones (no echo at all) | `SESS_ALL` (D5) | `interop` 03 |
| A3 | Does the sender of `PRIV` receive a **copy**? | **yes**, same rule as A2 | same as A2, for private messages | `handle_priv()` (D14) | `interop` 08 |
| A4 | OK to add a `"to"` field to private `CHAT`s (our extension)? `{"type":"CHAT","from":"ana","to":"rui","text":"hi","private":true}` | **yes**, and in general: **receivers ignore unknown fields**. Without `"to"` the sender's copy cannot say who it was for. A public `CHAT` never has it (exactly the slide's bytes) | a client that **rejects** unknown fields rejects our private messages. If a group cannot accept it, we drop `"to"` (one line in `handle_priv()`) | `protocol.c`, `ui.c` | `test_protocol` #60; `interop` 05 |
| A5 | **Maximum line length** | **4096 bytes, including the `\n`** | a 3000-byte line is accepted by one side and refused by the other | `RX_MAX_LINE` (`framing.h`) | `test_framing` #7, #21 |
| A6 | **Field limits**, counted in **bytes** (not characters; UTF-8 is not validated) | `nick` 1–32, `text` 1–1024 (no control characters, so no multi-line messages), `reason` 0–128 | a 40-byte nick or a 2000-byte message is accepted by one server and rejected by another group's client; a group counting characters accepts 32 accented letters (64 bytes) that we reject | `protocol.h` (D3, D4) | `test_protocol` #26 |
| A7 | **Characters forbidden in a nick** | **space, `"`, `\` and control characters** | spaces break `/msg nick text`; with `"`/`\` the worst `USERS` (64 × 32 bytes) is 4315 bytes and does not fit in a line (A5) — calculation in `PROTOCOL.md` §3.1 | `proto_valid_nick()` (D20) | `test_protocol` #61, #59 |

---

## B — Open, but only changes what the user sees

None of these break anything; agreeing them makes the chat look the same
everywhere, and makes our conformance scenarios pass against other servers
(they compare codes, the `USERS` order, etc.).

| # | Question | Our proposal | If groups differ | In our code |
|---|---|---|---|---|
| B1 | Default port | `5000` | everyone types the port by hand | `DEFAULT_PORT` |
| B2 | `QUIT`: does the server answer before closing? | **`OK`, then close**; also before `NICK` | a client expecting `OK` shows nothing or `[ok]`; ours works either way (it sends `QUIT`, then FIN, and waits for the close) | `case MT_QUIT` (D17) |
| B3 | Are nicks case-sensitive for uniqueness (and for finding the recipient of a `PRIV`)? | **no**: `Ana`, `ana` and `ANA` are the same nick (ASCII letters only; `Zé` ≠ `ZÉ`); shown as typed | one server allows `Ana` and `ana` together (easy impersonation); `/msg Rui` gives 404 on one server and works on another | `proto_nick_eq()` (D18) |
| B4 | After a `409`, does the connection stay open? | **yes**, the client can try another nick | our client asks for another nick and retries; against a server that closes, it reports the close and exits | `session.c` (D19) |
| B5 | Error codes besides 409 | **400** malformed message, **401** request before `NICK`, **404** `PRIV` to someone not in the chat, **503** server full; no other codes. Clients decide on the code, the `reason` is free text | users see different codes for the same problem; our scenarios compare codes, so they fail against other servers | `session.c`, `protocol.h` |
| B6 | `PRIV` to a nick that is not in the chat | `ERROR 404 "no such nick"` | see B5 | `handle_priv()` (D15) |
| B7 | `PRIV` to oneself | delivered **once** | may appear twice | `handle_priv()` |
| B8 | Who receives an `EVENT` | everyone **except** the client it is about | the joiner sees a notice about themselves | `SESS_OTHERS` (D10) |
| B9 | `EVENT` text on leaving | `QUIT` and a dropped connection both give `"<nick> left"` | different text; a client that **parses** `EVENT` texts to keep a user list breaks — the list must come from `LIST` | `fill_event()` (D10) |
| B10 | Does `USERS` include the requester? In which order? | **yes**, **alphabetical ignoring case**; clients without a nick are not listed | a "N people online" counter shows N±1; our `10_list` scenario compares the exact list | `handle_list()` (D16) |
| B11 | Server full | `ERROR 503 "server full"`, then close; the limit is each group's choice (ours: 64) | a close with no explanation | `server/main.c` (D12) |

---

## C — Internal to our implementation (no agreement needed)

Either they are our own choices, or they only concern how we react to things
that **our** programs never send (strict when sending, tolerant when receiving).

| # | What | Our choice |
|---|---|---|
| C1 | Server concurrency | one thread per client (not `select`/`poll`) |
| C2 | Maximum clients | 64 (`ROSTER_MAX`) |
| C3 | Send deadline (S15) | 5 s per message, total deadline (not `SO_SNDTIMEO`) |
| C4 | Line too long | `ERROR 400 "line too long"`, then close (no resynchronisation) |
| C5 | Invalid line (not JSON, missing/wrong field, two objects on a line) | `ERROR 400`; the connection **stays open** |
| C6 | Request before `NICK` (breaks S5) | `ERROR 401 "nick required"`; the connection stays open |
| C7 | Second `NICK` on the same connection | `ERROR 400 "nick already set"` (no nick change) |
| C8 | A client sends a S→C type (`OK`, `CHAT`, `USERS`, …) | `ERROR 400 "unexpected type"` |
| C9 | `"from"` sent by a client | ignored: `from` is always the session's nick (no impersonation) |
| C10 | Traffic before `NICK` is accepted | none: the first line a client receives is always the answer to its `NICK`. Our client does not rely on this with other servers |
| C11 | Optional fields we receive | `private` absent = `false`; `reason` absent = `""`; `to` absent = `""` |
| C12 | Nicks we **receive** from servers with looser rules | accepted with `"` and `\` (only spaces and control characters are refused) |
| C13 | When a nick is freed | as soon as its connection leaves the list (`QUIT` or drop) |
| C14 | Client commands | `/list`, `/msg <nick> <text>`, `/quit`, `/help`, `//text` for a line starting with `/`; unknown commands are refused locally (never sent to the chat) |
| C15 | Client-side validation | same rules as the server, before sending |
| C16 | Client waiting for the answer to `NICK` | 5 s; shows anything that arrives before the `OK` instead of taking it as the answer |
| C17 | Language and libraries | C11 + POSIX + cJSON |

---

## D — Later milestones: agree early

The slides already fix part of each milestone (slide 123). The rest is the
same kind of problem as above: if each group decides alone, it is only
discovered on interop day.

| # | Milestone | Fixed by the slides | Still open | Initial idea (not implemented yet) |
|---|---|---|---|---|
| D1 | M4 | an **idle timeout** must exist | how long; is the client told before? | ≥ 5 min; `ERROR 408 "idle timeout"` then close (408 = HTTP *Request Timeout*) |
| D2 | M4 | — | keepalive? (slide 124 has no `PING`; a new type would be `unknown type` elsewhere) | **no** new type: a client that wants to stay connected sends a `LIST` |
| D3 | M4 | **Ctrl+C shutdown that closes sockets** | are clients told? | `EVENT "server shutting down"` to everyone, then close |
| D4 | M4 | **automated tests with pytest** | — (internal) | our network tests are already Python scripts; turn them into pytest |
| D5 | M5 | the server answers a **UDP broadcast** called **`DISCOVER`** with **its TCP port** | exact JSON of the request and the reply; UDP port; client wait time | request `{"type":"DISCOVER"}`; reply `{"type":"SERVER","port":5000}` (+ optional `"name"`); the IP is the packet's **source**, never a field (`typical-problems.md` §4.4); UDP port `5001`; wait 1 s and list every reply |
| D6 | M6-A | file transfer with **length-prefixed binary frames** | the header and how it coexists with the JSON lines | to agree between the groups that pick it |
| D7 | M6-B | **TLS** with `ssl` and a **self-signed certificate** | same port or another? | another port (e.g. `5443`), so clients without TLS do not break |
| D8 | M6-C | chat rooms with **`/join` and `/leave`** | the message types (not in slide 124) | to agree between the groups that pick it |
| D9 | M6 | one extension per group | which one each group picks | tell each other early |

---

## The questions to ask (in order)

1. **A1** — Which requests get an `OK`? (and do you read *"Last request accepted"* the same way? — S8)
2. **A2/A3** — Does the sender receive its own `CHAT`, and a copy of its `PRIV`?
3. **A5/A6/A7** — Lines up to 4096 bytes; nick 1–32 bytes without spaces, `"` or `\`; text 1–1024 bytes; all in bytes?
4. **A4** — Do your receivers ignore unknown fields (our `"to"`)?
5. Section **S** — do you read the slides the same way?

---

## Message to send to the groups

> Hi! We are the group doing the chat in C (repository Future_Networks). For
> M3 we have the full protocol implemented and tested (`docs/PROTOCOL.md`,
> v0.5). Most of it is fixed by the slides; only a few points really need
> agreeing (`docs/DECISIONS.md`, section A):
>
> 1. Which requests get an `OK`? Our reading of *"Last request accepted"*: `NICK` and `QUIT` get `OK`; `MSG`/`PRIV` are confirmed by the `CHAT` itself and `LIST` by the `USERS`.
> 2. Does the sender of a `MSG` receive its own `CHAT`, and the sender of a `PRIV` a copy? (us: yes and yes, so clients do not echo locally)
> 3. Limits, in bytes: lines up to 4096 (including `\n`), nick 1–32 with no spaces, `"` or `\`, text 1–1024?
> 4. Do your receivers ignore unknown fields? (we add `"to"` to private `CHAT`s)
>
> Our server accepts IPv4 and IPv6 on port 5000. We also have plain-text test
> scenarios that run against any server (`python3 scripts/interop.py <ip>
> <port>`). Shall we set up a cross-test session: our client against your
> server and your client against ours?
