# Capturas

Capturas de rede guardadas como prova de trabalho. O `.gitignore` ignora
`*.pcap`/`*.pcapng` em todo o repositório **excepto** nesta pasta.

| Ficheiro | Milestone | O que mostra |
|---|---|---|
| `m0-handshake.pcap` | M0 | SYN / SYN-ACK / ACK, dados, eco, FIN / FIN-ACK |

Reler sem abrir o Wireshark:

```sh
tcpdump -r docs/captures/m0-handshake.pcap -n -tt
```
