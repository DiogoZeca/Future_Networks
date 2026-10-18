# Decisions to agree with the other groups

What **has to be agreed** with the other groups so that our client works with
their servers and our server works with their clients — what is already
agreed, and what does **not** need agreeing because the slides decide it.

- The specification is in [`PROTOCOL.md`](PROTOCOL.md) (v0.7). This file says
  **what to ask**, what was agreed with whom, and what happens if two groups
  decide differently.
- Every row has **our position**, implemented and tested. Each decision lives
  in a single place in our code (see the "In our code" column), so changing
  it is cheap — the first agreement (group 1, below) changed four of them in
  one afternoon.
- Status of each point: **AGREED (g1)** = agreed with group 1; **OPEN** =
  still to ask. Once every **A** point is agreed with the groups we test with,
  `PROTOCOL.md` becomes v1.0.

| Section | What it is | Needs agreement? |
|---|---|---|
| **S** | Fixed by the slides | **No** — only confirm everyone reads the slide the same way |
| **A** | Breaks interoperability if groups differ | **Yes** — the real negotiation |
| **B** | Only changes what the user sees (or what our tests compare) | Nice to have |
| **C** | Internal to our implementation | No (worth mentioning) |
| **D** | Later milestones (M4–M6): what the slides fix and what is still open | Agree early |

### Why the list is short: strict when sending, tolerant when receiving

A point only needs agreement if it can **break** another group. Anything that
we never send, and that we accept gracefully when someone else sends it, needs
no agreement: whatever the other group decides, nothing breaks. For example,
our client never sends an empty line or two objects on one line, and our
server copes when someone else does — so how each server reacts to them is
its own business (section C).

The first conversation (group 1) showed it again: their nicks are counted in
**characters**, ours in **bytes**. Instead of negotiating, our client now
accepts nicks of up to 128 bytes from other servers (32 UTF-8 characters),
while our server keeps its 32-byte limit. Nothing to agree.

---

## Contact log

