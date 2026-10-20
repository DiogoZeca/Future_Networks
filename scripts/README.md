# scripts

Utilitários de apoio. **Não fazem parte do projeto entregue** — nada aqui é
compilado nem ligado aos binários.

Até ao M3 havia aqui também as sondas `probe_m0.py`, `probe_m2.py`,
`probe_m3.py` e `probe_client.py`. No M4 passaram a testes de sistema com
pytest, em [`tests/system/`](../tests/system/) (`make pytest`), e saíram daqui
para não haver duas versões do mesmo teste. Continuam nos branches `M0`–`M3`,
como estavam no fim de cada fase.

---

## `interop.py`

Corre os cenários de `tests/interop/*.txt` contra **qualquer** servidor — o
nosso ou o de outro grupo, em qualquer linguagem. Só usa a biblioteca padrão.

```sh
make && ./bin/server 5000 &                  # ou o servidor de outro grupo
python3 scripts/interop.py 127.0.0.1 5000    # todos os cenários
python3 scripts/interop.py <host> 5000 tests/interop/03_msg_chat.txt
```

O mesmo motor corre dentro do pytest (`tests/system/test_interop.py`), contra
o nosso servidor ou, com `--host <ip> --port <p>`, contra o de outro grupo.

Cada cenário corre em **três modos de envio**, porque é isso que o M1 tem de
aguentar (slide 125):

| Modo | Como envia | O que prova no servidor |
|---|---|---|
| `linha` | um `send()` por linha | o caso fácil |
| `colado` | as linhas `>` seguidas num só `send()` | mensagens coladas num `recv()` |
| `byte-a-byte` | um `send()` por byte, com `TCP_NODELAY` | mensagens partidas em muitos `recv()` |

Desde o M3 um cenário pode usar **várias ligações** (`a>`, `b<`, ...) e
esperar o fecho (`< CLOSED`). Os `EVENT` que não se esperam são saltados
(também à espera do fecho), e à espera de um `EVENT` concreto saltam-se os de
outro texto (as boas-vindas, D22)
— formato em `tests/interop/README.md`.

É passa/falha: devolve `0` se tudo passou. Resultado atual contra o nosso
servidor: **42 ok, 0 falhas** (14 cenários × 3 modos).

No fim de cada cenário o script fecha com FIN e **espera que o servidor feche**
do lado dele (no máximo 1 s). Sem isso, como todos os cenários usam o nick
"ana", o cenário seguinte às vezes pedia "ana" antes de o servidor ter dado
pela saída do anterior, e levava um `409` falso (M3.md, P33).

---

## `discover.py` (M5)

Quem responde a um `DISCOVER` nesta rede, e **como**: manda o pedido por
broadcast (`255.255.255.255` e o broadcast de cada interface) e mostra cada
resposta **em bruto**, com o IP de origem, e o que o nosso cliente faria com
ela. Para o dia da interoperabilidade: separa "a rede não deixa passar"
(ninguém responde) de "o formato é outro" (responde, mas o cliente ignora).

```sh
python3 scripts/discover.py                       # broadcast, porta 5001, 2 s
python3 scripts/discover.py --plain               # pedido em texto: DISCOVER
python3 scripts/discover.py --to 192.168.1.20     # um servidor só (unicast)
```

```
> {"type":"DISCOVER"} para 255.255.255.255:5001
< 192.168.1.23:5001  b'{"type":"SERVER","tcp_port":5000,"name":"g1","users":2}\n'
    ok: porta TCP 5000 via 'tcp_port'
```

---

## `gen-cert.sh`

Gera o certificado auto-assinado e a chave do servidor (M6), e imprime o
**fingerprint SHA-256** — que é o que o cliente precisa para autenticar.

```sh
scripts/gen-cert.sh             # -> certs/server.pem, certs/server.key
```

```
fingerprint SHA-256 (e isto que o cliente precisa):
  52:59:68:E4:98:3B:0B:07:...
```

**Recusa correr se o `.gitignore` não proteger o `certs/`.** Não é zelo a mais:
uma chave privada que entre no histórico do git fica comprometida para sempre,
mesmo depois de apagada. Por isso a ordem é sempre `.gitignore` primeiro.

A chave sai com `chmod 600` e **nunca** é commitada — cada máquina gera a sua.
Para ligar:

```sh
./bin/server -T 5443 5000
./bin/client -s -f <fingerprint> 127.0.0.1 5443 ana
```
