# Testes unitários

Sem rede, determinísticos: `make test` (e `make test SAN=1`, que é o que conta).
Cada suite é um binário; o `Makefile` apanha sozinho cada `test_*.c` novo.

| Suite | Testa | Fase |
|---|---|---|
| `test_framing.c` | bytes -> linhas: mensagens partidas, coladas, `\r\n`, limites, UTF-8 | M1 |
| `test_protocol.c` | linhas -> mensagens: JSON inválido, campos em falta, limites | M1, M2 |
| `test_session.c` | máquina de estados e destino de cada saída | M1, M2 |
| `test_ui.c` | texto do teclado -> mensagem, mensagem -> ecrã | M1, M2 |
| `test_roster.c` | lista de clientes partilhada, refcount, com threads | M2 |
| `test_net.c` | `send_all_timeout()` com `socketpair()`: prazo total, meia mensagem | M2 |

Os números dos casos (`#n`) remetem para as tabelas de testes em
`docs/milestones/M1.md` e `M2.md` (o `test_roster.c` tem numeração própria, 1–14).
O ThreadSanitizer não combina com o `SAN=1`; ver `docs/milestones/M2.md`, "Testes".
