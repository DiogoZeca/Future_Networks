# Problemas típicos

Catálogo dos erros que aparecem sempre neste tipo de projeto (com base nos do professor — slide 125 — mais alguns encontrados a testar o M0, o M1, o M2 e o M3).

Cada entrada tem um **Estado**. Os resolvidos estão explicados em detalhe
(evidência → solução → prova) em
[`milestones/M1.md` — "Problemas do M0 resolvidos no M1"](milestones/M1.md#problemas-do-m0-resolvidos-no-m1) e
[`milestones/M2.md` — "Problemas do M1 resolvidos no M2"](milestones/M2.md#problemas-do-m1-resolvidos-no-m2) e
[`milestones/M3.md` — "Problemas resolvidos no M3"](milestones/M3.md#problemas-resolvidos-no-m3) e
[`milestones/M5.md` — "Problemas resolvidos no M5"](milestones/M5.md#problemas-resolvidos-no-m5) (descoberta UDP).

---

## 0. Evidência medida no nosso M0

Não são hipóteses - foram observadas nesta máquina em 2026-10-12, com
`scripts/probe_m0.py` contra `bin/server`:

| Experiência                               | Resultado observado                                      |
|-------------------------------------------|----------------------------------------------------------|
| `send()` único de 10 001 bytes            | Chegou ao servidor em **3 `recv()`: 4096 + 4096 + 1809** |
| 2.º cliente enquanto o 1.º está ligado    | TCP liga (handshake completo) mas **não é servido** - fica na fila do `listen()` |
| Cliente fecha com dados por ler no buffer | Kernel pode enviar **RST em vez de FIN** → `ECONNRESET` no `recv()`. **Depende do timing**: em execuções repetidas vimos as duas coisas |
| Cliente com `SO_LINGER{1,0}` a fechar     | Servidor apanha `ECONNRESET` **no `send()`** (determinístico) |
| `shutdown(SHUT_WR)` (half-close)          | Servidor vê `recv()==0`, fecha, cliente vê `recv()==0` - Já resolvido |
| Reiniciar servidor na mesma porta         | Sem erro - `SO_REUSEADDR` está a funcionar - Já resolvido |

## 0.1 Evidência medida no M1

Observada em 2026-10-13, com o servidor do M1 compilado com `SAN=1`
(detalhe em `milestones/M1.md`, "Verificação pela rede"):

| Experiência | Resultado observado |
|---|---|
| Cenários de `tests/interop/` em 3 modos (linha / colado / byte a byte) | 18 ok - mensagens partidas e coladas tratadas com sockets reais |
| 4 linhas num só `printf` para o `nc` | chegaram num só `recv()`; as 4 respostas certas |
| 4096 bytes sem `\n` | `ERROR 400 "line too long"` + FIN |
| 5000 bytes + `\n` | `ERROR 400 "line too long"` + **RST** - o servidor fechou com bytes por ler (ver 2.2) |
| Meia mensagem + RST / + FIN | servidor sobrevive; o cliente seguinte é servido |
| Linha com `'\0'` embebido | `ERROR 400 "invalid JSON"` |
| Relatórios do ASan/UBSan | zero |

## 0.2 Evidência medida no M2

Observada em 2026-10-13 (detalhe em `milestones/M2.md`, P12 e P18):

| Experiência | Primeira versão do M2 | Depois da revisão |
|---|---|---|
| Um cliente que nunca lê + 3000 mensagens de 1 KB | ao fim de ~2,6 MB **o chat inteiro congela, para sempre** | o lento é desligado ao fim de 5 s; o chat continua |
| 2000 entradas com um cliente a falar sem parar | **26** viram um `CHAT` antes do `OK` ao seu `NICK` | 0 |
| O mesmo com ThreadSanitizer (300 entradas) | **61** | 0 |
| `probe_m2.py` inteiro com ThreadSanitizer | — | zero data races |

## 0.3 Evidência medida no M3

Observada em 2026-10-14 (detalhe em `milestones/M3.md`, P20–P22 e P28–P29):

| Experiência | Com o bug (mutação) | Versão final |
|---|---|---|
| 16 threads pedem o mesmo nick no mesmo instante, 200 rondas | verificar e reservar em 2 locks: **36** rondas acabam com mais do que uma "ana" | 0 |
| 50 rondas de 8 clientes reais com o mesmo `NICK` | — | sempre 1 `OK` e 7 × `409` |
| 1500 `PRIV` para um cliente que entra e sai sem parar | `find` sem refcount: ASan **heap-use-after-free**, o servidor cai | só `OK` ou 404; servidor vivo |
| `USERS` com 64 nicks de 32 bytes | com `"` nos nicks seriam 4315 bytes (> 4096) | 2266 bytes + `\n` |
| `probe_m2.py` + `probe_m3.py` com ThreadSanitizer | — | zero avisos |
| `interop.py` contra o mesmo servidor em `127.0.0.1` e em `::1` | antes do M3: só IPv4 | 42/42 nas duas famílias |
| `interop.py` 12 vezes seguidas (todos os cenários usam o nick "ana") | **2 execuções** com um `409` falso: o cenário seguinte pedia "ana" antes de o servidor dar pela saída do anterior | fechar com FIN e esperar pelo servidor: 15/15 |
| Linha de 64 KiB + 1 byte | — | `ERROR 413` e fecho; de vez em quando o RST engole o 413 (2.2, M4) |
| `probe_m2.py` §7 numa máquina mais lenta (VM) | espera fixa de 0,5 s: falha sem bug no servidor (também com o código do M2) | espera até a mensagem chegar: passa |

## 0.4 Evidência medida no M4

Observada em 2026-10-18 (detalhe em `milestones/M4.md`, P38–P45):

| Experiência | Antes | Depois do M4 |
|---|---|---|
| Linha de 70 KB (e mais 50 KB atrás), 20 vezes | de vez em quando o RST engolia o `413` | 20/20: `413` e FIN |
| `QUIT` com 200 KB de lixo atrás, 20 vezes | idem com o `OK` | 20/20: `OK` e FIN |
| Uma ligação que nunca diz nada | ficava para sempre | `408` ao fim de 30 s (sem `NICK`) / 300 s |
| 500 linhas de lixo de 8 espécies seguidas | uma com UTF-8 inválido era **aceite e reenviada a todos** (e rebentava o cliente Python do teste) | 500 × `400`; a ligação continua |
| Ctrl+C no servidor | morria; ninguém avisado; memória nunca libertada | aviso a todos, FIN, saída 0; LeakSanitizer limpo |
| Os 98 testes de sistema com ASan, e com TSan | — | zero relatórios |
| A 1.ª versão do Ctrl+C (`pselect`) sob o TSan | — | o sinal só era tratado na ligação seguinte; corrigido com um pipe (P44) |
| O cliente quando o servidor fecha, com o stdin num pipe aberto, no Ubuntu 24.04 (glibc 2.39) | **nunca terminava**: o `exit()` da thread de leitura esperava pelo lock do `stdin`, que o `fgets()` da principal tinha (no 22.04 passava) | `term_exit()`: `_exit()` depois de despejar à mão (P46) |

---
## 0.5 Evidência medida no M5

Observada em 2026-10-19, na loopback (detalhe em `milestones/M5.md`, P50–P61):

| Experiência | Resultado observado |
|---|---|
| `sendto()` para `127.255.255.255` sem `SO_BROADCAST` | `EACCES` — o kernel obriga a pedir o broadcast |
| `sendto()` para `255.255.255.255` numa máquina sem rede | `ENETUNREACH`; o `127.255.255.255` funciona (é a loopback) |
| Dois sockets com `SO_REUSEADDR` na mesma porta UDP | um **broadcast** chega aos **dois**; um **unicast** só a **um** (o último ligado) |
| Um 3.º socket sem `SO_REUSEADDR` nessa porta (ou ao contrário) | `EADDRINUSE` |
| Datagrama de 612 bytes lido com um buffer de 512 | o `recvfrom()` devolve 512 como se fosse tudo; só o `MSG_TRUNC` do `recvmsg()` o diz; o resto perde-se |
| 200 `DISCOVER` seguidos ao nosso servidor | 200 respostas; o chat continua a aceitar ligações |

## 1. TCP é um byte stream, não um stream de mensagens

### 1.1 `recv()` não devolve mensagens inteiras

- **Problema:** o parser recebe meio JSON e rebenta, ou lê duas mensagens coladas como se fossem uma.
- **Porquê:** TCP garante *ordem* e *integridade*, não *fronteiras*. O que o emissor escreveu em 1 `send()` pode chegar em N `recv()`, e N `send()` podem chegar em 1 `recv()` (coalescência / Nagle).
- **Como evitar:** a aplicação define o enquadramento. Opções: tamanho fixo, delimitador com escaping, ou prefixo de comprimento. **Nós usamos delimitador `\n`** (fixado pelo enunciado). Implica um buffer de receção persistente por ligação que acumula bytes e extrai só linhas completas.
- **Estado:** `Resolvido` - **M1**. Buffer por ligação (`struct rxbuf`, `src/common/framing.c`); um ciclo que extrai **todas** as linhas de cada `recv()`; o cliente vê o buffer antes de voltar ao `recv()`. Provas: testes #2, #3, #4, #24; `interop.py` modos `colado` e `byte-a-byte`. Ver M1.md, P1 e P2.

### 1.2 `send()` pode enviar só parte

- **Problema:** mensagens truncadas no outro lado, aparentemente ao acaso, só com carga alta ou mensagens grandes.
- **Porquê:** `send()` devolve o número de bytes aceites pelo buffer do kernel, que pode ser menor que o pedido.
- **Como evitar:** ciclo até enviar tudo ou dar erro.
- **Estado:** `Resolvido` - `send_all()` em `src/common/net.c`.

### 1.3 Armadilha latente: `send_all()` + timeout de envio

- **Sintoma:** a partir do M4, mensagens cortadas a meio chegam ao peer, que fecha a ligação por JSON inválido.
- **Porquê:** hoje `send_all()` devolve `-1` em qualquer erro que não seja `EINTR`. Enquanto os sockets forem bloqueantes puros isso é seguro. Quando o M4 puser
`SO_SNDTIMEO` (ou sockets não bloqueantes), `send()` passa a poder **aceitar parte dos bytes e só depois devolver `EAGAIN`/`EWOULDBLOCK`**. O código atual desiste *depois de já ter enviado meia mensagem* - o stream do peer fica corrompido e não há recuperação possível, porque o `\n` seguinte aparece no sítio errado.
- **Como evitar:** quando se introduzir o timeout, decidir explicitamente entre (a) tratar `EAGAIN` como erro fatal **e fechar a ligação** (nunca voltar a escrever nela), ou (b) fila de envio por cliente. Nunca devolver erro e continuar a usar o socket.
- **Estado:** `Resolvido` - **M2**, no mesmo commit em que entrou o timeout de envio, com a opção (a): `send_all_timeout()` devolve erro se o prazo expirar e `send_line_to()` marca o cliente `dead` e desliga-o com `shutdown()` — **nunca mais se escreve nesse socket**. Prova de que a meia mensagem existe mesmo: `test_net` #57. O cliente continua a usar o `send_all()` sem prazo (só fala com um servidor). Ver M2.md, P13 e nota 9.

### 1.4 Linha maior que o buffer

- **Sintoma:** buffer overflow, ou mensagens silenciosamente perdidas.
- **Porquê:** um peer malicioso ou com bug envia 1 MB sem nenhum `\n`. Um buffer de receção fixo enche e não há linha para extrair.
- **Como evitar:** limite explícito acordado na spec e, ao ser excedido, responder com um `ERROR` e fechar. **Nunca** truncar em silêncio e continuar a ler - os bytes restantes seriam interpretados como uma mensagem nova (no servidor; no cliente, que lê de um servidor que manda linhas completas, descartar até ao `\n` seguinte é seguro).
- **Estado:** `Resolvido` - **M1**: `RX_CAP = RX_MAX_LINE + RX_CHUNK`, garantido por `_Static_assert`; acima do limite, erro e fecho, sem ressincronizar (D1/D2). Provas: #6 (com ASan), #7, #21, #22, #23. Ver M1.md, P3. **Revisão do M3**: limite **64 KiB** (era 4096), acordado com o primeiro grupo; o erro passou a **`413`** (era 400); o cliente tem um modo tolerante que descarta a linha e continua (`test_framing` #78–#80).

---

## 2. As quatro maneiras de uma ligação morrer

Este é o grupo que responde à pergunta do professor *"e se um computador se
desligar?"*. São quatro cenários distintos, com sintomas diferentes.

### 2.1 Fecho ordeiro (FIN) - `close()` ou `shutdown(SHUT_WR)`

- **Sintoma:** `recv()` devolve **0**.
- **Porquê:** o peer acabou de enviar. 0 significa fim de stream, não erro.
- **Como evitar o bug:** testar `n == 0` explicitamente. Sem isso o servidor fica em ciclo infinito a processar um socket morto e gasta 100% de CPU.
- **Estado:** `Resolvido`

### 2.2 Fecho abrupto (RST) - processo morto, `Ctrl+C`, ou fecho com dados por ler

- **Sintoma:** `recv()` ou `send()` devolvem `-1` com `ECONNRESET`; ou `send()` dá `EPIPE`; sem `MSG_NOSIGNAL` o processo **morre com `SIGPIPE`** sem mensagem nenhuma.
- **Porquê:** não houve FIN. Nota contraintuitiva que observámos: **fechar um socket que ainda tem dados por ler no buffer de receção pode gerar RST em vez de FIN** - ou seja, um cliente "bem comportado" que faz `close()` sem ler as respostas pendentes pode aparecer ao servidor como uma ligação abortada. Em execuções repetidas do `probe_m0.py` vimos as duas coisas, consoante o timing: **o servidor tem de aguentar ambas**, e não pode assumir que um cliente educado produz sempre `recv()==0`.
- **Como evitar:** `MSG_NOSIGNAL` no `send()` (ou ignorar `SIGPIPE` no arranque), e tratar `ECONNRESET`/`EPIPE` como "cliente foi-se embora" - não como erro fatal do servidor.
- **Estado:** `Resolvido` - `MSG_NOSIGNAL` em `send_all()`; verificado experimentalmente nos dois sentidos.
- **Visto de novo no M1, do nosso lado:** ao recusar uma linha demasiado longa, o servidor fecha com bytes do cliente **ainda por ler** - e o kernel manda RST. Se o RST chegar antes de o cliente ler o `ERROR "line too long"`, o cliente nunca o vê. Resolver exige continuar a ler (e deitar fora) durante algum tempo antes de fechar, o que precisa de timeouts - **M4**.
- **Resolvido no M4:** `linger_drain()` sempre que é o servidor a fechar (413, `QUIT`, 408, 503, encerramento): `shutdown(SHUT_WR)` (o FIN sai depois da última resposta) e ler e deitar fora até o cliente fechar, no máximo 2 s. Prova: `test_robustness.py`, 413 e `QUIT`+lixo 20 vezes cada, sempre a resposta e FIN; mutação 8 do M4.md (sem o linger, falha logo na 1.ª ronda).
- **Voltou no M6, por outra porta:** o `MSG_NOSIGNAL` protege os **nossos** `send()`. O OpenSSL escreve com `write()`, que não tem essa flag - e um cliente que recusa o fingerprint e fecha matava o servidor com `SIGPIPE` (exit 141), apesar de todo o cuidado que já lá estava. **A defesa antiga não cobre escritas que não são nossas.** Resolvido com `signal(SIGPIPE, SIG_IGN)` no arranque do servidor e do cliente: o `write()` passa a devolver `EPIPE`, que o código já sabia tratar.

### 2.3 Morte silenciosa - cabo desligado, Wi-Fi caído, máquina sem energia

- **Sintoma:** **nenhum**. O servidor fica bloqueado no `recv()` daquele cliente *indefinidamente*. O cliente continua na lista de utilizadores, aparece no `/list`, e os broadcasts para ele acumulam-se no buffer de envio até encher.
- **Porquê:** não há FIN nem RST porque a máquina não teve oportunidade de os enviar. Do ponto de vista do TCP local a ligação continua perfeitamente válida. O keepalive do TCP só atua ao fim de **~2 horas** por omissão no Linux (`net.ipv4.tcp_keepalive_time = 7200`).
- **Como evitar:** é o cenário que o enunciado quer ver tratado e **não se resolve com `recv()`**. Precisa de um dos dois:
  - *idle timeout* ao nível da aplicação (`SO_RCVTIMEO`, ou `poll()` com timeout): sem nada recebido em N segundos, assume-se morto e fecha-se; **e/ou**
  - *heartbeat*: `PING`/`PONG` periódico - mais fiável, porque distingue "ligação morta" de "utilizador calado". Como `PING` não está na lista fixa do slide 124, tem de ser negociado com os outros grupos ou implementado de forma compatível (ex.: o servidor usa um `EVENT` inócuo, ou mede só a inatividade).
  - `SO_KEEPALIVE` com `TCP_KEEPIDLE`/`TCP_KEEPINTVL`/`TCP_KEEPCNT` é a alternativa ao nível do kernel; funciona, mas as opções são específicas de Linux.
- **Estado:** `Resolvido` - **M4** (D23): 300 s sem nenhuma **mensagem** → `ERROR 408 "idle timeout"` e fecho; antes do `NICK`, 30 s desde a ligação. Do lado do cliente, o nosso manda um `LIST` silencioso a cada 120 s sem enviar nada (não é desligado por estar só a ler) e, se esse `LIST` ficar sem resposta mais 120 s, conclui que o **servidor** morreu em silêncio e sai. Provas: `test_robustness.py` (prazos), `test_client.py` (keepalive, servidor mudo). *Antes do M4:* desde o M2, **se houver tráfego** para esse cliente, o prazo de envio (2.4) apanha-o: os broadcasts acumulam-se, o buffer enche, o envio expira ao fim de 5 s e ele é desligado (com `EVENT "x left"`). **Sem tráfego** continua invisível até ao M4. **É o cenário mais provável de ser perguntado na defesa.** Vale a pena saber explicar porque é que é diferente de 2.1 e 2.2.

### 2.4 Cliente lento ou parado - não morreu, só não lê

- **Sintoma:** **todos** os outros utilizadores ficam bloqueados. O chat congela.
- **Porquê:** o servidor faz broadcast com `send()` bloqueante. Se um cliente não lê, a janela TCP dele fecha, o buffer de envio do servidor enche e o `send()` bloqueia - com o lock da lista de clientes na mão, no pior caso. Um único cliente parado (ou um `Ctrl+S` no terminal, ou um portátil suspenso) para o servidor inteiro.
- **Como evitar:** `SO_SNDTIMEO` + desligar quem estourar o timeout, ou fila de envio por cliente com thread de escrita dedicada. **E nunca fazer `send()` com um mutex global agarrado** (ver 3.2).
- **Estado:** `Resolvido` - **M2** (D11): prazo **total** de 5 s por envio (`send_all_timeout()`, `poll()` + `MSG_DONTWAIT`); quem o esgota é marcado `dead` e desligado com `shutdown()`, e mais ninguém espera por ele. Provas: `test_net` #54, #55; `probe_m2.py` §7. **Atenção:** *não* é fácil de demonstrar à mão — com `Ctrl+Z` e poucas mensagens os buffers do kernel (centenas de KB a MB) absorvem tudo e não se vê nada. Medimos o congelamento só ao fim de ~2,6 MB (secção 0.2). A primeira versão do M2 enviava fora do lock mas sem prazo, e congelava na mesma — enviar fora do lock **não chega** (ver 3.2). Alternativa não implementada: fila de envio por cliente (M2.md, nota 7).

---

## 3. Concorrência (a partir do M2)

### 3.1 Lista de clientes alterada durante o broadcast

- **Sintoma:** crashes esporádicos, só com vários clientes, impossíveis de reproduzir.
- **Porquê:** uma thread percorre a lista enquanto outra remove um cliente que saiu. Ponteiro pendente.
- **Como evitar:** mutex na lista; **copiar a lista dentro do lock e enviar fora dele**.
- **Estado:** `Resolvido` - **M2**: `roster_snapshot()` copia dentro do lock; teste #4 (determinista) e #12 (5 threads a tirar retratos contra 5 a entrar/sair).

### 3.2 Enviar com o lock agarrado

- **Sintoma:** o chat fica lento ou bloqueia completamente quando um cliente é lento.
- **Porquê:** combina 2.4 com 3.1 - um `send()` que bloqueia dentro da secção crítica transforma um cliente lento num deadlock global.
- **Como evitar:** o lock protege a *estrutura de dados*, nunca a *E/S*. Copiar e sair.
- **Estado:** `Resolvido` - **M2**: `deliver()` envia fora do lock; o lock do roster nunca e segurado durante um `send()`. **Mas isto sozinho não resolve o 2.4:** o lock do roster fica livre, só que a thread que envia para o cliente lento continua presa (e com o `wlock` dele), e num broadcast todas as threads lhe escrevem. É preciso também o prazo de envio (2.4).

### 3.3 Mensagens de vários emissores intercaladas no mesmo socket

- **Sintoma:** o cliente recebe JSON corrompido: metade de uma mensagem, metade de outra.
- **Porquê:** duas threads a fazer `send_all()` no mesmo fd ao mesmo tempo. Cada `send()` é atómico, mas o ciclo do `send_all()` não é.
- **Como evitar:** um mutex de escrita **por cliente** (já previsto no `M2.md`).
- **Estado:** `Resolvido` - **M2**: `wlock` por cliente em `send_line_to()`. **Nota honesta:** nao esta *demonstrado* - o TSan nao ve esta corrida (e logica, no socket, nao de memoria) e com `SO_SNDBUF` de 16 KB e linhas de 4 KB o `send_all()` quase nunca faz mais de uma iteracao. E protecao para o buffer cheio (cliente que nao le), que desde a revisao do M2 tambem tem prazo (2.4).

### 3.4 Use-after-free na saída de um cliente

- **Sintoma:** crash ou corrupção quando alguém sai durante um broadcast.
- **Porquê:** a thread do cliente liberta a sua estrutura enquanto outra ainda a tem na cópia da lista.
- **Como evitar:** contagem de referências, ou remover da lista e só libertar depois de garantir que mais ninguém tem a referência. Em C isto é trabalho a sério - decidir a estratégia **antes** de escrever o M2, não durante.
- **Estado:** `Resolvido` - **M2**: contagem de referencias no `roster` (`roster_snapshot` incrementa dentro do lock, `client_release` larga). Teste #4 e #12 com ASan; servidor sob ThreadSanitizer com o `probe_m2.py` completo: zero races. Quando é outra thread a detetar que um cliente morreu, faz `shutdown()` e **não** `close()` — o `close()` é sempre do refcount (M2.md, nota 10).

### 3.5 A resposta a um pedido ultrapassada por um broadcast

- **Sintoma:** um cliente envia `NICK` e a primeira linha que recebe é um `CHAT` de outra pessoa, não o `OK`. Raro (~1%), só com tráfego, e só aparece com muitas ligações.
- **Porquê:** o cliente entrava na lista do chat (`roster_join()`) **antes** de o `OK` ser enviado. Nessa janela, a thread de outro cliente tirava um retrato que já o incluía e enviava-lhe o `CHAT` primeiro. Os dois envios são serializados pelo `wlock`, mas por ordem de chegada, não pela ordem "lógica".
- **Como evitar:** quando a ordem importa, só tornar o cliente visível aos outros **depois** de lhe ter enviado a resposta. E, do lado do cliente, não assumir que a primeira linha é a resposta: esperar pelo tipo certo.
- **Estado:** `Resolvido` - **M2** (revisão, D9): `roster_join()` depois do `OK`; o cliente espera por `OK`/`ERROR`. Prova: `probe_m2.py` §6 (1000 entradas, 0 falhas; antes 26/2000). O ThreadSanitizer **não** o apanha: não é uma data race.

### 3.6 Verificar e depois agir (*check-then-act*): nicks duplicados

- **Sintoma:** dois clientes com o mesmo nick, mas só quando entram **ao mesmo tempo** (raro à mão, frequente com scripts ou no dia da interop com vários grupos a ligar).
- **Porquê:** "o nick está livre?" e "guardar o nick" são duas operações. Cada uma com o lock está correta; entre as duas, outra thread faz a mesma pergunta e recebe a mesma resposta. Proteger cada acesso não protege uma decisão que depende de dois.
- **Como evitar:** a decisão e a escrita no **mesmo** lock (uma só função). E contar como "em uso" quem já reservou mas ainda não entrou no chat.
- **Estado:** `Resolvido` - **M3**: `roster_claim()`. Prova: `test_roster` #18 (16 threads × 200 rondas; a mutação com dois locks falha 36/200), `probe_m3.py` §1.


### 3.7 Uma protecção posta "por via das dúvidas" pode ser pior (M6)

- **Sintoma:** o cliente deixou de sair. Imprimia tudo, e ficava preso para sempre no fim.
- **Porquê:** ao pôr TLS, o lock da ligação passou a cobrir a **leitura** além da escrita - porque um `SSL*` não pode ser usado por duas threads ao mesmo tempo. Só que o lock foi posto **sempre**, também em claro. E em claro o `io_recv()` é bloqueante: a thread de leitura segurava o lock à espera de bytes, enquanto a thread principal esperava o mesmo lock para mandar o `QUIT` - que era exactamente o que faria os bytes chegarem. Deadlock.
- **Como evitar:** o lock só onde é preciso - **com TLS**. Em TCP os dois sentidos são independentes e o kernel trata deles em separado; foi por isso que o cliente do M2 não precisava de lock nenhum.
- **A lição:** um lock a mais não é conservadorismo barato. Transformou um caminho que funcionava desde o M2 num deadlock, e só apareceu no fecho - o sítio onde menos se olha.
- **Estado:** `Resolvido` - **M6**: `int locked = io_is_tls(io);` nos dois lados.

---

## 4. Rede e ambiente

### 4.1 "Address already in use" ao reiniciar

- **Porquê:** a ligação anterior está em `TIME_WAIT` (2×MSL).
- **Como evitar:** `SO_REUSEADDR` antes do `bind()`.
- **Estado:** `Resolvido` - verificado - reinício imediato funciona.

### 4.2 Firewall bloqueia o servidor ou o UDP de descoberta

- **Sintoma:** funciona em `localhost`, falha entre máquinas. O broadcast UDP é o que falha mais.
- **Como evitar:** testar cedo entre duas máquinas reais, não só em loopback. No dia da interoperabilidade, ter à mão o comando para abrir a porta e saber qual é a firewall da máquina (`ufw`, `firewalld`, Windows Defender).
- **E a firewall do cliente:** a resposta a um `DISCOVER` vem do IP do servidor, não do endereço de broadcast para onde foi o pedido — o *connection tracking* não a reconhece como resposta, e uma firewall que recusa o que entra deita-a fora. Com a `ufw`: no servidor `sudo ufw allow 5001/udp`; no cliente `sudo ufw allow proto udp from any port 5001 to any` (`milestones/M5.md`, nota 13).
- **Estado:** `Resolvido do nosso lado` - **M5**: quando ninguém responde, o cliente diz as causas prováveis com os dois comandos do `ufw` e sugere `./bin/client <ip>`; `scripts/discover.py` mostra o que chega em bruto (separa "a rede não deixa" de "o formato é outro"). **Por fazer:** o teste entre duas máquinas com as firewalls ligadas (`milestones/M5.md`, "Teste numa rede a sério", passos 6 e 6b). **Testar antes do dia**, não no dia.

### 4.3 IPv4 vs IPv6 - assimetria no nosso código

- **Sintoma:** o cliente de outro grupo não se liga ao nosso servidor, apesar de o nosso cliente se ligar bem.
- **Porquê:** `tcp_listen()` usa `AF_INET` (confirmado: escuta em `0.0.0.0:porta`, só IPv4) mas `tcp_connect()` usa `AF_UNSPEC`. O nosso cliente aguenta porque percorre todos os endereços devolvidos pelo `getaddrinfo()` e cai no IPv4 - testámos com `localhost` e funciona. Um cliente de outro grupo que resolva um AAAA e **não** faça esse fallback liga-se ao nada.
- **Como evitar:** uma de duas, mas decidir explicitamente:
  - declarar **"IPv4 apenas"** no `PROTOCOL.md` (mais simples, e suficiente para uma LAN); ou
  - escutar em `AF_INET6` com `IPV6_V6ONLY=0` (dual-stack). Atenção: no Windows o `IPV6_V6ONLY` vem a **1** por omissão, ao contrário de muitos Linux - se um grupo fizer isto em Windows sem desligar a opção, perde os clientes IPv4.
- **Estado:** `Resolvido` - **M3**: dual-stack, como o slide 85 recomenda (*"Use getaddrinfo() and sockaddr_storage for IPv4 / IPv6 portability"*). O `tcp_listen()` tenta primeiro `AF_INET6` com `IPV6_V6ONLY=0` posto **explicitamente** (o valor por omissão varia entre sistemas) e, numa máquina sem IPv6, cai para `AF_INET`. Os clientes IPv4 aparecem nos logs como IPv4 (`addr_to_str()` desembrulha o `::ffff:a.b.c.d`). Prova: `test_net` #76–#77; `interop.py` 36/36 em `127.0.0.1` **e** em `::1` contra o mesmo servidor. Deixou de ser uma decisão a acordar (`DECISIONS.md`, S14).

### 4.4 NAT e endereços privados

- **Sintoma:** o IP que o servidor anuncia no `DISCOVER` não é alcançável.
- **Porquê:** máquina com várias interfaces (Wi-Fi + Ethernet + docker0 + VPN), ou atrás de NAT. O "meu IP" não é uma coisa só.
- **Como evitar:** no M5, responder ao broadcast a partir do endereço **da interface onde o pedido chegou**, não de um IP escolhido a priori. Melhor ainda: o cliente usa o IP de origem do pacote UDP de resposta e ignora qualquer IP que venha no payload.
- **Estado:** `Resolvido` - **M5** (P50): a resposta não leva IP nenhum; o cliente usa o **IP de origem** do datagrama de resposta (chegou até ele, logo é alcançável) e ignora um `ip`/`host` que venha na mensagem. E o pedido vai também para o broadcast de cada interface, não só para o `255.255.255.255` (P51). Prova: `test_discovery.py` (um `"ip":"10.9.9.9"` falso é ignorado); mutação 38.

---

## 5. Portabilidade (Windows & Linux)

Relevante porque os outros grupos não usam todos Linux.

| Problema | Nota |
|---|---|
| `openssl` para certificados | Não existe por omissão no Windows (afeta M6-B) |
| Abrir ficheiros em modo texto | Corrompe dados binários no Windows (`\n` → `\r\n`) - afeta M6-A |
| Wireshark em loopback | Precisa do Npcap com adaptador de loopback instalado |
| Terminação de linha | O nosso parser **tem** de aceitar `\r\n`, já está na spec |
| Codificação UTF-8 | A consola do Windows pode não estar em UTF-8 (`chcp 65001`) |
| `recv`/`send` | No Winsock devolvem `SOCKET_ERROR`, não `-1` com `errno`; `closesocket()` em vez de `close()` |
| `MSG_NOSIGNAL` | Não existe no Windows (mas também não há `SIGPIPE`) |

---

## 6. JSON e UTF-8

### 6.1 `\n` dentro de uma mensagem parte o enquadramento

- **Sintoma:** o peer recebe duas "mensagens", ambas inválidas.
- **Porquê:** JSON com pretty-printing, ou texto do utilizador com newline literal.
- **Como evitar:** `cJSON_PrintUnformatted()` (nunca `cJSON_Print()`). O cJSON faz escape de `\n` dentro de strings para `\\n`, o que é seguro. Verificar na mesma antes de enviar.
- **Estado:** `Resolvido` - **M1**. `cJSON_PrintUnformatted()`; caracteres de controlo recusados em `nick`/`text` (sem mensagens multi-linha), o que limita o crescimento ao codificar a 2× e garante, por `_Static_assert`, que a maior mensagem (2164 bytes) cabe numa linha. Provas: #18, #27, #31. Ver M1.md, P8.

### 6.2 UTF-8 cortado entre dois `recv()`

- **Sintoma:** acentos corrompidos ("Ã©") quando a mensagem chega partida.
- **Porquê (e boa notícia):** **partir em `\n` é sempre seguro em UTF-8.** Todos os bytes de continuação de uma sequência multibyte são ≥ 0x80, por isso 0x0A nunca aparece *dentro* de um caracter. Partir o *buffer* a meio de um caracter também não é problema **desde que nunca se interprete o buffer como texto antes de ter a linha completa**. O bug aparece quando se faz `printf("%s")` ou validação de UTF-8 sobre um buffer parcial.
- **Como evitar:** tratar os bytes como opacos até haver `\n`. Só então passar ao JSON.
- **Estado:** `Resolvido` - **M1** (decisão D3: bytes opacos até haver linha completa; nenhum `printf` sobre o buffer). Provas: #9; `interop.py` cenário 06 em modo `byte-a-byte`. Ver M1.md, P7.
 
### 6.3 JSON inválido ou campos em falta

- **Sintoma:** segfault no `cJSON_GetObjectItem(...)->valuestring`.
- **Porquê:** o cJSON devolve `NULL` para campos ausentes e o tipo pode não ser o esperado (`{"type":"MSG","text":123}`).
- **Como evitar:** validar sempre: item não-`NULL` **e** `cJSON_IsString()`. Um peer de outro grupo vai enviar algo inesperado - é garantido.
- **Estado:** `Resolvido` - **M1**. Três verificações por campo (existe, tipo certo, limites) antes de tocar em `valuestring`; `ERROR 400` com motivo, sem fechar a ligação. Também recusados: lixo depois do objeto (`{…}{…}`) e `'\0'` embebido. Provas: #12–#16, #25–#30. Ver M1.md, P5 e P6.

---

## 7. Memória e recursos (específico de C)

| Problema | Como evitar | Estado |
|---|---|---|
| Fugas do cJSON | Cada `cJSON_Parse()` precisa de `cJSON_Delete()`; cada `cJSON_PrintUnformatted()` devolve memória que é preciso libertar | `Resolvido` - **M1**: uma única saída por função; mutações apanhadas pelo LeakSanitizer (M1.md, P9) |
| Fuga de descritores | Falhar um `close(fd)` num caminho de erro esgota o limite de fds; o servidor deixa de aceitar ligações ao fim de ~1024 | `Parcial Resolvido` - rever cada `goto`/`break` |
| Buffer na stack por thread | Com N clientes, N × buffer. Escolher tamanhos conscientemente | `Resolvido` - **M2**: ~23 KB por thread do servidor (`rxbuf` 8 KB + `chunk` 4 KB + `line` 4 KB + saídas 2,5 KB + linha codificada 4 KB + retrato 0,5 KB), dentro de uma stack de 8 MB; com o máximo de 64 clientes, ~1,5 MB. Sem `malloc` por mensagem. **Revisão do M3** (linha de 64 KiB): `rxbuf` 68 KB + `line` 64 KB + linha codificada 64 KB + `struct msg` ~12,5 KB (várias): **~280 KB por thread** no pior caso, ~18 MB com 64 clientes — dentro dos 8 MB de cada thread. O buffer de nicks do `USERS` ficou em 8 KB (e não 64) de propósito; no cliente, os buffers de 64 KB estão em `static` |
| Strings sem terminador | `recv()` **não** põe `\0`. Nunca passar o buffer a `printf("%s")` sem limitar o tamanho | `Resolvido` - **M1**: comprimento sempre explícito (`out_len`), linhas com `'\0'` recusadas, `%.*s` nos logs (M1.md, P6) |

**Ferramenta:** `make SAN=1` (ASan + UBSan) já está no Makefile. Usar **sempre** que se testa concorrência, não só quando algo rebenta.

---

## 8. Interoperabilidade

| Problema | Como evitar |
|---|---|
| Cada grupo resolve a semântica ambígua de forma diferente | Fechar as propostas do `PROTOCOL.md` **com os outros grupos**, por escrito. A lista completa, com prioridades e o que parte se divergirem, está em [`DECISIONS.md`](DECISIONS.md) |
| Porta diferente por grupo | Acordar a porta por omissão (proposta: 5000) |
| O emissor de `MSG` recebe o próprio `CHAT`? | Se um grupo decide que sim e outro que não, as mensagens aparecem duplicadas ou em falta - e parece um bug de rede. **D5: sim** — acordado com o grupo 1 |
| `OK` a todos os pedidos ou só a alguns? | Um cliente que espere `OK` bloqueia contra um servidor que não o envia. **D13 (revisão do M3): `OK` a todo o pedido aceite**, sempre antes do `CHAT` — a leitura literal do slide 124, acordada com o grupo 1. O nosso cliente não espera pelo `OK` (só no `NICK`) e não o mostra |
| Campos desconhecidos | A spec já diz: **ignorar**. Isto permite extensões sem partir ninguém |
| A 1.ª linha depois do `NICK` é a resposta? | No nosso servidor **sim**, garantido (3.5). Mas o nosso cliente não o assume: espera por `OK`/`ERROR` e mostra o que vier antes, para aguentar servidores que mandem boas-vindas ou um `EVENT` primeiro |
| Servidor cheio | **Proposta nossa (D12):** `ERROR 503 "server full"` e fecho, em vez de um fecho sem explicação |
| Nicks com maiúsculas/acentos/espaços | **Proposta nossa (D4, D18, D20):** 1–32 bytes, sem espaços, `"`, `\` nem caracteres de controlo, UTF-8 aceite sem validar; **sem olhar a maiúsculas** (só ASCII) para a unicidade e para o destino do `PRIV` |
| O emissor de `PRIV` recebe cópia? | **D14 (revisão do M3): não**, só o `OK` — acordado com o grupo 1. Com cópia, o cliente deles mostrava-a como uma mensagem *para* o emissor. O nosso cliente mostra o `/msg` localmente |
| Limites diferentes entre grupos (linha, texto, nick em bytes ou em caracteres) | Acordar os que partem (linha 64 KiB / `413`; `text` 1–4096 com várias linhas / `400 "text too long"` — com o grupo 1) e ser **tolerante a receber** no resto: o nosso cliente aceita nicks até 128 bytes e textos de qualquer tamanho vindos de outro servidor, cortados e marcados `[...]` |
| Mensagens de várias linhas | O texto de outra pessoa pode trazer `\033` (apagar o ecrã, mudar cores) ou `\r` (escrever por cima). **Quem mostra é que limpa**: o nosso cliente mostra-os como `?` e indenta as linhas seguintes (`test_ui` #84) |
| `QUIT` tem `OK`? | **Proposta nossa (D17): sim**, e o servidor fecha; também antes do `NICK`. O nosso cliente aguenta um servidor que feche sem `OK` |
| `USERS` inclui quem pede? Que ordem? | **Proposta nossa (D16):** sim, alfabética. O `interop.py` compara a lista exata no cenário 10: contra outro grupo pode falhar só pela ordem |
| Um cenário de interop falha só porque o outro grupo manda `EVENT` a mais | O `interop.py` salta os `EVENT` quando espera outra coisa (M3) |
| Descoberta (M5): porta UDP, formato do pedido, `tcp_port` ou `port`, resposta só a quem pergunta, IP de origem, broadcast ou multicast | Se diferirem, os grupos simplesmente **não se encontram** — sem erro nenhum. Os pontos M5-1 a M5-11 estão no [`DECISIONS.md`](DECISIONS.md), secção M5. O nosso cliente já lê `tcp_port` e `port` e ignora um `ip`; o servidor aceita o pedido em JSON e em texto. `scripts/discover.py` mostra o formato de cada grupo |
| Testar só contra nós próprios | Os cenários em `tests/interop/` são texto puro e correm contra qualquer servidor com `scripts/interop.py` (3 modos de envio). `scripts/probe_client.py` mostra como o nosso cliente reage a servidores que decidiram diferente |

**Regra prática:** ser **estrito a enviar, tolerante a receber**. Nunca rebentar por causa do que o peer mandou - responder `ERROR` e continuar.

---

## 9. Checklist rápido antes de cada entrega

- [ ] `make` sem avisos
- [ ] `make SAN=1` e correr os testes - sem relatórios do ASan/UBSan
- [ ] Matar um cliente com `kill -9` → servidor sobrevive e remove-o
- [ ] `make pytest` (e `make clean && make pytest SAN=1`) - 0 falhas (M4+)
- [ ] Suspender um cliente com `Ctrl+Z` → os outros continuam a falar (M2+); para encher mesmo os buffers, `test_concurrency.py`
- [ ] `Ctrl+C` no servidor → os clientes veem `server shutting down`, sai com 0, sem "address already in use" a seguir (M4)
- [ ] Um `nc` calado → `408` ao fim de 30 s (sem `NICK`) ou 300 s (M4)
- [ ] Enviar lixo (`echo "nao e json" | nc host porta`) → `ERROR`, não crash
- [ ] Enviar uma linha de 1 MB sem `\n` → `ERROR`, não overflow
- [ ] Testar entre **duas máquinas**, não só em loopback — incluindo a descoberta (`./bin/client` sem endereço) com as firewalls ligadas (M5)
- [ ] Testar contra o servidor de outro grupo (e preencher o registo no `DECISIONS.md`)
- [ ] Dois clientes com o mesmo nick ao mesmo tempo → um `OK`, um `409` (M3+; `test_concurrency.py`)
- [ ] Notas do milestone preenchidas em `docs/milestones/`
