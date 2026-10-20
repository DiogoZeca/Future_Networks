# Capturas

Capturas de rede guardadas como prova de trabalho. O `.gitignore` ignora
`*.pcap`/`*.pcapng` em todo o repositório **excepto** nesta pasta.

| Ficheiro | Milestone | O que mostra |
|---|---|---|
| `m0-handshake.pcap` | M0 | SYN / SYN-ACK / ACK, dados, eco, FIN / FIN-ACK |
| `m6-claro.pcap` | M6 | o nosso cliente na porta 5000: o `NICK` e o `SEGREDO-123` legíveis no fio |
| `m6-tls.pcap` | M6 | o mesmo pela porta 5443 (TLS 1.3): só o handshake e records cifrados — nem o `SEGREDO-123`, nem o nick, nem sequer o certificado (o TLS 1.3 também o cifra) |

Reler sem abrir o Wireshark:

```sh
tcpdump -r docs/captures/m0-handshake.pcap -n -tt
strings docs/captures/m6-claro.pcap | grep SEGREDO     # aparece
strings docs/captures/m6-tls.pcap   | grep SEGREDO     # não aparece
```

Como se refazem as do M6: M6.md §5.