| Group | Language | Who | Date | S confirmed | A agreed | `interop.py` against their server | Their client against our server |
|---|---|---|---|---|---|---|---|
| **g1** — *(group name)* | Python (?) | *(friend's name)* | 2026-10-14 | yes | A1–A6 (A7: ask) | to do | to do |
| | | | | | | | |
| | | | | | | | |

### What we changed after talking to group 1

| Before (v0.5) | After (v0.6) | Why |
|---|---|---|
| `MSG` and `PRIV` were confirmed by the `CHAT`, no `OK` | `OK` to every accepted request, **before** the `CHAT` | the literal reading of slide 124 (*"Last request accepted"*); their server already did it, and neither client waits for `OK`, so it costs nothing |
| the sender of a `PRIV` received a copy (with a `"to"` field) | **no copy, no `"to"`**; our client echoes `/msg` locally | their client would have shown our copy as a message *to* the sender; a `CHAT` means "a message that reached you" |
| line ≤ 4096 bytes, above it `400` and close | line ≤ **64 KiB**, above it **`413`** and close | their limit; 413 = "connection unusable, closed", 400 = "refused, carry on" |
| text 1–1024 bytes, no control characters | text 1–**4096** bytes, **multi-line allowed**; above it `400 "text too long"`, connection stays open | their proposal; we kept "no empty text" |
| our client rejected a `CHAT` it could not fit, and closed on an over-long line | our client accepts and cuts (`[...]`), and discards an over-long line | tolerant receiving: their server has no text limit of its own |

---

## S — Fixed by the slides (no agreement needed, just confirm)

| # | What is fixed | Slide | What we do |
|---|---|---|---|
| S1 | The 10 message types, their fields and directions: `NICK`, `MSG`, `PRIV`, `LIST`, `QUIT` (C→S); `OK`, `CHAT`, `USERS`, `EVENT`, `ERROR` (S→C) | 124 | exactly the slide's fields, no extensions (`test_protocol` #31, #58, #59, #60) |
| S2 | **Newline-delimited JSON**: one JSON object per line, ending in `\n` | 123 (M1) | we send `\n`; we also accept `\r\n` and ignore empty lines (tolerant receiving) |
| S3 | Keys and `type` values are exactly as written (`"type"`, `"NICK"`) — JSON compares names exactly, so they are case-sensitive | 124 + JSON | `"Type"` or `"nick"` as a type → `ERROR 400` |
| S4 | `code` in `ERROR` is a JSON number (`"code":409`) | 124 | integer 100–999 |
| S5 | `NICK` **must be the first message** | 124 | our client always sends `NICK` first; our server answers anything else before it with `ERROR 401` |
| S6 | Nicknames are **unique** | 123 (M3) | `ERROR 409` |
| S7 | Nick in use → **`ERROR 409 "nick taken"`** | 124 | exactly that code and reason |
| S8 | `OK` means **"last request accepted"** | 124 | read literally (AGREED g1): every accepted request gets `OK` — see A1 |
| S9 | `USERS` is **the reply to `LIST`** | 124 | `LIST` → `USERS` (no `OK`) |
| S10 | `ERROR` means **"request refused"** | 124 | a refused request gets exactly one `ERROR`, and no `OK` |
| S11 | A private message arrives as a **`CHAT` with `"private":true`**; `PRIV` is what `/msg` sends | 123, 124 | `/msg rui hi` → `PRIV` → `CHAT … "private":true` to rui only |
| S12 | `EVENT` = **join and leave notices**; example text `"rui joined"` | 123 (M2), 124 | `"<nick> joined"` / `"<nick> left"` |
| S13 | `QUIT` = **"leave cleanly"** | 124 | the client sends `QUIT` on `/quit` and on Ctrl+D |
| S14 | **IPv4 and IPv6**: *"Use getaddrinfo() and sockaddr_storage for IPv4 / IPv6 portability"*; the client tries every candidate address (*"Retry another candidate"*) | 85 (sockets) | the server listens on both in one dual-stack socket; the client tries each address |
| S15 | A slow or stuck client must not block everyone: *"Use timeouts, or a send queue per client"* | 125 | 5 s send deadline (group 1 uses a send queue; both are fine — C3) |

**How to use this table with another group:** "We read slides 123–125 like
this — do you too?" If someone reads one differently, it moves to section A.

---

## A — Breaks interoperability if groups differ

| # | Question | Our position | Status | If groups differ | In our code | Test |
|---|---|---|---|---|---|---|
| A1 | **Which requests get an `OK`?** | every **accepted** request: `NICK`, `MSG`, `PRIV`, `QUIT` → `OK`, always **before** any `CHAT`; `LIST` → `USERS`; refused → only the `ERROR` | **AGREED (g1)** | a client that waits for `OK` after each `MSG` hangs on a server that does not send it; a server that sends it makes a naive client print `[ok]` everywhere (ours hides `OK`s) | `session.c` (D13) | `test_session` #83; `interop` 03, 08 |
| A2 | Does the sender of `MSG` receive its **own** `CHAT`? | **yes** — so clients do **not** echo `MSG` locally | **AGREED (g1)** | duplicated or missing messages | `SESS_ALL` (D5) | `interop` 03 |
| A3 | Does the sender of `PRIV` receive a **copy**? | **no** — only the `OK`; a client that wants to show what it sent does it **locally** | **AGREED (g1)** | with a copy, a client that echoes locally shows it twice, and one that does not shows it as a message *to* the sender | `handle_priv()` (D14) | `interop` 08 |
| A4 | Do receivers **ignore unknown fields**? | **yes** (and our server sends no extra fields: the `"to"` of v0.5 was dropped) | **AGREED (g1)** | a receiver that rejects unknown fields breaks with any extension | `protocol.c` | `interop` 05 |
| A5 | **Maximum line length** | **64 KiB including the `\n`**; above it `ERROR 413 "line too long"` and close | **AGREED (g1)** | a long line is accepted by one side and refused by the other | `RX_MAX_LINE` (`framing.h`) | `test_framing` #7, #21; `probe_m3.py` §8 |
| A6 | **`text` limit** | **1–4096 bytes** (after UTF-8 encoding), **multi-line allowed** (`\n` escaped by JSON); above it `ERROR 400 "text too long"`, connection stays open | **AGREED (g1)** (they accept empty text, we do not: B12) | a long message is accepted by one server and refused by another | `PROTO_TEXT_MAX` (`protocol.h`) | `test_protocol` #26, #81; `interop` 13, 14 |
| A7 | **Characters forbidden in a nick** | **space, `"`, `\` and control characters**; 1–32 **bytes** | **OPEN** (group 1 counts characters — harmless for us, see the note above; ask about `"` and `\`) | spaces break `/msg nick text`; a nick with `"`/`\` is legal on one server and refused on another | `proto_valid_nick()` (D20) | `test_protocol` #61 |

---

## B — Only changes what the user sees

None of these break anything; agreeing them makes the chat look the same
everywhere, and makes our conformance scenarios pass against other servers.

| # | Question | Our position | Status | If groups differ | In our code |
|---|---|---|---|---|---|
| B1 | Default port | `5000` | **AGREED (g1)** | everyone types the port by hand | `DEFAULT_PORT` |
| B2 | `QUIT`: does the server answer before closing? | **`OK`, then close**; also before `NICK` | **AGREED (g1)** (before `NICK`: ask) | a client expecting `OK` shows nothing; ours works either way | `case MT_QUIT` (D17) |
| B3 | Are nicks case-sensitive for uniqueness (and for finding the recipient of a `PRIV`)? | **no**: `Ana` = `ana` (ASCII letters only); shown as typed | **differs from g1** (case-sensitive) — kept: only changes which nicks each server allows | one server allows `Ana` and `ana` together; `/msg Rui` works on one server and gives 404 on another | `proto_nick_eq()` (D18) |
| B4 | After a `409`, does the connection stay open? | **yes**, the client can try another nick | OPEN | our client asks for another nick and retries; against a server that closes, it reports the close | `session.c` (D19) |
| B5 | Error codes | **400** malformed, **401** before `NICK`, **404** `PRIV` to someone not in the chat, **409** nick taken, **413** line too long, **503** server full | **AGREED (g1)** except 503 (ask); g1 also has **408** for idle (M4, D1) | users see different codes; our scenarios compare codes | `session.c`, `protocol.h` |
| B6 | `PRIV` to a nick that is not in the chat | `ERROR 404 "no such nick"` | **AGREED (g1)** | see B5 | `handle_priv()` (D15) |
| B7 | `PRIV` to oneself | `OK` + the `CHAT`, once | OPEN | may appear twice | `handle_priv()` |
| B8 | Who receives an `EVENT` | everyone **except** the client it is about | **AGREED (g1)** | the joiner sees a notice about themselves | `SESS_OTHERS` (D10) |
| B9 | `EVENT` text on leaving | `QUIT` and a dropped connection both give `"<nick> left"` | **AGREED (g1)** | a client that **parses** `EVENT` texts to keep a user list breaks — the list must come from `LIST` | `fill_event()` (D10) |
| B10 | Does `USERS` include the requester? In which order? | **yes**, **alphabetical ignoring case** | **AGREED (g1)** (they also list users of linked servers) | a counter shows N±1; our `10_list` scenario compares the exact list | `handle_list()` (D16) |
| B11 | Server full | `ERROR 503 "server full"`, then close; the limit is each group's choice (ours: 64) | OPEN | a close with no explanation | `server/main.c` (D12) |
| B12 | Empty `text` in `MSG`/`PRIV` | **refused** (`400 "invalid field"`) | **differs from g1** (they accept it) — kept | an empty message from their client gets a 400 from our server; an empty `CHAT` from their server is shown by our client | `get_request_text()` |
| B13 | Second `NICK` | `ERROR 400` (`"nick already set"`; g1 says `"already registered"`) | **AGREED (g1)** (code) | only the reason text differs | `session.c` |
| B14 | Help for whoever joins (the "`/help` on the server side" from the cross-test) | right after the `OK` to `NICK`, an `EVENT` **only to that client**: `"welcome, <nick>! commands: /list, /msg <nick> <text>, /quit"` | OPEN (new after the cross-test) | a client that treats the line after `OK` as special, or that parses `EVENT` texts, could be confused — a client that just displays `EVENT`s (ours, g1's) shows one more notice. `interop.py` skips `EVENT`s with another text | `SESS_EV_WELCOME` (D22) |

---

## C — Internal to our implementation (no agreement needed)

Either they are our own choices, or they only concern how we react to things
that **our** programs never send (strict when sending, tolerant when receiving).

| # | What | Our choice |
|---|---|---|
| C1 | Server concurrency | one thread per client (not `select`/`poll`) |
| C2 | Maximum clients | 64 (`ROSTER_MAX`) |
| C3 | Slow client (S15) | 5 s send deadline per message, total deadline (not `SO_SNDTIMEO`) |
| C4 | Invalid line (not JSON, missing/wrong field, two objects on a line) | `ERROR 400`; the connection **stays open** |
| C5 | A client sends a S→C type (`OK`, `CHAT`, `USERS`, …) | `ERROR 400 "unexpected type"` |
| C6 | `"from"` sent by a client | ignored: `from` is always the session's nick (no impersonation) |
| C7 | Traffic before `NICK` is accepted | none: the first line a client receives is always the answer to its `NICK`. Our client does not rely on this with other servers |
| C8 | Optional fields we receive | `private` absent = `false`; `reason` absent = `""` |
| C9 | What we accept from **other servers** | nicks up to 128 bytes (32 UTF-8 characters) and with `"`/`\`; `text`/`reason` of any length, empty included (cut to fit, shown with `[...]`); over-long lines are discarded, not fatal |
| C10 | Displaying text | control characters are shown as `?` (a `\033` could clear the screen); `\n` indents the following lines; `\t` stays |
| C11 | When a nick is freed | as soon as its connection leaves the list (`QUIT` or drop) |
| C12 | Client commands | `/list`, `/msg <nick> <text>`, `/quit`, `/help`, `//text` for a line starting with `/`; unknown commands are refused locally (never sent to the chat) |
| C13 | Client and `OK` | sends and keeps reading; `OK`s are not shown; only the answer to `NICK` is waited for (5 s) |
| C14 | `/msg` echo | local: `<ana -> rui> (privado) …`, since the server sends no copy (A3) |
| C15 | Language and libraries | C11 + POSIX + cJSON |
| C16 | The client's input line (fix after the cross-test) | in a terminal, the client draws the line being typed itself (raw mode), with the nick in front (`ana> `): incoming messages never mix with it, what is sent appears once (the typed line is erased on Enter and the server's `CHAT` stays), Enter on an empty line does nothing. Without a terminal (pipe) nothing changes |

