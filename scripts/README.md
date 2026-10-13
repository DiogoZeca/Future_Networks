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

Ao contrário do `probe_m0.py`, este é passa/falha: devolve `0` se tudo passou.
Resultado atual contra o nosso servidor: **18 ok, 0 falhas**.

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
