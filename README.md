# Future_Networks — a multi-user chat in C

A client/server chat over TCP, written in C with the POSIX sockets API, built
for the *Future Internet* course (Universidade de Aveiro, 2026/27).

The point of the project is **interoperability**: every group in the class
implements the same JSON-over-TCP protocol in its own language, and our client
has to work with their servers — and our server with their clients.

```
$ ./bin/client 192.168.43.10 5000 ana
[client] ligado a 192.168.43.10:5000 como ana (/help para os comandos, Ctrl+D para sair)
* welcome, ana! commands: /list, /msg <nick> <text>, /quit
* rui joined
<rui> hi everyone
<ana> hello!
[online: 2] ana rui
```

## Features

- **Multi-user chat** — broadcast messages, private messages, user list,
  unique nicknames (case-insensitive).
- **A simple, shared protocol** — one JSON object per line
  ([`docs/PROTOCOL.md`](docs/PROTOCOL.md)); you can talk to the server with
  plain `nc`.
- **Robust server** — one thread per client; a client that stops reading is
  cut off by a 5-second send deadline instead of freezing the chat; idle connections are
  closed (`408`); clean shutdown on `Ctrl+C`; up to 64 clients (`503` beyond).
- **LAN discovery** — run the client with no address and it finds servers on
  the local network by UDP broadcast.
- **TLS** — an optional second port with a self-signed certificate, where the
  client authenticates the server by its **SHA-256 fingerprint** (pinning).
- **Friendly terminal client** — incoming messages never mix with what you are
  typing; line editing with Backspace, `Ctrl+U`, `Ctrl+W`; a silent keepalive.
- **Well tested** — unit tests in C, 153 end-to-end tests with pytest, runs
  clean under AddressSanitizer, UBSan and ThreadSanitizer, and a
  language-independent conformance suite that runs against **any** server.

## Quick start

