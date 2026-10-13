# Problemas típicos

Catálogo dos erros que aparecem sempre neste tipo de projeto (com base nos do professor mais alguns encontrados a testar o M0 e o M1).

Cada entrada tem um **Estado**. Os resolvidos no M1 estão explicados em detalhe
(evidência do M0 → solução → prova) em
[`milestones/M1.md` — "Problemas do M0 resolvidos no M1"](milestones/M1.md#problemas-do-m0-resolvidos-no-m1).

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

---

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
- **Estado:** `Parcialmente corrigido` - corrigir **no mesmo commit** em que entrar o timeout (**M4**). Nota: no M1 o cliente já usa `SO_RCVTIMEO` (timeout de **leitura**); esta armadilha é só do timeout de **envio**, que ainda não existe.

### 1.4 Linha maior que o buffer

- **Sintoma:** buffer overflow, ou mensagens silenciosamente perdidas.
- **Porquê:** um peer malicioso ou com bug envia 1 MB sem nenhum `\n`. Um buffer de receção fixo enche e não há linha para extrair.
- **Como evitar:** limite explícito acordado na spec (proposta: 4096 bytes incl. `\n`) e, ao ser excedido, responder `ERROR 400` e fechar. **Nunca** truncar em silêncio e continuar a ler - os bytes restantes seriam interpretados como uma mensagem nova.
- **Estado:** `Resolvido` - **M1**. `RX_CAP = RX_MAX_LINE + RX_CHUNK = 8192` (sobra até 4095 + `recv()` até 4096 = 8191), garantido por `_Static_assert`; linha > 4096 → `ERROR 400 "line too long"` e fecho, sem ressincronizar (D1/D2). Provas: #6 (com ASan), #7, #21, #22, #23. Ver M1.md, P3.

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

### 2.3 Morte silenciosa - cabo desligado, Wi-Fi caído, máquina sem energia

- **Sintoma:** **nenhum**. O servidor fica bloqueado no `recv()` daquele cliente *indefinidamente*. O cliente continua na lista de utilizadores, aparece no `/list`, e os broadcasts para ele acumulam-se no buffer de envio até encher.
- **Porquê:** não há FIN nem RST porque a máquina não teve oportunidade de os enviar. Do ponto de vista do TCP local a ligação continua perfeitamente válida. O keepalive do TCP só atua ao fim de **~2 horas** por omissão no Linux (`net.ipv4.tcp_keepalive_time = 7200`).
- **Como evitar:** é o cenário que o enunciado quer ver tratado e **não se resolve com `recv()`**. Precisa de um dos dois:
  - *idle timeout* ao nível da aplicação (`SO_RCVTIMEO`, ou `poll()` com timeout): sem nada recebido em N segundos, assume-se morto e fecha-se; **e/ou**
  - *heartbeat*: `PING`/`PONG` periódico - mais fiável, porque distingue "ligação morta" de "utilizador calado". Como `PING` não está na lista fixa do slide 124, tem de ser negociado com os outros grupos ou implementado de forma compatível (ex.: o servidor usa um `EVENT` inócuo, ou mede só a inatividade).
  - `SO_KEEPALIVE` com `TCP_KEEPIDLE`/`TCP_KEEPINTVL`/`TCP_KEEPCNT` é a alternativa ao nível do kernel; funciona, mas as opções são específicas de Linux.
- **Estado:** `Por resolver` - **M4** ("idle timeout"). **É o cenário mais provável de ser perguntado na defesa.** Vale a pena saber explicar porque é que é diferente de 2.1 e 2.2.

### 2.4 Cliente lento ou parado - não morreu, só não lê

- **Sintoma:** **todos** os outros utilizadores ficam bloqueados. O chat congela.
- **Porquê:** o servidor faz broadcast com `send()` bloqueante. Se um cliente não lê, a janela TCP dele fecha, o buffer de envio do servidor enche e o `send()` bloqueia - com o lock da lista de clientes na mão, no pior caso. Um único cliente parado (ou um `Ctrl+S` no terminal, ou um portátil suspenso) para o servidor inteiro.
- **Como evitar:** `SO_SNDTIMEO` + desligar quem estourar o timeout, ou fila de envio por cliente com thread de escrita dedicada. **E nunca fazer `send()` com um mutex global agarrado** (ver 3.2).
- **Estado:** `Por Resolver` - **M2/M4**. Fácil de demonstrar: basta suspender um cliente com `Ctrl+Z` e ver o resto do chat parar.

---

## 3. Concorrência (a partir do M2)

### 3.1 Lista de clientes alterada durante o broadcast

- **Sintoma:** crashes esporádicos, só com vários clientes, impossíveis de reproduzir.
- **Porquê:** uma thread percorre a lista enquanto outra remove um cliente que saiu. Ponteiro pendente.
- **Como evitar:** mutex na lista; **copiar a lista dentro do lock e enviar fora dele**.
- **Estado:** `Por Resolver` - **M2**.

### 3.2 Enviar com o lock agarrado

- **Sintoma:** o chat fica lento ou bloqueia completamente quando um cliente é lento.
- **Porquê:** combina 2.4 com 3.1 - um `send()` que bloqueia dentro da secção crítica transforma um cliente lento num deadlock global.
- **Como evitar:** o lock protege a *estrutura de dados*, nunca a *E/S*. Copiar e sair.
- **Estado:** `Por Resolver` - **M2**.

### 3.3 Mensagens de vários emissores intercaladas no mesmo socket

- **Sintoma:** o cliente recebe JSON corrompido: metade de uma mensagem, metade de outra.
- **Porquê:** duas threads a fazer `send_all()` no mesmo fd ao mesmo tempo. Cada `send()` é atómico, mas o ciclo do `send_all()` não é.
- **Como evitar:** um mutex de escrita **por cliente** (já previsto no `M2.md`).
- **Estado:** `Por Resolver` - **M2**.

### 3.4 Use-after-free na saída de um cliente

- **Sintoma:** crash ou corrupção quando alguém sai durante um broadcast.
- **Porquê:** a thread do cliente liberta a sua estrutura enquanto outra ainda a tem na cópia da lista.
- **Como evitar:** contagem de referências, ou remover da lista e só libertar depois de garantir que mais ninguém tem a referência. Em C isto é trabalho a sério - decidir a estratégia **antes** de escrever o M2, não durante.
- **Estado:** `Por Resolver` - **M2**. Testar com `make SAN=1`.

---

## 4. Rede e ambiente

### 4.1 "Address already in use" ao reiniciar

- **Porquê:** a ligação anterior está em `TIME_WAIT` (2×MSL).
- **Como evitar:** `SO_REUSEADDR` antes do `bind()`.
- **Estado:** `Resolvido` - verificado - reinício imediato funciona.

### 4.2 Firewall bloqueia o servidor ou o UDP de descoberta

- **Sintoma:** funciona em `localhost`, falha entre máquinas. O broadcast UDP é o que falha mais.
- **Como evitar:** testar cedo entre duas máquinas reais, não só em loopback. No dia da interoperabilidade, ter à mão o comando para abrir a porta e saber qual é a firewall da máquina (`ufw`, `firewalld`, Windows Defender).
- **Estado:** `Por Resolver` - **M5**. **Testar antes do dia**, não no dia.

### 4.3 IPv4 vs IPv6 - assimetria no nosso código

- **Sintoma:** o cliente de outro grupo não se liga ao nosso servidor, apesar de o nosso cliente se ligar bem.
- **Porquê:** `tcp_listen()` usa `AF_INET` (confirmado: escuta em `0.0.0.0:porta`, só IPv4) mas `tcp_connect()` usa `AF_UNSPEC`. O nosso cliente aguenta porque percorre todos os endereços devolvidos pelo `getaddrinfo()` e cai no IPv4 - testámos com `localhost` e funciona. Um cliente de outro grupo que resolva um AAAA e **não** faça esse fallback liga-se ao nada.
- **Como evitar:** uma de duas, mas decidir explicitamente:
  - declarar **"IPv4 apenas"** no `PROTOCOL.md` (mais simples, e suficiente para uma LAN); ou
  - escutar em `AF_INET6` com `IPV6_V6ONLY=0` (dual-stack). Atenção: no Windows o `IPV6_V6ONLY` vem a **1** por omissão, ao contrário de muitos Linux - se um grupo fizer isto em Windows sem desligar a opção, perde os clientes IPv4.
- **Estado:** funciona, mas por acidente. **Proposta** no `PROTOCOL.md` v0.2 §1: IPv4 apenas (por acordar com os outros grupos).

### 4.4 NAT e endereços privados

- **Sintoma:** o IP que o servidor anuncia no `DISCOVER` não é alcançável.
- **Porquê:** máquina com várias interfaces (Wi-Fi + Ethernet + docker0 + VPN), ou atrás de NAT. O "meu IP" não é uma coisa só.
- **Como evitar:** no M5, responder ao broadcast a partir do endereço **da interface onde o pedido chegou**, não de um IP escolhido a priori. Melhor ainda: o cliente usa o IP de origem do pacote UDP de resposta e ignora qualquer IP que venha no payload.
- **Estado:** `Por Resolver` - **M5**.

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
| Buffer na stack por thread | Com N clientes, N × buffer. Escolher tamanhos conscientemente | `por Resolver` - **M2** |
| Strings sem terminador | `recv()` **não** põe `\0`. Nunca passar o buffer a `printf("%s")` sem limitar o tamanho | `Resolvido` - **M1**: comprimento sempre explícito (`out_len`), linhas com `'\0'` recusadas, `%.*s` nos logs (M1.md, P6) |

**Ferramenta:** `make SAN=1` (ASan + UBSan) já está no Makefile. Usar **sempre** que se testa concorrência, não só quando algo rebenta.

---

## 8. Interoperabilidade

| Problema | Como evitar |
|---|---|
| Cada grupo resolve a semântica ambígua de forma diferente | Fechar as secções *TO AGREE* do `PROTOCOL.md` **com os outros grupos**, por escrito, antes do M3 |
| Porta diferente por grupo | Acordar a porta por omissão (proposta: 5000) |
| O emissor de `MSG` recebe o próprio `CHAT`? | Se um grupo decide que sim e outro que não, as mensagens aparecem duplicadas ou em falta - e parece um bug de rede. **Proposta nossa (D5): sim** - `PROTOCOL.md` v0.2, por acordar |
| `OK` a todos os pedidos ou só a alguns? | Um cliente que espere `OK` bloqueia contra um servidor que não o envia. **Proposta nossa (D6): `OK` ao `NICK`**; o nosso cliente tem timeout de 5 s para não bloquear contra quem decidir diferente |
| Campos desconhecidos | A spec já diz: **ignorar**. Isto permite extensões sem partir ninguém |
| Nicks com maiúsculas/acentos/espaços | **Proposta nossa (D4):** 1–32 bytes, sem espaços nem caracteres de controlo, UTF-8 aceite sem validar; case-sensitive ainda por decidir (importa no M3, nicks únicos) |
| Testar só contra nós próprios | Os cenários em `tests/interop/` são texto puro e correm contra qualquer servidor com `scripts/interop.py` (3 modos de envio). `scripts/probe_client.py` mostra como o nosso cliente reage a servidores que decidiram diferente |

**Regra prática:** ser **estrito a enviar, tolerante a receber**. Nunca rebentar por causa do que o peer mandou - responder `ERROR` e continuar.

---

## 9. Checklist rápido antes de cada entrega

- [ ] `make` sem avisos
- [ ] `make SAN=1` e correr os testes - sem relatórios do ASan/UBSan
- [ ] Matar um cliente com `kill -9` → servidor sobrevive e remove-o
- [ ] Suspender um cliente com `Ctrl+Z` → os outros continuam a falar (M2+)
- [ ] `Ctrl+C` no servidor → fecha os sockets, sem "address already in use" a seguir
- [ ] Enviar lixo (`echo "nao e json" | nc host porta`) → `ERROR`, não crash
- [ ] Enviar uma linha de 1 MB sem `\n` → `ERROR`, não overflow
- [ ] Testar entre **duas máquinas**, não só em loopback
- [ ] Testar contra o servidor de outro grupo
- [ ] Notas do milestone preenchidas em `docs/milestones/`
