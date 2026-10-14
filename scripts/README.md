# scripts

Utilitários de apoio ao desenvolvimento. **Não fazem parte do projeto entregue** - nada aqui é compilado nem ligado aos binários. São ferramentas para nos ajudar a
ver o que o código faz.

---

## `interop.py`

Corre os cenários de `tests/interop/*.txt` contra **qualquer** servidor — o
nosso ou o de outro grupo, em qualquer linguagem. Só usa a biblioteca padrão.

```sh
make && ./bin/server 5000 &                  # ou o servidor de outro grupo
python3 scripts/interop.py 127.0.0.1 5000    # todos os cenários
python3 scripts/interop.py <host> 5000 tests/interop/03_msg_chat.txt
```

Cada cenário corre em **três modos de envio**, porque é isso que o M1 tem de
aguentar (slide 125):

| Modo | Como envia | O que prova no servidor |
|---|---|---|
| `linha` | um `send()` por linha | o caso fácil |
| `colado` | as linhas `>` seguidas num só `send()` | mensagens coladas num `recv()` |
| `byte-a-byte` | um `send()` por byte, com `TCP_NODELAY` | mensagens partidas em muitos `recv()` |

Desde o M3 um cenário pode usar **várias ligações** (`a>`, `b<`, ...), esperar
o fecho (`< CLOSED`) e salta os `EVENT` que não estiver à espera — formato em
`tests/interop/README.md`.

Ao contrário do `probe_m0.py`, este é passa/falha: devolve `0` se tudo passou.
Resultado atual contra o nosso servidor: **42 ok, 0 falhas** (14 cenários × 3 modos).

No fim de cada cenário o script fecha com FIN e **espera que o servidor feche**
do lado dele (no máximo 1 s). Sem isso, como todos os cenários usam o nick
"ana", o cenário seguinte às vezes pedia "ana" antes de o servidor ter dado
pela saída do anterior, e levava um `409` falso (M3.md, P33).

---

## `probe_client.py`

O inverso do `interop.py`: testa o **nosso cliente** contra servidores falsos
que se portam como o servidor de outro grupo *podia* portar-se — sem `OK` ao
`NICK`, respostas byte a byte, um `EVENT` colado, nick recusado, fecho a meio,
linha gigante, `\r\n`, linha que não é JSON. Como o `probe_m0.py`, é uma sonda:
mostra o que o cliente imprime em cada caso. Resultados em `docs/milestones/M1.md`.

```sh
make
python3 scripts/probe_client.py         # portas 5190-5197
```

---

## `probe_m2.py`

Verificação do servidor multiutilizador. Arranca o seu próprio `./bin/server`
(porta 5210), liga clientes a sério e verifica o que **cada um** recebe — e o
que **não** deve receber. É passa/falha. Demora ~25 s.

```sh
make
python3 scripts/probe_m2.py
```

| § | Cenário | O que prova |
|---|---|---|
| 1–5 | três clientes entram, falam, erram e saem; um liga e desliga sem nick | `OK`, broadcast incluindo o emissor, `ERROR` só para quem errou, `EVENT` de entrada e saída |
| 6 | 1000 entradas enquanto alguém fala sem parar | a 1.ª linha depois do `NICK` é sempre o `OK` |
| 7 | um cliente entra e nunca lê; ~3 MB de tráfego | é desligado ao fim de 5 s e o chat continua |
| 8 | 70 ligações para 64 lugares | as que sobram recebem `ERROR 503` |

As secções 6–8 falham contra a primeira versão do M2: cada uma é um bug que
existiu (`docs/milestones/M2.md`, "Revisão do M2"). O log do servidor vai para
um ficheiro temporário e não para um pipe — um pipe que ninguém lê enche e
**para o servidor** (nota 6 do M2.md).

No M3 a §7 deixou de esperar um tempo fixo (0,5 s) pela última mensagem e
passou a esperar **até ela chegar** (no máximo 10 s): numa máquina mais lenta
os ~3 MB ainda estavam a ser entregues e a secção falhava sem haver bug no
servidor (nota 11 do `M3.md`).

---

## `probe_m3.py`

Verificação dos comandos do M3, no mesmo estilo do `probe_m2.py`: arranca o
seu próprio `./bin/server` (porta 5310) e liga clientes a sério. Passa/falha.