Requirements: Linux, `gcc`, `make`, [cJSON](https://github.com/DaveGamble/cJSON),
OpenSSL 3, and Python 3 with `pytest` for the system tests.

```sh
sudo apt install build-essential libcjson-dev libssl-dev python3-pytest   # Ubuntu/Debian
sudo pacman -S base-devel cjson openssl python-pytest                      # Arch

make                              # builds bin/server and bin/client
./bin/server 5000                 # terminal 1
./bin/client 127.0.0.1 5000 ana   # terminal 2
./bin/client 127.0.0.1 5000 rui   # terminal 3
```

Or without our client at all:

```sh
nc 127.0.0.1 5000
{"type":"NICK","nick":"bob"}
{"type":"MSG","text":"hello from nc"}
```

## Usage

### Server

```
./bin/server [-v | -q] [-i secs] [-n secs] [-u port] [-N name] [-T port] [-C cert] [-K key] [port]
```

| Option | Default | Meaning |
|---|---|---|
| `port` | `5000` | TCP port of the chat |
| `-v` / `-q` | | verbose (every message) / warnings only |
| `-i secs` | `300` | idle time before `ERROR 408` and disconnect |
| `-n secs` | `30` | time allowed to send `NICK` after connecting |
| `-u port` | `5001` | UDP port for discovery (`0` disables it) |
| `-N name` | hostname | name announced to discovery |
| `-T port` | off | also listen for **TLS** on this port (e.g. `5443`) |
| `-C` / `-K` | `certs/server.pem` / `certs/server.key` | certificate and private key |

`Ctrl+C` tells every client `server shutting down`, closes all connections and
exits with 0.

### Client

```
./bin/client [-k secs] [-s] [-f fingerprint] [<host> [port] [nick]]
```

- With no host, it **discovers** servers on the LAN: one found → connects;
  several → lets you pick; none → explains the likely causes.
- `-k secs` — keepalive interval (default 120, `0` disables).
- `-s` — connect over TLS; `-f <fingerprint>` — and verify the server (implies `-s`).

Inside the client:

| Type | Does |
|---|---|
| any text | message to everyone |
| `/msg <nick> <text>` | private message |
| `/list` | who is online |
| `/quit` or `Ctrl+D` | leave |
| `/help` | list the commands |
| `//text` | send a line that starts with `/` |

The client's own messages are in Portuguese (it was written for the class).

## The protocol at a glance

One JSON object per line, terminated by `\n`. Full specification, including
every edge case and the example sessions, in [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

| Client → server | Server → client |
|---|---|
| `{"type":"NICK","nick":"ana"}` | `{"type":"OK"}` |
| `{"type":"MSG","text":"hi"}` | `{"type":"CHAT","from":"ana","text":"hi","private":false}` |
| `{"type":"PRIV","to":"rui","text":"psst"}` | `{"type":"EVENT","text":"rui joined"}` |
| `{"type":"LIST"}` | `{"type":"USERS","nicks":["ana","rui"]}` |
| `{"type":"QUIT"}` | `{"type":"ERROR","code":409,"reason":"nick taken"}` |

| Error | When | Connection |
|---|---|---|
| `400` | invalid JSON, unknown type, bad field, text over 4096 bytes | stays open |
| `401` | a request before `NICK` | stays open |
| `404` | `PRIV` to someone who is not online | stays open |
| `408` | idle for 300 s, or no `NICK` within 30 s | closed |
| `409` | nickname already taken | stays open |
| `413` | a line longer than 64 KiB | closed |
| `503` | server full (64 clients) | closed |

## TLS

TLS runs on a **separate** port, so the plaintext port keeps working for every
other implementation and for `nc`.

```sh
scripts/gen-cert.sh                       # creates certs/ and prints the SHA-256 fingerprint
./bin/server -T 5443 5000                 # plaintext on 5000, TLS on 5443
./bin/client -f <fingerprint> 127.0.0.1 5443 ana
```

The certificate is self-signed, so there is no certificate authority to
trust: the client compares the server certificate's SHA-256 with the
fingerprint it was given, and refuses **before sending anything** if they
differ. Without `-f` the connection is encrypted but not authenticated, and
the client says so. The private key never leaves the machine (`certs/` is in
`.gitignore`, and `gen-cert.sh` refuses to run otherwise).

The two captures in [`docs/captures/`](docs/captures/) show the difference on
the wire: the same message is readable on port 5000 and nowhere to be found on
port 5443.

## Discovery

The client broadcasts `{"type":"DISCOVER"}` to UDP port 5001 (three times,
over 1.5 s); each server replies only to the asker with
`{"type":"SERVER","tcp_port":5000,"name":"...","users":2}`, and the server's
address is taken from the reply's source IP. If a firewall is on:

```sh
sudo ufw allow 5000/tcp && sudo ufw allow 5001/udp     # on the server
sudo ufw allow proto udp from any port 5001 to any     # on the client (the replies)
```

## Testing

```sh
make test              # unit tests (C), ~4 s
make pytest            # 153 end-to-end tests: the real server and client, real sockets
make clean && make test SAN=1 && make pytest SAN=1     # under ASan + UBSan
```

**Against any server** — ours or another group's, in any language:

```sh
python3 scripts/interop.py <host> <port>
```

It runs the scenarios in [`tests/interop/`](tests/interop/), each in three
sending modes: one line per `send()`, all lines glued in one `send()`, and one
byte at a time — the split and merged messages that TCP produces in real life.
A scenario is plain text: `>` is a line sent, `<` a line expected (compared as
JSON, extra fields ignored), `a>`/`b<` select one of several connections, and
`< CLOSED` waits for the server to close.

```
# tests/interop/07_nick_em_uso.txt (excerpt)
a> {"type":"NICK","nick":"ana"}
a< {"type":"OK"}
b> {"type":"NICK","nick":"ana"}
b< {"type":"ERROR","code":409}
```

Other tools in [`scripts/`](scripts/): `discover.py` shows who answers a
`DISCOVER` on the network and in what format; `gen-cert.sh` creates the TLS
certificate.

## Project layout

```
include/, src/common/   sockets, framing (bytes ↔ lines), protocol (lines ↔ JSON messages),
                        discovery datagrams, TLS layer, logging
src/server/             session state machine, shared client list, the server
src/client/             commands and display, terminal line editor, discovery, the client
tests/unit/             C unit tests            (make test)
tests/system/           pytest end-to-end tests (make pytest)
tests/interop/          language-independent conformance scenarios
scripts/                interop.py, discover.py, gen-cert.sh
docs/                   PROTOCOL.md, ARCHITECTURE.md, packet captures
```

## Design

How it is built and why — threads and locks, timeouts, how connections are
closed without losing the last message, TLS on top of an existing app — is
summarised in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

The project was developed in milestones (M0 echo server → M6 TLS). Each one
has its own branch with the full development notes, decisions and test
evidence (in Portuguese), and the source code comments are in Portuguese too.

## License

[MIT](LICENSE)
