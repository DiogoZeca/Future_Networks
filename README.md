# Future_Networks — Chat cliente/servidor em C

Projeto prático de **Internet do Futuro 2026/27** (UA): um chat multiutilizador
sobre TCP, desenvolvido por fases (M0–M6). O objetivo central é a
**interoperabilidade**: o nosso cliente deve falar com servidores de outros
grupos (noutras linguagens) e o nosso servidor deve aceitar os clientes deles.

O protocolo comum está em [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Fases

| #  | Fase                   | Estado |
|----|------------------------|--------|
| M0 | Echo warm-up           | DONE |
| M1 | Framing + protocolo    | DONE |
| M2 | Vários clientes        | TODO |
| M3 | Comandos + spec        | TODO |
| M4 | Robustez e testes      | TODO |
| M5 | Descoberta UDP         | TODO |
| M6 | Extensão               | TODO |

Notas de cada fase em [`docs/milestones/`](docs/milestones/).

## Estrutura

```
include/         headers (net.h, framing.h, protocol.h, session.h, ui.h)
src/common/      código usado por cliente e servidor
  net.c            sockets: criar, ligar, send_all
  framing.c        bytes <-> linhas (buffer de receção, split em '\n')
  protocol.c       linhas <-> mensagens JSON validadas
src/server/      servidor
  session.c        máquina de estados por ligação (testada à unidade)
  main.c           sockets à volta das camadas
src/client/      cliente
  ui.c             texto <-> mensagens (testada à unidade)
  main.c           pedido-resposta sobre o socket
tests/unit/      testes framing / protocol / session / ui (make test)
tests/interop/   cenários em texto, independentes da linguagem
docs/            especificação do protocolo e notas por fase
scripts/         utilitários (correr cenários de interop, etc.)
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
