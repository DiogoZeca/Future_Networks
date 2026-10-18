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
| M4 | Robustez e testes      | DONE (idle 408, Ctrl+C limpo, log, pytest) |
| M5 | Descoberta UDP         | TODO |
| M6 | Extensão               | TODO |

Notas de cada fase em [`docs/milestones/`](docs/milestones/).

## Estrutura

```
include/         headers (net.h, framing.h, protocol.h, session.h, roster.h, ui.h, term.h, log.h)
src/common/      código usado por cliente e servidor
  net.c            sockets: criar, ligar, send_all, send_all_timeout (prazo total),
                   wait_readable (esperar com prazo), linger_drain (fechar sem RST)
  log.c            o log do servidor: hora, nível, uma linha por evento
  framing.c        bytes <-> linhas (buffer de receção, split em '\n')
  protocol.c       linhas <-> mensagens JSON validadas
src/server/      servidor
  session.c        máquina de estados por ligação: 409, 404, USERS, QUIT (testada à unidade)
  roster.c         lista de clientes partilhada: lock, refcount, nicks únicos
  main.c           uma thread por cliente; broadcast e PRIV com prazo de envio;
                   idle timeout (408), Ctrl+C que fecha tudo e sai com 0
src/client/      cliente
  ui.c             texto e comandos (/list, /msg, /quit) <-> mensagens; editor de linha (testada à unidade)
  term.c           o terminal: linha de escrita com o nick à frente, sem se misturar com o que chega
  main.c           teclado -> socket, socket -> ecrã, e o keepalive (LIST silencioso)
tests/unit/      testes framing / log / net / protocol / roster / session / ui (make test)
tests/system/    servidor e cliente a sério, com pytest (make pytest)
tests/interop/   cenários em texto, independentes da linguagem (1 ou várias ligações)
docs/            especificação do protocolo, decisões a acordar e notas por fase
scripts/         interop.py: os cenários contra qualquer servidor
```

## Compilar e correr

Dependência: [cJSON](https://github.com/DaveGamble/cJSON) para o JSON.

```sh
sudo pacman -S cjson            # Arch
sudo apt install libcjson-dev   # Ubuntu/Debian
make            # compila para bin/
./bin/server 5000               # [-v | -q] [-i inatividade s] [-n prazo do NICK s]
./bin/client <ip> 5000 ana      # o nick é pedido se não for dado; [-k keepalive s]
make test       # testes unitários (make test SAN=1 com sanitizers)
make pytest     # testes de sistema (precisa do pytest: sudo apt install python3-pytest)
```

O servidor sai com Ctrl+C: avisa os clientes (`server shutting down`), fecha
todas as ligações e sai com 0. Quem estiver 300 s sem mandar nada recebe
`ERROR 408` e é desligado; o nosso cliente manda um `LIST` silencioso a cada
120 s para isso não acontecer a quem só está a ler.

Teste rápido sem o nosso cliente: `nc <ip> 5000` e escrever JSON à mão.
Contra qualquer servidor: `python3 scripts/interop.py <ip> 5000` (ou
`python3 -m pytest tests/system/test_interop.py --host <ip> --port 5000`).

No cliente: qualquer linha vai para toda a gente; `/list` mostra quem está,
`/msg <nick> <texto>` manda uma mensagem privada, `/quit` (ou Ctrl+D) sai e
`/help` mostra os comandos; Ctrl+C também sai (manda `QUIT`). Escreve-se à frente do nick (`ana> `): o que chega
dos outros aparece por cima, sem se misturar com o que se está a escrever.
Backspace, Ctrl+U (apagar a linha) e Ctrl+W (apagar a palavra) funcionam.

Cada fase tem em `docs/milestones/` uma tabela "O que os slides pedem" com a
resposta a cada ponto dos slides 123–125, onde está no código e como se prova.