```sh
make
python3 scripts/probe_m3.py
```

| § | Cenário | O que prova |
|---|---|---|
| 1 | 50 rondas de 8 `NICK` iguais no mesmo instante | sempre 1 `OK` e 7 × `409` (verificar e reservar é atómico) |
| 2 | `ANA` com `ana` ligada, depois outro nick na mesma ligação | 409 sem olhar a maiúsculas; a ligação continua |
| 3 | `PRIV` para alguém, a si próprio, para quem não existe | o emissor recebe só o `OK`, só o destino recebe o `CHAT`; 404 |
| 4 | 1500 `PRIV` para quem entra e sai sem parar | só `OK` ou 404; o servidor sobrevive |
| 5 | 64 clientes com nicks de 32 bytes e um `LIST` | 64 nicks ordenados numa linha |
| 6 | `QUIT` e um `MSG` colados | `OK`, fecho, `left` aos outros, o `MSG` ignorado, nick livre |
| 7 | o nosso `./bin/client` com o stdin preparado | 409 → pede outro nick, `/list`, `/msg` com eco local, `/lsit` recusado, sem `[ok]` no ecrã, `/quit` |
| 8 | linhas de 64 KiB e 64 KiB + 1; texto de várias linhas com `\033` | 400 `text too long` vs 413 + fecho; o texto chega igual; o nosso cliente mostra-o limpo |

Atenção a um pormenor do Python que enganou a primeira versão: `sock.close()`
não fecha a ligação enquanto houver um `sock.makefile()` aberto sobre ela
(nota 9 do `M3.md`).

---

## `probe_m0.py`

Verificação funcional do servidor de echo. Arranca o `./bin/server`, provoca cinco cenários difíceis de reproduzir à mão, mata o servidor e mostra o log dele.

Não é um teste no sentido de "passa/falha", é uma **sonda**: mostra o que acontece, e o nosso trabalho é saber explicar porquê.

### Como correr

```sh
make                          # a partir da raiz do repositório
python3 scripts/probe_m0.py   # usa a porta 5099 por omissão
python3 scripts/probe_m0.py 6000
```

> Não é preciso ter um servidor a correr: o script arranca o seu próprio, numa porta diferente da habitual (5099), para não colidir com um que esteja aberto noutro terminal.

### Os cinco cenários

| # | O que provoca                                     | Resultado esperado (M0)                      | O que prova                                |
|---|---------------------------------------------------|----------------------------------------------|--------------------------------------------|
| 1 | Envia `hello\n` e lê a resposta                   | echo igual                                   | o caminho feliz funciona                   |
| 2 | Liga um **2.º cliente** enquanto o 1.º está ativo | **timeout**                                  | servidor iterativo: liga mas não é servido |
| 3 | `send()` único de **10 001 bytes**                | eco completo, mas **3 `recv()` no servidor** | TCP é byte stream, não preserva fronteiras |
| 4 | Fecha com `SO_LINGER{1,0}` → **RST**              | `send: Connection reset by peer`             | fecho abrupto tratado sem matar o servidor |
| 5 | `shutdown(SHUT_WR)` → **half-close**              | servidor devolve `b''`                       | `recv()==0` tratado corretamente           |

> **Atenção ao cenário 2:** aqui o **timeout é o resultado esperado**

### Onde está a evidência

O script imprime cinco linhas `[probe] ...`, mas **a parte importante é o log do servidor**, impresso no fim em `--- LOG DO SERVIDOR ---`. É lá que se vê o que interessa:

```bash
[server] recv de 4096 bytes       <- um unico send() de 10 001 bytes
[server] recv de 4096 bytes          chegou ao servidor em TRES recv()
[server] recv de 1809 bytes
```

O script captura o `stdout` **e** o `stderr` do servidor, porque as mensagens de `perror()` (`recv: Connection reset by peer`) saem em `stderr`.


### Porque é em Python

E há uma vantagem acidental: o probe é **um cliente que não escrevemos nós, noutra linguagem, com outra biblioteca de sockets**. Se o nosso servidor o aguenta, é um
primeiro ensaio de interoperabilidade, que é metade do objetivo do projeto.
