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

## 7. Mapa de comportamentos — "o que acontece se…?"

O professor vai mandar **fazer coisas** e perguntar *"porque é que fez isto?"*
e *"o que farias para mudar?"*. Para cada experiência: **o que se vê**, **que
decisão nossa o causa** (com o `D` dos milestones e o sítio no código) e **a
alternativa**. Tudo isto foi corrido a 20/10 contra o código do M6 (os prazos
longos com `-i`, `-n` e `-k` curtos); onde não foi, diz-se.

Como ler a coluna *Porquê*: `D23` = decisão 23 (as tabelas de decisões dos
`docs/milestones/M*.md`); `tp §2.3` = `typical-problems.md` secção 2.3.

### 7.1 Entrar e sair

| # | Experiência | O que se vê | Porquê | Se quisesse outra coisa |
|---|---|---|---|---|
| 1 | entrar | `OK`, depois `EVENT "welcome, <nick>! commands: ..."` só para quem entrou; os outros `"<nick> joined"` | `OK` ao `NICK` (D6); boas-vindas como `EVENT` porque o slide não tem `HELP` e qualquer cliente mostra um `EVENT` (D22); entra no chat **depois** do `OK`, senão um `CHAT` de outro passava-lhe à frente (D9) | tirar as boas-vindas é uma linha em `session.c` |
| 2 | `/quit`, ou `Ctrl+D` | `OK`, o servidor fecha, os outros veem `"<nick> left"` | `QUIT` → `OK` e fecho (D17); o `Ctrl+D` manda `QUIT` na mesma (D17) | — |
| 3 | `Ctrl+C` no cliente | sai como o `/quit`; um 2.º `Ctrl+C` mata logo | o handler marca e sai pelo caminho normal (`term.c`) | — |
| 4 | `QUIT` antes do `NICK` | `OK` e fecho; ninguém é avisado | sair tem de ser sempre possível (D17); sem nick não há `"left"` (D10) | — |
| 5 | ligar e não mandar `NICK` (`nc` calado) | `ERROR 408 "nick timeout"` aos **30 s**, fecho | quem não se identifica não ocupa um dos 64 lugares (D23); `-n` muda o prazo | — |
| 6 | o 65.º cliente | `ERROR 503 "server full"` e fecho; o nosso cliente mostra `[erro 503] server full` e sai com 1 | `ROSTER_MAX` = 64, array fixo (D12; `roster.h`) | lista dinâmica; mas um `USERS` com todos tem de caber numa linha de 64 KiB (`_Static_assert` em `roster.h`) |

### 7.2 Mortes e silêncios — a parte mais perguntada

| # | Experiência | O que se vê | Porquê | Se quisesse outra coisa |
|---|---|---|---|---|
| 7 | `kill -9` a um cliente | os outros veem `"x left"` **logo**; log: `fechou a ligacao (FIN)` | o processo morre, o **kernel** fecha o socket (FIN, ou RST se havia dados por ler — tp §2.2) e o `recv()` do servidor dá 0 | — |
| 8 | **desligar o Wi-Fi** (ou o cabo) do cliente | **nada**, durante minutos. Servidor: `ERROR 408 "idle timeout"` aos **300 s**. O nosso cliente: sai ao fim de ~**240 s** (`o servidor nao responde`) | morte silenciosa: nem FIN nem RST, ninguém sabe (tp §2.3). As defesas são o idle timeout (D23) e o keepalive do cliente: um `LIST` silencioso aos 120 s sem enviar, e sem `USERS` em mais 120 s → desiste (D25). *Não testado com Wi-Fi real; simulado congelando o processo do outro lado* | prazos mais curtos (`-i`, `-k`) ou `SO_KEEPALIVE` do TCP; o custo é tráfego e falsos positivos numa rede lenta |
| 9 | `Ctrl+Z` no nosso cliente, **chat calado** | `ERROR 408` aos 300 s; ao fazer `fg`, o cliente mostra `[erro 408] idle timeout` e `o servidor fechou a ligacao` | o processo parado não envia nada — nem o keepalive (D23) | — |
| 10 | `Ctrl+Z` no nosso cliente, **chat com muito tráfego** | desligado **aos 5 s depois de os buffers encherem** (centenas de KB a MB); log: `nao le ha 5000 ms: a desligar`; os outros nem dão por ele | prazo **total** de 5 s por envio, quem esgota é marcado `dead` e desligado (D11; tp §2.4) | uma fila de envio por cliente, com uma thread de escrita, em vez de desligar (M2.md, nota 7) |
| 11 | *porquê 9 ≠ 10?* | a mesma ação, dois mecanismos | o 9 é "não fala" (idle, do lado da leitura); o 10 é "não lê" (prazo de envio, do lado da escrita) | — |
| 12 | mandar só lixo a cada segundo, depois do `NICK` | `400` a cada linha, e **nunca** leva o 408 | depois do `NICK`, **qualquer linha completa** renova o prazo, mesmo recusada (D23) — antes do `NICK` não renova | contar só mensagens aceites (hoje, "mensagem" = qualquer linha completa) |

