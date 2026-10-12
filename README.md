# Future_Networks — Chat cliente/servidor em C

Projeto prático de **Internet do Futuro 2026/27** (UA): um chat multiutilizador
sobre TCP, desenvolvido por fases (M0–M6). O objetivo central é a
**interoperabilidade**: o nosso cliente deve falar com servidores de outros
grupos (noutras linguagens) e o nosso servidor deve aceitar os clientes deles.

O protocolo comum está em [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Fases

| #  | Fase                   | Estado |
|----|------------------------|--------|
| M0 | Echo warm-up           | ⏳ |
| M1 | Framing + protocolo    | ⏳ |
| M2 | Vários clientes        | ⏳ |
| M3 | Comandos + spec        | ⏳ |
| M4 | Robustez e testes      | — |
| M5 | Descoberta UDP         | — |
| M6 | Extensão               | — |

Cada fase fica marcada com uma tag git (`m0`, `m1`, …).
`git diff m1 m2` mostra exatamente o que mudou entre fases.
Notas de cada fase em [`docs/milestones/`](docs/milestones/).

## Estrutura

```
include/         headers partilhados (net.h, framing.h, protocol.h)
src/common/      código usado por cliente e servidor
  net.c            sockets: criar, ligar, send_all
  framing.c        bytes <-> linhas (buffer de receção, split em '\n')
  protocol.c       linhas <-> mensagens JSON validadas
src/server/      main do servidor
src/client/      main do cliente
tests/unit/      testes das camadas framing/protocol
tests/interop/   cenários em texto, independentes da linguagem
docs/            especificação do protocolo e notas por fase
scripts/         utilitários (correr cenários de interop, etc.)
```

## Compilar e correr

Dependência: [cJSON](https://github.com/DaveGamble/cJSON) para o JSON.

```sh
sudo apt install libcjson-dev   # Ubuntu/Debian (Fedora: cjson-devel, Arch: cjson)
make            # compila para bin/
./bin/server 5000
./bin/client <ip> 5000
```

Teste rápido sem o nosso cliente: `nc <ip> 5000` e escrever JSON à mão.