---

## D — Later milestones: agree early

The slides already fix part of each milestone (slide 123). The rest is the
same kind of problem as above: if each group decides alone, it is only
discovered on interop day. Group 1 is ahead here: their choices are the
natural starting point.

| # | Milestone | Fixed by the slides | Still open | Initial idea (not implemented yet) |
|---|---|---|---|---|
| D1 | M4 | an **idle timeout** must exist | how long; is the client told before? | **as group 1:** 300 s, then `ERROR 408` and close |
| D2 | M4 | — | keepalive? (slide 124 has no `PING`; a new type would be `unknown type` elsewhere) | **no** new type: a client that wants to stay connected sends a `LIST` |
| D3 | M4 | **Ctrl+C shutdown that closes sockets** | are clients told? | `EVENT "server shutting down"` to everyone, then close |
| D4 | M4 | **automated tests with pytest** | — (internal) | our network tests are already Python scripts; turn them into pytest |
| D5 | M5 | the server answers a **UDP broadcast** called **`DISCOVER`** with **its TCP port** | exact JSON of the request and the reply; UDP port; client wait time | **as group 1:** request `{"type":"DISCOVER"}` (also accept the plain text `DISCOVER`); reply `{"type":"SERVER","tcp_port":5000,…}`; UDP port **5001**; the IP is the packet's **source**, never trusted from a field; accept `{"port":N}` from other groups |
| D6 | M6-A | file transfer with **length-prefixed binary frames** | the header and how it coexists with the JSON lines | to agree between the groups that pick it |
| D7 | M6-B | **TLS** with `ssl` and a **self-signed certificate** | same port or another? | another port (e.g. `5443`), so clients without TLS do not break; group 1 answers TLS bytes on the normal port with `ERROR 400` and closes |
| D8 | M6-C | chat rooms with **`/join` and `/leave`** | the message types (not in slide 124) | to agree between the groups that pick it |
| D9 | M6-D | supervisory node | server-to-server messages | group 1 already links servers (`PEER`, message ids, origin); worth reading before choosing |