### 7.3 O protocolo — o que é recusado, e se a ligação continua

Regra: **`400` = este pedido foi recusado, a ligação continua; `413` = a
ligação não está em condições, fecha** (revisão do M3, acordado com o grupo 1).

| # | Linha enviada (depois do `NICK`, salvo dito) | Resposta | Liga. | Porquê |
|---|---|---|---|---|
| 13 | `isto nao e json` | `400 "invalid JSON"` | continua | erro do pedido, não da ligação |
| 14 | `{"type":"FOO"}` | `400 "unknown type"` | continua | só os tipos do slide 124 |
| 15 | sem `type`; `text` número; `text` vazio; nick com espaço, vazio ou com 33 bytes | `400 "invalid field"` | continua | limites D4/D20; nick sem espaços por causa do `/msg <nick>` |
| 16 | `text` com 4097 bytes | `400 "text too long"` | continua | limite de 4096 (D4 revista, grupo 1) |
| 17 | linha com mais de 64 KiB | `413 "line too long"` e **fecho (FIN)** | fecha | não se sabe onde começa a mensagem seguinte: não há ressincronização (D1/D2) |
| 18 | `MSG`/`LIST` antes do `NICK` | `401 "nick required"` | continua | proposta nossa — o slide não diz |
| 19 | `NICK` outra vez | `400 "nick already set"` | continua | o nick não muda a meio |
| 20 | nick que já existe, **noutras maiúsculas** (`ANA` com `ana` lá dentro) | `409 "nick taken"`; o nosso cliente pede outro | continua | sem olhar a maiúsculas, só ASCII: `Zé` ≠ `ZÉ` (D18, D19; `proto_nick_eq`) |
| 21 | `{"type":"MSG","text":"x","cor":"azul"}` | `OK` + `CHAT` | continua | campos desconhecidos ignorados (`PROTOCOL.md` §3) |
| 22 | `{"type":"MSG","text":"x","from":"admin"}` | `OK` + `CHAT` com `from` = **o nick da sessão** | continua | não dá para se fazer passar por outro |
| 23 | UTF-8 inválido no `text` | `400 "invalid JSON"` | continua | o JSON tem de ser UTF-8 (RFC 8259) e o cJSON não verifica: validamos nós antes (M4, P38; `utf8_valid` em `protocol.c`) |
| 24 | `{"type":"LIST"}{"type":"LIST"}` | `400 "invalid JSON"` | continua | uma mensagem por linha |
| 25 | linha vazia | nada | continua | ignorada |
| 26 | `PRIV` a si próprio | `OK` + `CHAT` privado para si | continua | é uma mensagem que lhe chega |
| 27 | `PRIV` a quem não existe | `404 "no such nick"`, só para quem pediu | continua | D15 |
| 28 | `MSG` | `OK` e **também** o próprio `CHAT` | continua | o emissor recebe o seu `CHAT` (D5): todos veem a mesma ordem, e o cliente não ecoa (senão aparecia duas vezes). O `PRIV` é a exceção: só `OK`, eco local (D14) |
| 29 | `LIST` | `USERS` com **todos, incluindo quem pediu**, por ordem alfabética | continua | D16 |
| 30 | texto com `\n` (escapado no JSON) ou um carácter de controlo (`\u0007`) | aceite e reenviado tal e qual | continua | o **servidor** transporta; quem **mostra** é que limpa: o nosso cliente indenta as linhas e troca o controlo por `?` (P32) |

### 7.4 Framing — o TCP é um *byte stream*

