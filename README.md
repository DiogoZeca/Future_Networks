# Future_Networks — Chat cliente/servidor em C

Projeto prático de **Internet do Futuro 2026/27** (UA): um chat multiutilizador
sobre TCP, desenvolvido por fases (M0–M6). O objetivo central é a
**interoperabilidade**: o nosso cliente deve falar com servidores de outros
grupos (noutras linguagens) e o nosso servidor deve aceitar os clientes deles.

O protocolo comum está em [`docs/PROTOCOL.md`](docs/PROTOCOL.md); o que ainda
falta acordar com os outros grupos está em [`docs/DECISIONS.md`](docs/DECISIONS.md).

## Fases

| #  | Fase                   | Estado |
|----|------------------------|--------|
| M0 | Echo warm-up           | DONE |
| M1 | Framing + protocolo    | DONE |
| M2 | Vários clientes        | DONE |
| M3 | Comandos + spec        | DONE (semântica acordada e testada com o grupo 1; faltam mais grupos) |
| M4 | Robustez e testes      | TODO |
| M5 | Descoberta UDP         | TODO |
| M6 | Extensão               | TODO |

Notas de cada fase em [`docs/milestones/`](docs/milestones/).

## Estrutura

```
include/         headers (net.h, framing.h, protocol.h, session.h, roster.h, ui.h, term.h)
src/common/      código usado por cliente e servidor
  net.c            sockets: criar, ligar, send_all, send_all_timeout (prazo total)
  framing.c        bytes <-> linhas (buffer de receção, split em '\n')
  protocol.c       linhas <-> mensagens JSON validadas
src/server/      servidor
  session.c        máquina de estados por ligação: 409, 404, USERS, QUIT (testada à unidade)
  roster.c         lista de clientes partilhada: lock, refcount, nicks únicos
  main.c           uma thread por cliente; broadcast e PRIV com prazo de envio
src/client/      cliente
  ui.c             texto e comandos (/list, /msg, /quit) <-> mensagens; editor de linha (testada à unidade)
  term.c           o terminal: linha de escrita com o nick à frente, sem se misturar com o que chega
  main.c           duas threads: teclado -> socket e socket -> ecrã
tests/unit/      testes framing / net / protocol / roster / session / ui (make test)
tests/interop/   cenários em texto, independentes da linguagem (1 ou várias ligações)
docs/            especificação do protocolo, decisões a acordar e notas por fase
scripts/         utilitários (interop contra qualquer servidor, sondas por fase)
```

## Compilar e correr

Dependência: [cJSON](https://github.com/DaveGamble/cJSON) para o JSON.

```sh
sudo pacman -S cjson            # Arch
sudo apt install libcjson-dev   # Ubuntu/Debian
make            # compila para bin/
./bin/server 5000
./bin/client <ip> 5000 ana      # o nick é pedido se não for dado
make test       # testes unitários (make test SAN=1 com sanitizers)
```

Teste rápido sem o nosso cliente: `nc <ip> 5000` e escrever JSON à mão.
Contra qualquer servidor: `python3 scripts/interop.py <ip> 5000`.
Vários clientes a sério contra o nosso servidor: `python3 scripts/probe_m2.py`
e `python3 scripts/probe_m3.py`.

No cliente: qualquer linha vai para toda a gente; `/list` mostra quem está,
`/msg <nick> <texto>` manda uma mensagem privada, `/quit` (ou Ctrl+D) sai e
`/help` mostra os comandos. Escreve-se à frente do nick (`ana> `): o que chega
dos outros aparece por cima, sem se misturar com o que se está a escrever.
Backspace, Ctrl+U (apagar a linha) e Ctrl+W (apagar a palavra) funcionam.

Cada fase tem em `docs/milestones/` uma tabela "O que os slides pedem" com a
resposta a cada ponto dos slides 123–125, onde está no código e como se prova.
