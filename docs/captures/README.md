# Packet captures

| File | What it shows |
|---|---|
| `m0-handshake.pcap` | the first milestone's echo: TCP handshake (SYN / SYN-ACK / ACK), data, echo, FIN |
| `m6-plaintext.pcap` | our client on port 5000: the `NICK` and the message `SEGREDO-123` readable on the wire |
| `m6-tls.pcap` | the same on port 5443 (TLS 1.3): only the handshake and encrypted records — not the message, not the nick, not even the certificate (TLS 1.3 encrypts it too) |

Open them in Wireshark, or from the terminal:

```sh
tcpdump -r docs/captures/m0-handshake.pcap -n -tt
strings docs/captures/m6-plaintext.pcap | grep SEGREDO   # found
strings docs/captures/m6-tls.pcap       | grep SEGREDO   # not found
```

To record them again (loopback, needs `sudo`):

```sh
sudo tcpdump -i lo -n -U -w plain.pcap 'tcp port 5000' &
sudo tcpdump -i lo -n -U -w tls.pcap   'tcp port 5443' &
./bin/server -T 5443 5000 &
printf 'SEGREDO-123\n/quit\n' | ./bin/client 127.0.0.1 5000 ana
printf 'SEGREDO-123\n/quit\n' | ./bin/client -f <fingerprint> 127.0.0.1 5443 rui
```

`*.pcap` files are ignored by git everywhere except in this folder.