| # | Experiência | O que se vê | Porquê |
|---|---|---|---|
| 31 | duas mensagens num só `send()` | as duas respostas | o ciclo extrai **todas** as linhas do buffer — `while`, não `if` (M1, P2) |
| 32 | uma mensagem byte a byte (`interop.py`, modo `byte-a-byte`) | uma resposta, no fim | junta-se no `rxbuf` até ao `\n` (`framing.c`) |
| 33 | uma linha **sem** `\n` | nada — o servidor espera; ao chegar o `\n`, responde | o `\n` é o fim da mensagem |
| 34 | `\r\n` em vez de `\n` | aceite | tolerante a receber (`PROTOCOL.md` §2) |
| 35 | `QUIT` com mais linhas coladas atrás | `OK`, fecho; o que vem depois é ignorado — **e o `OK` chega sempre** | `linger_drain()`: FIN e ler o resto antes do `close()`, senão o kernel mandava RST e o `OK` podia perder-se (D26; tp §2.2) |

### 7.5 Vários clientes ao mesmo tempo

| # | Experiência | O que se vê | Porquê | Se quisesse outra coisa |
|---|---|---|---|---|
| 36 | dois clientes pedem o **mesmo nick ao mesmo tempo** | só um ganha; o outro leva `409` | verificar e reservar no **mesmo** lock (`roster_claim`; tp §3.6); com dois locks, falhava 36 em 200 | — |
| 37 | alguém entra a meio de muito tráfego | a 1.ª coisa que recebe é **sempre** o `OK` | entra no chat depois do `OK` (D9); medido antes da correção: 26 em 2000 viam outra coisa | — |
| 38 | um cliente sai enquanto se faz broadcast para ele | nada de estranho, nenhum crash | contagem de referências: o socket só fecha quando ninguém tem o ponteiro (D7; tp §3.4) — senão o número do fd podia ser reutilizado por outra ligação | — |
| 39 | um cliente lento | os outros não dão por nada | o broadcast tira um retrato da lista e envia **fora** do lock; cada cliente tem o seu `wlock` e o prazo de 5 s (D11; tp §3.2) | ver 10 |

### 7.6 O servidor

| # | Experiência | O que se vê | Porquê |
|---|---|---|---|
| 40 | `Ctrl+C` no servidor | cada cliente recebe `EVENT "server shutting down"` e o fecho; o servidor sai com **0** logo | D24: sem `"x left"` (estão todos a sair); `EVENT` e não `ERROR` porque não houve pedido |
| 41 | 2.º `Ctrl+C` durante o encerramento | o processo morre logo | `SIG_DFL` reposto (`main.c`) |
| 42 | `kill -9` ao servidor | o nosso cliente: `o servidor fechou a ligacao` | o kernel fecha os sockets |
| 43 | arrancar **logo** a seguir (depois de 40 ou 42) | arranca | `SO_REUSEADDR` (tp §4.1) |
| 44 | dois servidores na **mesma porta TCP** | o 2.º: `ERROR listen na porta 5000: Address already in use`, sai com 1 | o TCP não deixa (o `SO_REUSEADDR` não chega para isso) |
| 45 | `./bin/client ::1 5000` (IPv6) | liga | socket **dual-stack** (D21, slide 85). *Não testado nesta sessão (sem IPv6); testar no portátil* |

### 7.7 O nosso cliente contra servidores estranhos

| # | O servidor… | O nosso cliente | Porquê |
|---|---|---|---|
| 46 | não responde ao `NICK` | `sem resposta do servidor em 5 s`, sai com 1 | `REPLY_TIMEOUT_S` |
| 47 | não existe (porta fechada) | `não foi possível ligar a <ip>:<porta>`, sai com 1 | — |
| 48 | manda uma linha com mais de 64 KiB | descarta-a (`descartada`) e continua | tolerante a receber (D1/D2 revistas, no cliente) |
| 49 | manda texto acima de 4096 bytes, ou um nick até 128 bytes | mostra (o texto cortado com `[...]`) | tolerante a receber (D20, R5) |
| 50 | manda `EVENT`s antes do `OK` | mostra-os e continua à espera do `OK` | `do_nick()` |
| 51 | congela (processo parado) durante a conversa | sai ao fim de ~240 s: `o servidor nao responde` | keepalive (D25); com `-k 2`, medido: 3 s |

### 7.8 Descoberta