---

## The questions to ask (in order)

For a new group:

1. **A1** — Which requests get an `OK`? (and do you read *"Last request accepted"* the same way? — S8)
2. **A2/A3** — Does the sender receive its own `CHAT`? And a copy of its `PRIV`?
3. **A5/A6** — Lines up to 64 KiB (413 above); text 1–4096 bytes, multi-line allowed (400 "text too long" above)?
4. **A7** — Nick rules: 1–32 bytes, no spaces, `"` or `\`?
5. **A4** — Do your receivers ignore unknown fields?
6. Section **S** — do you read the slides the same way?

Still to ask group 1: **A7** (`"`/`\` in nicks), **B2** (`QUIT` before `NICK`),
**B4** (does the connection stay open after a 409?), **B7**, **B11** (503),
**B14** (the welcome `EVENT`, new after the cross-test).

---

## Message to send to the groups

> Hi! We are the group doing the chat in C (repository Future_Networks). For
> M3 we have the full protocol implemented and tested (`docs/PROTOCOL.md`,
> v0.7), already agreed with one group. Most of it is fixed by the slides;
> only a few points really need agreeing (`docs/DECISIONS.md`, section A):
>
> 1. Which requests get an `OK`? Our reading of *"Last request accepted"*: every accepted request (`NICK`, `MSG`, `PRIV`, `QUIT`) gets an `OK`, always before the `CHAT`; `LIST` gets its `USERS`.
> 2. The sender of a `MSG` receives its own `CHAT` (so no local echo); the sender of a `PRIV` gets only the `OK`, no copy (echo it locally if you want).
> 3. Limits: lines up to 64 KiB including `\n` (`ERROR 413` and close above it); text 1–4096 bytes, multi-line allowed (`ERROR 400 "text too long"` above it, connection stays open).
> 4. Nicks: 1–32 bytes, no spaces, `"` or `\`?
> 5. Receivers ignore unknown fields?
> 6. After the `OK` to `NICK` our server sends a welcome `EVENT` with the commands, only to that client. Does your client just display it?
>
> Our server accepts IPv4 and IPv6 on port 5000. We also have plain-text test
> scenarios that run against any server (`python3 scripts/interop.py <ip>
> <port>`). Shall we set up a cross-test session: our client against your
> server and your client against ours?
