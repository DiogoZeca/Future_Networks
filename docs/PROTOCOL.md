# Chat Protocol — v0.1 (draft)

Shared specification for interoperability between groups (IF 2026/27).
Message names and fields come from the course slides (slide 124) and are
**fixed**. Sections marked *TO AGREE* are proposals open for discussion.

## 1. Transport

- TCP. Default port: *TO AGREE* (proposal: `5000`).
- Server binds to all interfaces (`0.0.0.0`).

## 2. Framing

- One JSON object per line, UTF-8 encoded, terminated by `\n` (0x0A).
- Senders MUST send `\n`; receivers MUST also accept `\r\n`.
- No pretty-printing: a message never contains a literal newline.
- Maximum line length: *TO AGREE* (proposal: 4096 bytes including `\n`).

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

General rules:
- Receivers MUST ignore unknown fields (allows optional extensions).
- Key order is not significant.

## 4. Semantics — TO AGREE

- [ ] Which requests get an `OK`? (all / only `NICK`, `PRIV`, `QUIT`)
- [ ] Does the sender of `MSG` receive its own `CHAT`?
- [ ] What does the sender of `PRIV` receive? (`OK` / copy as `CHAT` with `private:true`)
- [ ] Nick rules: max length, allowed characters, case-sensitive?
- [ ] Exact `EVENT` text for join/leave.
- [ ] Does `QUIT` get an `OK` before the server closes?
- [ ] Unknown `type` → `ERROR`?
- [ ] Missing optional fields: default values (e.g. `private` absent = `false`).

## 5. Error codes — TO AGREE

| Code | Reason            | When                                      |
|------|-------------------|-------------------------------------------|
| 409  | nick taken        | `NICK` already in use *(from the slides)* |
| 400  | *proposal*        | Invalid JSON / missing field              |
| 401  | *proposal*        | Request before a successful `NICK`        |
| 404  | *proposal*        | `PRIV` to an unknown nick                 |

## 6. Example sessions

*To be filled in, mirrored by `tests/interop/`.*

## Changelog

- v0.1 — initial draft.