| # | Experiência | O que se vê | Porquê |
|---|---|---|---|
| 52 | `./bin/client` com um servidor na rede | liga-se logo, sem perguntar | D31 |
| 53 | com dois | lista ordenada por nome, IP e porta, e pede um número | D31; a mesma ordem de uma procura para a outra (P56) |
| 54 | com nenhum | a lista das causas prováveis, com o comando do `ufw` para **os dois lados**; sai com 1 | D31; a firewall do cliente também bloqueia (M5.md, nota 13) |
| 55 | firewall a bloquear a 5001/udp no servidor | nenhum servidor; `./bin/client <ip>` continua a funcionar | o UDP falha **em silêncio** (P61) |
| 56 | dois servidores **na mesma máquina e na mesma porta UDP** | com broadcast aparecem os dois; com `-b <ip>` (unicast) **só um** | `SO_REUSEADDR` em UDP: o broadcast é entregue a todos os sockets, um datagrama unicast só a um (M5.md, nota 8) |
| 57 | porta UDP ocupada por outro programa | `WARN descoberta UDP desligada`, o chat arranca na mesma | D33 |
| 58 | lixo na porta UDP | silêncio, o chat continua | só se responde a um `DISCOVER` válido (D28) |
| 59 | `-s` sem host | recusado: `indica o host e a porta TLS` | a descoberta só anuncia a porta em claro (D-TLS-5) |

### 7.9 TLS (M6)

| # | Experiência | O que se vê | Porquê |
|---|---|---|---|
| 60 | `-f` certo | liga; conversa com quem está em claro | duas portas, o mesmo chat (D-TLS-1) |
| 61 | `-f` errado | recusa **antes** de o `NICK` sair | pinning do SHA-256 (D-TLS-2) |
| 62 | sem `-f` | liga, com `AVISO: certificado NAO verificado` e o fingerprint | cifra sem autenticação; um MITM seria aceite (§5.1) |
| 63 | `nc` na porta TLS | recusado (`wrong version number`); o servidor continua | o handshake falha; é feito na thread do cliente, com prazo (M6.md §3.3) |
| 64 | `-s` na porta em claro | erro do TLS, não um bloqueio | prazo no handshake |
| 65 | cliente TLS que não lê | desligado aos 5 s, como em claro | socket não-bloqueante depois do handshake (M6.md, bug 4) |
| 66 | servidor sem `certs/` | `TLS desligado`, com a dica do `gen-cert.sh`; o claro arranca | o TLS é uma extensão: não deita abaixo o chat que interopera |

### 7.10 Coisas que podem surpreender (saber antes que aconteçam)

| # | O quê | Porquê | Se perguntarem como corrigir |
|---|---|---|---|
| 67 | `Ctrl+D` no cliente **com o servidor congelado**: o cliente fica à espera; um `Ctrl+C` não chega, o 2.º mata | depois do `QUIT` o cliente espera pelo fecho do servidor sem prazo, e o keepalive já parou | um prazo no fecho (como o `linger_drain` do servidor) |
| 68 | o nosso cliente sai com **0** quando é o servidor a fechar (408, `Ctrl+C`, `kill -9`) | só o erro de arranque (sem ligação, 409, 503, sem resposta) dá 1 | distinguir "saí eu" de "fecharam-me" no código de saída |
| 69 | escrever `/quit` no prompt do nick dá um utilizador chamado `/quit` | o `/` é um carácter válido num nick (D20) | proibir `/` no início do nick |
| 70 | o mesmo servidor pode responder diferente conforme a versão do cJSON (números enormes) | a 1.7.19 aceita números com mais de 63 caracteres; a 1.7.17 não | o RFC deixa os limites a cada implementação (M6.md §7) |

---

## 8. As partes fáceis e as difíceis

O professor vai discutir "as partes fáceis e as difíceis". As nossas:

- **Fácil:** o protocolo JSON por linha; o servidor de um só cliente; a
  descoberta UDP (um datagrama chega inteiro ou não chega).
- **Difícil:**
  - o TCP é um *byte stream* — mensagens coladas e partidas (M1; §7.4);
  - a concorrência: enviar fora do lock, mas com prazo, e o *check-then-act*
    dos nicks (M2, M3; §7.5);
  - as mortes silenciosas, e fechar bem: FIN e não RST (M4; §7.2);
  - pôr TLS numa aplicação já feita: o `SSL*` não serve os dois sentidos ao
    mesmo tempo, e o cliente passou a precisar de dois locks (M6);
  - a interoperabilidade: tudo o que o slide não diz tem de ser combinado.

---

## 9. Quando algo corre mal

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

## 10. Registo da demo (preencher depois)

- Rede usada:
- Grupos com quem interligámos:
- O que falhou e porquê:
- O que o professor comentou:
