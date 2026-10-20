# DEMO — aula de 22 de outubro

O guião da demonstração em aula: o que preparar antes, como montar, o que
mostrar em cada bloco, com os comandos prontos a colar e o que se espera ver,
e o que fazer quando algo falha.

> *"you will not be able to do this in this time without a clear demo and test
> plan"* — o email do professor

## 0. O plano num relance

| Tempo | Bloco | O que se mostra | Quem |
|---|---|---|---|
| 20 min | **Montagem** (§2) | rede, build, firewall, IPs e fingerprint trocados com os outros grupos | A e B |
| ~15 min | **1. Cada programa sozinho** (§3) | servidor, cliente, `nc`, erros, descoberta, testes | A conduz, B escreve |
| ~15 min | **2. Interligação** (§4) | a matriz: o nosso cliente × servidores deles, os clientes deles × o nosso servidor, a descoberta | B conduz, A no log |
| ~15 min | **3. O que é nosso** (§5) | TLS com pinning (e o MITM), robustez, como testamos | A |
| resto | **4. Escalabilidade** (§6) | o desenho, os limites, e a prova ao vivo | A e B |

- **Portátil A** — o **servidor**, sempre visível (o log é o que conta a
  história). Quem conduz explica enquanto o outro escreve.
- **Portátil B** — os **clientes** (o nosso, `nc`, `interop.py`, `discover.py`).
- Com um terceiro portátil, fica com os clientes dos outros grupos e com o
  `tcpdump`.

Regra de ouro: **cada comando aqui foi corrido antes** (§1). Na aula não se
improvisa — copia-se daqui.

---

## 1. Antes da aula (véspera)

### 1.1 Em cada portátil

```sh
# dependências
sudo apt install build-essential libcjson-dev libssl-dev python3-pytest openssl netcat-openbsd tcpdump   # Ubuntu
sudo pacman -S base-devel cjson openssl python-pytest openbsd-netcat tcpdump                           # Arch

cd ~/Documents/Future_Networks
make clean && make            # sem avisos
make test                     # RESULTADO: tudo passou (~4 s)
make pytest                   # 153 passed (~2,5 min) — guardar o resumo
```

- [ ] `make` e `make test` passam nos **dois** portáteis
- [ ] `make pytest` passa — **guardar a última linha** (é o que se mostra no
      bloco 1, em vez de esperar 2,5 min na aula)
- [ ] no portátil A: `scripts/gen-cert.sh` → **anotar o fingerprint** (fica
      também aqui em baixo, na tabela da §2.4)
- [ ] ensaiar a demo inteira, com cronómetro, pelo menos uma vez

### 1.2 A rede

- **Plano A:** a rede de testes do laboratório (perguntar ao professor qual).
- **Plano B:** o hotspot de um telemóvel nosso — testado na véspera com os
  dois portáteis (passo a passo na conversa, e o M5.md, "Teste numa rede a
  sério").
- **Não** contar com o eduroam: costuma isolar os clientes uns dos outros e
  não deixa passar broadcasts.

### 1.3 Os outros grupos

- [ ] contactados antes da aula, com as perguntas do `DECISIONS.md` (portas
      5000/5001, formato do `DISCOVER`)
- [ ] o que responderam está no registo de contactos do `DECISIONS.md`
- [ ] se possível, um pré-teste com pelo menos um grupo (`interop.py` contra o
      servidor deles, 10 minutos)

### 1.4 Os terminais

No portátil A, três terminais abertos e já em `~/Documents/Future_Networks`:
**servidor** (letra grande, sempre visível), **cliente**, **comandos**. No B,
quatro: **cliente 1**, **cliente 2**, **nc**, **comandos**.

---

## 2. Montagem (20 min)

### 2.1 Rede e IPs (5 min)

```sh
hostname -I                    # nos dois: anotar o IP (ex.: 192.168.43.10)
ping -c 2 <IP de A>            # de B: tem de responder
```

### 2.2 Firewall (2 min) — só se estiver ativa

```sh
sudo ufw status
# A (servidor): chat, descoberta e TLS
sudo ufw allow 5000/tcp && sudo ufw allow 5001/udp && sudo ufw allow 5443/tcp
# B (cliente): as respostas da descoberta vêm DA porta 5001 (M5.md, nota 13)
sudo ufw allow proto udp from any port 5001 to any
```

### 2.3 Arrancar e verificar (5 min)

```sh
# A — o servidor, com as duas portas e um nome que se reconheça na descoberta
./bin/server -v -N grupo-zeca -T 5443 5000
```

Esperado no log de A:

```
a escuta na porta 5000 (max 64 clientes; inatividade 300 s, NICK em 30 s)
descoberta UDP na porta 5001 (nome "grupo-zeca")
TLS na porta 5443 (certificado certs/server.pem)
```

```sh
# B — verificação rápida antes de começar
python3 scripts/discover.py            # < <IP de A>:5001 {... "name":"grupo-zeca" ...}
python3 scripts/interop.py <IP de A> 5000   # interop: 42 ok, 0 falhas
```

### 2.4 Trocar dados com os outros grupos (8 min)

Escrever no quadro (ou numa mensagem para todos) e recolher o deles:

| Grupo | Linguagem | IP do servidor | Porta TCP | Descoberta (UDP) | Extra |
|---|---|---|---|---|---|
| **nós** | C | | 5000 | 5001, `{"type":"DISCOVER"}` | TLS na 5443, fingerprint: |
| | | | | | |
| | | | | | |
| | | | | | |

O fingerprint obtém-se com:

```sh
openssl x509 -in certs/server.pem -noout -fingerprint -sha256 | cut -d= -f2
```

---

## 3. Bloco 1 — Cada programa sozinho (~15 min)

O que se quer provar: **o nosso servidor e o nosso cliente fazem tudo o que o
slide 124 pede**, e respondem bem ao que está errado. O log de A fica sempre à
vista.

### 3.1 O chat (5 min)

```sh
# B, cliente 1                         # B, cliente 2
./bin/client <IP de A> 5000 ana        ./bin/client <IP de A> 5000 rui
```

| Fazer (onde) | Esperado |
|---|---|
| entrar (os dois) | `* welcome, ana! commands: ...`; o outro vê `* rui joined`; log de A: `entrou como rui (2 no chat)` |
| `ola a todos` (ana) | os dois veem `<ana> ola a todos` — o emissor também (é o servidor que manda o `CHAT`) |
| `/list` (rui) | `[online: 2] ana rui` |
| `/msg ana so para ti` (rui) | rui: `<rui -> ana> (privado) so para ti`; ana: `<rui> (privado) so para ti` |
| `/msg zz oi` (rui) | `[erro 404] no such nick` — só para quem se enganou |
| um 3.º cliente com o nick `ANA` | recusado com 409 (nicks sem olhar a maiúsculas); o cliente pede outro nick |
| `/quit` (rui) | ana vê `* rui left`; log: `saiu: QUIT` |

### 3.2 O protocolo em bruto, com `nc` (3 min)

Mostra que o servidor fala JSON por linha com **qualquer** cliente, e o que
faz com o que está mal:

```sh
nc <IP de A> 5000
{"type":"NICK","nick":"nc"}
{"type":"MSG","text":"via nc"}
{"type":"LIST"}
isto nao e json
{"type":"PRIV","to":"zz","text":"x"}
{"type":"QUIT"}
```

Esperado, linha a linha: `OK` + boas-vindas; `OK` + `CHAT`; `USERS`;
`ERROR 400 "invalid JSON"` **e a ligação continua**; `ERROR 404`; `OK` e fecho.

### 3.3 Descoberta (2 min)

```sh
./bin/client               # B, sem endereço nenhum
```

Esperado: `encontrado: 1) <IP de A>:5000  grupo-zeca  (N online)`, pede o nick
e entra. No log de A (com `-v`): `descoberta: DISCOVER de <IP de B> -> porta 5000, N no chat`.

### 3.4 Encerramento limpo (1 min)

`Ctrl+C` no servidor, com clientes ligados → cada um vê
`* server shutting down` e o fecho; o servidor sai com 0. **Voltar a arrancar
logo** (a porta fica livre de imediato: `SO_REUSEADDR`).

### 3.5 Como sabemos que funciona (3 min)

```sh
make test                                       # ao vivo: ~4 s
python3 -m pytest tests/system/test_tls.py -q -m "not slow"    # ao vivo: ~20 s
```

E mostrar o resumo do `make pytest` guardado na véspera (153 testes). Uma
frase: *"cada teste destes falhou antes da correção que o fez passar —
desfizemos as correções uma a uma para o confirmar (mutações, M4–M6)."*

**Plano B do bloco:** se a rede falhar, tudo isto corre em `127.0.0.1` num só
portátil — perde-se a descoberta entre máquinas, mais nada.

---

## 4. Bloco 2 — Interligação (~15 min)

O que se quer provar: **o nosso cliente funciona com os servidores dos outros e
o nosso servidor com os clientes deles.** Cerca de 3 minutos por grupo.

### 4.1 Por cada grupo, nesta ordem

```sh
# 1. o protocolo deles, de forma sistemática (14 cenários × 3 modos de envio)
python3 scripts/interop.py <IP deles> <porta deles>

# 2. o nosso cliente no servidor deles, a conversar com alguém deles
./bin/client <IP deles> <porta deles> zeca

# 3. o cliente deles no NOSSO servidor (eles correm-no; nós vemos o log de A)

# 4. descoberta nos dois sentidos
python3 scripts/discover.py            # aparece o servidor deles? em que formato?
./bin/client                           # o nosso cliente encontra-os e entra?
```

O `interop.py` imprime `ok`/`FALHA` por cenário e modo. Uma falha **não** é
necessariamente um erro deles: é uma diferença de semântica, e o
`DECISIONS.md` diz o que é cada uma.

### 4.2 A matriz (preencher na aula)

| Grupo | `interop.py` no servidor deles | o nosso cliente → servidor deles | cliente deles → o nosso servidor | descoberta: nós os achamos | descoberta: eles nos acham |
|---|---|---|---|---|---|
| g1 | | | | | |
| | | | | | |
| | | | | | |

### 4.3 O que dizer quando algo difere

- **Tolerantes a receber, estritos a enviar** (`DECISIONS.md`): o nosso cliente
  aceita nicks de outros servidores até 128 bytes, texto acima de 4096 bytes
  (mostra-o cortado, com `[...]`), e na resposta da descoberta lê `tcp_port`
  **ou** `port`.
- Cada decisão de semântica vive **num só sítio** (uma constante ou um `case`
  da `session.c`): mudar para o que os grupos acordarem é uma linha e um
  teste. Com o grupo 1 mudámos quatro numa tarde.
- O que foi acordado com o grupo 1 (14/10): `OK` a todos os pedidos aceites,
  `PRIV` sem cópia para o emissor, linha até 64 KiB (`413` e fecho), texto até
  4096 bytes (`400` e a ligação continua).

**Plano B do bloco:** se um grupo não tiver servidor a funcionar, correr o
`interop.py` contra o **nosso** e mostrar o que ele testa; se a descoberta não
passar na rede, `discover.py --to <IP deles>` (unicast) separa "o formato é
diferente" de "a rede não deixa passar broadcasts".

---

## 5. Bloco 3 — O que é nosso (~15 min)

### 5.1 TLS com certificado auto-assinado (M6) — 8 min

Uma porta **a mais** (5443), nunca em vez de: a 5000 continua a interoperar
com os outros grupos.

```sh
# B — com o fingerprint certo: liga, cifrado e autenticado
./bin/client -f <fingerprint> <IP de A> 5443 tls1

# B — ao mesmo tempo, um cliente em claro: os dois conversam no mesmo chat
./bin/client <IP de A> 5000 claro1

# B — fingerprint errado: recusa ANTES de o NICK sair
./bin/client -f 00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF <IP de A> 5443 x
# → [client] TLS: o certificado do servidor NAO corresponde ao fingerprint

# B — JSON em claro na porta TLS: recusado, e o servidor continua vivo
nc <IP de A> 5443          # escrever {"type":"NICK","nick":"x"}
```

**O man-in-the-middle** (o que o pinning protege, que a cifra sozinha não):

```sh
# B — um servidor FALSO, com outro certificado auto-assinado
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -keyout /tmp/k.pem -out /tmp/c.pem -subj "/CN=future-networks"
openssl s_server -quiet -accept 5444 -cert /tmp/c.pem -key /tmp/k.pem

# B, noutro terminal
./bin/client -f <fingerprint> 127.0.0.1 5444 ana   # recusa; o falso não recebe nada
./bin/client -s 127.0.0.1 5444 ana                 # sem -f: o falso recebe {"type":"NICK","nick":"ana"}
```

**No fio** — as capturas já estão no repositório (sem `sudo`):

```sh
strings docs/captures/m6-claro.pcap | grep SEGREDO   # aparece
strings docs/captures/m6-tls.pcap   | grep SEGREDO   # não aparece — nem o nick, nem o certificado (TLS 1.3)
```

(Ou ao vivo com `sudo tcpdump -i any -n -A 'tcp port 5000'` e `... 5443` em
dois terminais enquanto se escreve nos dois clientes.)

### 5.2 Robustez (M4) — 4 min

Escolher **dois** destes:

| Caso | Como | O que se vê |
|---|---|---|
| cliente que não lê | `python3 -m pytest tests/system/test_concurrency.py -k nao_le -q` (~10 s) | desligado aos 5 s; o chat continua |
| linha gigante | `python3 -m pytest tests/system -k "413" -q` | `ERROR 413` e fecho limpo (FIN, não RST) |
| inatividade | servidor com `-i 20`; um `nc` que faz `NICK` e fica calado | `ERROR 408` aos 20 s |
| servidor que morre | `kill -9` ao servidor com o nosso cliente ligado | o cliente diz que o servidor fechou, não fica pendurado |
| keepalive (só explicar) | o cliente manda um `LIST` silencioso a cada 120 s (`-k`) | um servidor que morre sem FIN é detetado pelo cliente |

### 5.3 Como testamos — 3 min

- **153 testes de sistema** (pytest, com o servidor e o cliente a sério) +
  unitários em C; também com ASan/UBSan e ThreadSanitizer.
- O `interop.py` corre cada cenário em **3 modos de envio** (uma linha por
  `send()`, coladas, byte a byte) — as mensagens partidas e coladas do slide
  125.
- **Mutações:** cada correção foi desfeita para ver o teste falhar (M4.md,
  M5.md, M6.md §7).
- Bugs que só os testes apanharam, para contar: o TLS desfazia o prazo de 5 s
  do M4 (M6.md, bug 4); um teste que falhava numa máquina e não noutra por
  causa da **versão do cJSON** (M6.md §7).

---

## 6. Bloco 4 — Escalabilidade

### 6.1 O desenho (o que dizer)

| Aspeto | O que fizemos | Porquê |
|---|---|---|
| concorrência | uma thread por cliente | simples e suficiente para uma turma; cada cliente lento só prende a sua thread |
| broadcast | tira um **retrato** da lista dentro do lock e envia **fora** dele | um envio lento nunca prende a lista (typical-problems §3.2) |
| escritas | um lock **por cliente** + prazo **total** de 5 s por envio | um cliente que não lê é desligado; os outros nem dão por ele (§2.4) |
| capacidade | máximo de 64 clientes; o 65.º recebe `ERROR 503` e fecho | um limite explícito em vez de cair; um `USERS` com 64 nicks cabe numa linha |
| inativos | `ERROR 408` aos 300 s; 30 s para mandar o `NICK` | ligações mortas não ocupam lugares para sempre |
| descoberta | UDP broadcast, resposta unicast só a quem pergunta | um servidor novo aparece sem configurar nada |

### 6.2 A prova ao vivo

```sh
python3 -m pytest tests/system/test_chat.py -k 503 -q          # 70 ligações para 64 lugares: 6 × 503
python3 -m pytest tests/system/test_concurrency.py -q          # corridas e clientes lentos (~15 s)
```

### 6.3 Os limites (dizê-los antes que perguntem)

- **Uma thread por cliente** não chega aos milhares (memória de stack, trocas
  de contexto). O passo seguinte seria `epoll` com um número fixo de threads.
- **O broadcast é O(N) por mensagem**: com N clientes a falar, O(N²) envios. É
  inerente a um chat de sala única; reduz-se com salas/canais.
- **64 está fixo no código** (`ROSTER_MAX`), por causa do array e do tamanho do
  `USERS`.
- **Um só servidor:** não há federação entre servidores; a descoberta encontra
  vários, mas cada um é uma sala à parte.

> Por decidir (ponto 4): medir mensagens/s e latência com N clientes e trazer
> a tabela para aqui.

---

## 7. Perguntas prováveis

O professor vai discutir "as partes fáceis e as difíceis". As nossas:

- **Fácil:** o protocolo JSON por linha; o servidor de um só cliente; a
  descoberta UDP (um datagrama chega inteiro ou não chega).
- **Difícil:**
  - o TCP é um *byte stream* — mensagens coladas e partidas (M1);
  - a concorrência: enviar fora do lock, mas com prazo, e o *check-then-act*
    dos nicks (M2, M3);
  - fechar bem: FIN e não RST, e o `linger_drain` (M4);
  - pôr TLS numa aplicação já feita: o `SSL*` não é dos dois sentidos ao mesmo
    tempo, e o cliente passou a precisar de dois locks (M6);
  - a interoperabilidade: tudo o que o slide não diz tem de ser combinado.

| Pergunta | Resposta curta | Onde |
|---|---|---|
| Porque é que o emissor recebe a própria mensagem? | acordado com o grupo 1: todos veem as mensagens pela mesma ordem, e o cliente não as ecoa (senão apareciam duas vezes); o `PRIV` é a exceção — só `OK`, eco local | PROTOCOL.md §4 |
| E se dois clientes escolherem o mesmo nick ao mesmo tempo? | a verificação e a reserva são feitas no mesmo lock (`roster_claim`) | typical-problems §3.6 |
| O que acontece a um cliente que não lê? | prazo total de 5 s por envio, depois é desligado | typical-problems §2.4 |
| Porque é que o TLS sem `-f` não chega? | cifra sem autenticar: um MITM é aceite (demonstrado) | M6.md D-TLS-2 |
| Porque duas portas e não TLS na 5000? | os outros grupos e o `nc` continuam a funcionar | M6.md D-TLS-1 |
| O TSan diz que não há corridas? | só no nosso código; não vê dentro da libssl | M6.md §6 |
| Porque UDP para a descoberta? | broadcast só existe em UDP; um pedido, uma resposta | M5.md, nota 1 |

---

## 8. Quando algo corre mal

| Sintoma | Causa provável | O que fazer |
|---|---|---|
| `Address already in use` | um servidor antigo ainda a correr | `pkill -f bin/server` e voltar a arrancar |
| o cliente não liga a `<IP>` | firewall, ou rede que isola clientes | §2.2; `ping`; trocar para o hotspot |
| a descoberta não encontra nada | broadcast bloqueado, ou firewall do **cliente** | `discover.py --to <IP>` (unicast); `./bin/client <IP>` continua a funcionar |
| `TLS desligado: nao foi possivel ler o certificado` | não há `certs/` neste portátil | `scripts/gen-cert.sh` e anotar o novo fingerprint |
| fingerprint "errado" com o certificado certo | gerou-se outro certificado | voltar a tirar o fingerprint (§2.4) |
| o servidor de outro grupo responde de forma inesperada | diferença de semântica | `interop.py` diz qual; ver §4.3 |
| um teste falha na aula | — | não depurar ao vivo: mostrar o resumo guardado e seguir |

---

## 9. Registo da demo (preencher depois)

- Rede usada:
- Grupos com quem interligámos:
- O que falhou e porquê:
- O que o professor comentou:
