# Decisões a acordar com os outros grupos

Tudo o que **tem de ser falado** com os outros grupos para que o nosso cliente
funcione com os servidores deles e o servidor nosso com os clientes deles. Os
nomes das mensagens e dos campos vêm do slide 124 e **não se discutem**. O que
se discute é a **semântica** que o slide deixa em aberto (*"you may need to
define semantics"*).

- A especificação está em [`PROTOCOL.md`](PROTOCOL.md) (v0.5). Este ficheiro
  diz **o que perguntar** e o que acontece se dois grupos decidirem diferente.
- Cada linha tem a **nossa proposta**, já implementada e testada. Uma proposta
  não é uma imposição: se a maioria decidir diferente, mudamos. Cada decisão
  está num só sítio do código (indicado na coluna "No código"), por isso
  mudar é barato.
- Prioridades: **A** parte a interoperabilidade se divergir; **B** só muda o
  que o utilizador vê; **C** é interna e não precisa de acordo (mas convém
  dizer); **D** é de fases seguintes e convém combinar cedo.
- Quando um ponto ficar acordado, passa a **ACORDADO** aqui e no
  `PROTOCOL.md`. Com todos os pontos A acordados, o `PROTOCOL.md` passa a v1.0.

---

## Registo dos contactos

| Grupo | Linguagem | Quem | Data | Pontos A acordados | `interop.py` contra o servidor deles | O cliente deles contra o nosso |
|---|---|---|---|---|---|---|
| | | | | | | |
| | | | | | | |
| | | | | | | |

---

## A — Partem a interoperabilidade se divergirem

### A.1 Transporte e enquadramento

| # | Pergunta | Nossa proposta | Se divergirem | No código | Teste |
|---|---|---|---|---|---|
| A1 | Porta por omissão | `5000` | cada um tem de escrever a porta à mão; inofensivo mas chato no dia | `DEFAULT_PORT` (server/client `main.c`) | — |
| A2 | IPv4, IPv6 ou ambos? | **IPv4**: o servidor escuta em `0.0.0.0`; o cliente cai para IPv4 se o nome der IPv6 primeiro | um cliente que só tente o AAAA não liga ao nosso servidor | `net.c` | `typical-problems.md` §4.3 |
| A3 | Delimitador | `\n`; aceitar também `\r\n` (tirar um `\r`) | um grupo em Windows que mande `\r\n` vê `"invalid JSON"` num servidor que não tire o `\r` | `framing.c` | `test_framing` |
| A4 | Linhas vazias | ignorar, sem erro | um `Enter` a mais num `nc` dá `ERROR` num lado e nada no outro | `framing.c` | `test_framing` |
| A5 | Tamanho máximo de uma linha | **4096 bytes, incluindo o `\n`** | uma mensagem de 3000 bytes passa num lado e é recusada no outro | `RX_MAX_LINE` (`framing.h`) | `test_framing` #7, #21 |
| A6 | Linha maior do que o máximo | `ERROR 400 "line too long"` e **fechar** (não se ressincroniza) | quem tentar ressincronizar interpreta o resto da linha como mensagens | `server/main.c` | M1.md caso hostil B |
| A7 | Uma linha com dois objetos (`{..}{..}`) ou lixo depois | `ERROR 400 "invalid JSON"` | um servidor que aceite o 1.º perde o 2.º em silêncio | `protocol.c` | `test_protocol` #25 |

### A.2 Campos

| # | Pergunta | Nossa proposta | Se divergirem | No código | Teste |
|---|---|---|---|---|---|
| A8 | Limite do nick | **1–32 bytes** (bytes, não caracteres) | um nick de 40 bytes entra num servidor e o cliente de outro grupo recusa o `CHAT` dele | `PROTO_NICK_MAX` | `test_protocol` #26 |
| A9 | Caracteres proibidos no nick | **espaço, `"`, `\` e controlo** (< 0x20, 0x7F) | espaços partem o `/msg nick texto`; com `"`/`\` o pior `USERS` (64 × 32 bytes) passa de 4096 bytes e não cabe numa linha (cálculo no `PROTOCOL.md` §3.1) | `proto_valid_nick()` | `test_protocol` #61, #59 |
| A10 | E nicks que **recebemos** de um servidor com regras mais largas? | o cliente aceita `"` e `\` em `from`/`USERS` (estrito a enviar, tolerante a receber) | um cliente estrito recusa mensagens inteiras de um servidor mais permissivo | `valid_peer_nick()` | `test_protocol` #61 |
| A11 | Limite do texto | **1–1024 bytes**, sem caracteres de controlo (logo sem mensagens de várias linhas) | uma mensagem de 2000 bytes passa num servidor e é recusada no cliente de outro grupo | `PROTO_TEXT_MAX` | `test_protocol` #26 |
| A12 | UTF-8 | não se valida; contam-se bytes | um grupo que conte caracteres aceita um nick com 32 letras acentuadas (64 bytes) que nós recusamos | `protocol.c` (D3) | `interop` 06 |
| A13 | Campos desconhecidos | **ignorar** | um servidor que recuse campos extra parte com qualquer extensão (incluindo o nosso `"to"`, A20) | `protocol.c` | `interop` 05 |
| A14 | Maiúsculas nas chaves e no `type` | **case-sensitive** (`"Type"` ≠ `"type"`, `"list"` ≠ `"LIST"`) | um aceita, outro recusa | `protocol.c` | `test_protocol` #15, #32 |
| A15 | Campos opcionais | `private` ausente = `false`; `reason` ausente = `""` | um servidor que omita `private` parte um cliente que o exija | `protocol.c` | `test_protocol` #30 |
| A16 | `code` do `ERROR` | inteiro 100–999 | `"409"` (string) é recusado por nós | `protocol.c` | `test_protocol` #30 |

### A.3 Respostas: quem recebe o quê

| # | Pergunta | Nossa proposta | Se divergirem | No código | Teste |
|---|---|---|---|---|---|
| A17 | **Que pedidos recebem `OK`?** | regra geral: **cada pedido tem UMA resposta direta**. `NICK` → `OK`; `MSG` → o próprio `CHAT`; `PRIV` → a cópia do `CHAT`; `LIST` → `USERS`; `QUIT` → `OK`. Ou um `ERROR` | um cliente que espere `OK` a cada `MSG` fica à espera para sempre; um que não o espere mostra `[ok]` a cada linha | `session.c` (D6, D13) | `interop` 01, 03 |
| A18 | O emissor de `MSG` recebe o **próprio** `CHAT`? | **sim** — é a confirmação de que chegou; por isso o cliente **não** faz eco local | mensagens **duplicadas** (eco local + servidor) ou **em falta** (sem eco nenhum) | `SESS_ALL` (D5) | `interop` 03 |
| A19 | O emissor de `PRIV` recebe o quê? | **uma cópia** do `CHAT` privado (a mesma regra da A18) | o mesmo que A18, nas mensagens privadas | `handle_priv()` (D14) | `interop` 08 |
| A20 | Campo `"to"` no `CHAT` privado (extensão nossa) | `{"type":"CHAT","from":"ana","to":"rui","text":"hi","private":true}` — sem `"to"` a cópia do emissor não diz para quem foi. Num `CHAT` público nunca aparece | inofensivo para quem ignora campos desconhecidos (A13); só se perde o "para quem" | `protocol.c`, `ui.c` | `test_protocol` #60 |
| A21 | `PRIV` a si próprio | entregue **uma vez** | inofensivo (pode aparecer duplicado) | `handle_priv()` | `test_session` #66 |
| A22 | `QUIT`: o servidor responde e fecha? | **`OK` e fecha** (FIN logo a seguir ao `OK`); o que vier depois do `QUIT` é ignorado | um cliente que espere `OK` antes de sair fica pendurado num servidor que só feche; um que não o espere mostra `[ok]` | `case MT_QUIT` (D17) | `interop` 11 |
| A23 | `QUIT` antes do `NICK` | **permitido**: `OK` e fecha | `401` num lado, `OK` no outro | `case MT_QUIT` | `interop` 12 |
| A24 | O primeiro pedido tem de ser `NICK`. E se não for? | `MSG`, `PRIV`, `LIST` → **`ERROR 401 "nick required"`**, ligação aberta | um servidor que feche a ligação parte um cliente que mande `LIST` cedo demais | `session.c` | `interop` 02, 12 |
| A25 | Há tráfego antes do `NICK` aceite? | **não**: a ligação não está no chat; a 1.ª linha que o cliente recebe é **sempre** a resposta ao `NICK` | um cliente que tome a 1.ª linha pela resposta lê um `CHAT` como se fosse `OK` (o nosso cliente já não assume isto) | `roster_join()` depois do `OK` (D9) | `probe_m2.py` §6 |
| A26 | `NICK` repetido na mesma ligação | `ERROR 400 "nick already set"` (não há mudança de nick) | um servidor que mude o nick deixa os outros sem saber quem é quem | `session.c` | `test_session` #36 |
| A27 | Um cliente manda um tipo S→C (`OK`, `CHAT`, `USERS`, ...) | `ERROR 400 "unexpected type"` | inofensivo se for `ERROR` (só muda o motivo) | `session.c` | `test_session` #37, #70 |
| A28 | Uma linha inválida fecha a ligação? | **não**: `ERROR 400` e a próxima linha é tratada normalmente (exceto "line too long") | um cliente com um bug numa mensagem perde a ligação num lado e não no outro | `session.c` | `interop` 04 |
| A29 | O `from` de um `CHAT` | sempre o nick da sessão; um `"from"` mandado pelo cliente é ignorado | um servidor que o aceite deixa qualquer um fazer-se passar por outro | `session.c` | `interop` 05 |

### A.4 Nicks únicos

| # | Pergunta | Nossa proposta | Se divergirem | No código | Teste |
|---|---|---|---|---|---|
| A30 | **Os nicks são case-sensitive?** | **Não, para a unicidade**: `Ana`, `ana` e `ANA` são o mesmo nick. Só letras ASCII (`Zé` ≠ `ZÉ`). O nick é guardado e mostrado como foi escrito | um servidor aceita `Ana` e `ana` ao mesmo tempo (fácil de se fazer passar por outro), o outro dá 409 | `proto_nick_eq()` (D18) | `interop` 07; `test_protocol` #62 |
| A31 | O `PRIV` encontra o destino da mesma forma? | **sim**, mesma comparação; o `"to"` do `CHAT` sai com o nick como foi registado (`/msg ANA` → `"to":"ana"`) | `/msg Rui` funciona num servidor e dá 404 noutro | `proto_nick_eq()` | `test_session` #64 |
| A32 | Depois de um `409`, a ligação continua? | **sim**: o estado não muda e o cliente pode tentar outro nick | um cliente que tente outro nick numa ligação que o servidor já fechou falha; um que desista à 1.ª perde a hipótese de tentar outro | `session.c` (D19) | `interop` 07 |
| A33 | Quando fica livre o nick de quem sai? | quando a ligação sai da lista (`QUIT` ou queda); logo a seguir pode ser reutilizado | inofensivo | `roster_remove()` | `probe_m3.py` §6 |

### A.5 Códigos de erro

O cliente deve decidir pelo **código**; o `reason` é texto livre para mostrar.

| # | Código | Quando (nossa proposta) | Reason que enviamos | Estado |
|---|---|---|---|---|
| A34 | 400 | JSON inválido; campo em falta/errado/fora dos limites; tipo desconhecido; tipo S→C vindo de um cliente; segundo `NICK`; linha demasiado longa | `invalid JSON`, `invalid field`, `unknown type`, `unexpected type`, `nick already set`, `line too long` | PROPOSTA |
| A35 | 401 | `MSG`/`PRIV`/`LIST` antes de um `NICK` aceite | `nick required` | PROPOSTA |
| A36 | 404 | `PRIV` para um nick que não está no chat | `no such nick` | PROPOSTA |
| A37 | 409 | `NICK` em uso | `nick taken` | **do slide 124** |
| A38 | 503 | servidor cheio; o servidor fecha a seguir | `server full` | PROPOSTA |
| A39 | — | Usar outros códigos? (ex.: 413 para linha longa, 422 para campo inválido) | — | proposta: **não**, só estes cinco; quanto menos, menos para divergir |

---

## B — Só mudam o que o utilizador vê

| # | Pergunta | Nossa proposta | Se divergirem | No código |
|---|---|---|---|---|
| B1 | Texto dos `EVENT` | `"<nick> joined"` e `"<nick> left"` (o exemplo do slide é `"rui joined"`); `QUIT` e queda dão o mesmo `"left"` | o texto aparece diferente; nada parte. Um cliente que **interprete** o texto para manter uma lista local parte — por isso a lista vem do `LIST` | `fill_event()` (D10) |
| B2 | Quem recebe o `EVENT` | todos **menos** o próprio | quem entra vê um aviso de si próprio; inofensivo | `SESS_OTHERS` (D10) |
| B3 | `USERS` inclui quem pediu? | **sim** | um cliente que conte "N pessoas ligadas" dá N±1 | `handle_list()` (D16) |
| B4 | Ordem do `USERS` | **alfabética, sem olhar a maiúsculas** | os testes que comparem a lista exata falham; o utilizador não nota | `handle_list()` (D16) |
| B5 | `USERS` mostra quem ainda não fez `NICK`? | **não** (não têm nick) | — | `roster_nicks()` |
| B6 | Servidor cheio | `ERROR 503` e fecho; o limite de clientes é de cada um (o nosso: 64) | um fecho sem explicação | `server/main.c` (D12) |
| B7 | Cliente que não lê | desligado se um envio não sair em 5 s; o valor é de cada um, o que importa é que **ninguém bloqueie o chat** | um servidor sem prazo congela com um cliente suspenso | `SEND_TIMEOUT_MS` (D11) |
| B8 | Texto dos `reason` | os da tabela A.5 | nada parte: os clientes usam o código | `protocol.h`, `session.h` |

---

## C — Internas (não precisam de acordo, mas convém dizer)

| # | O quê | A nossa escolha |
|---|---|---|
| C1 | Concorrência no servidor | uma thread por cliente (não `select`/`poll`) |
| C2 | Máximo de clientes | 64 (`ROSTER_MAX`) |
| C3 | Prazo de envio | 5 s por mensagem, prazo total (não `SO_SNDTIMEO`) |
| C4 | Comandos do cliente | `/list`, `/msg <nick> <texto>`, `/quit`, `/help`, `//texto` para uma linha começada por `/`; comando desconhecido recusado localmente (nunca vai para o chat) |
| C5 | Validação no cliente | o cliente valida com as mesmas regras do servidor antes de enviar |
| C6 | Cliente à espera do `OK` ao `NICK` | 5 s; mostra o que vier antes do `OK` em vez de o tomar por resposta |
| C7 | Ctrl+D no cliente | envia `QUIT`, depois fecha o envio (FIN) |
| C8 | Linguagem | C11 + POSIX + cJSON |

---

## D — Fases seguintes: combinar cedo

Não são do M3, mas são do mesmo tipo: se cada grupo decidir sozinho, só se
descobre no dia da interoperabilidade.

| # | Fase | Pergunta | Ideia inicial (a discutir, ainda não implementada) |
|---|---|---|---|
| D1 | M4 | Idle timeout: quanto tempo sem tráfego até o servidor desligar um cliente? | ≥ 5 min; um chat parado não é um cliente morto |
| D2 | M4 | O servidor avisa antes de desligar por inatividade? Com que código? | `ERROR 408 "idle timeout"` e fecho (408 = HTTP *Request Timeout*) |
| D3 | M4 | Há *keepalive*? | o slide 124 não tem `PING`; um tipo novo seria `unknown type` noutros servidores. Proposta: **não** criar tipo novo; quem quiser manter a ligação manda um `LIST` |
| D4 | M4 | Ctrl+C no servidor: avisa os clientes? | `EVENT "server shutting down"` a todos, depois fecho |
| D5 | M5 | Formato do pedido UDP | `{"type":"DISCOVER"}` numa linha, em broadcast |
| D6 | M5 | Formato da resposta | `{"type":"SERVER","port":5000,"name":"<grupo>"}`; o IP é o de **origem** do pacote, nunca um campo do JSON (`typical-problems.md` §4.4) |
| D7 | M5 | Porta UDP de descoberta | uma só para a turma (ex.: `5001`) |
| D8 | M5 | Quanto tempo espera o cliente por respostas? Vários servidores? | 1 s; lista todos os que responderem |
| D9 | M6 | Que extensão escolhe cada grupo? | se dois grupos escolherem a mesma (ex.: salas `/join`), acordar os formatos entre eles |
| D10 | M6-A | Ficheiros: formato do cabeçalho e do prefixo de comprimento | a combinar entre os grupos que a escolherem |
| D11 | M6-B | TLS: na mesma porta ou noutra? | outra porta (ex.: `5443`), para não partir clientes sem TLS |

---

## Perguntas abertas que mais importam

Se só houver tempo para cinco perguntas, estas são as que mais partem
(por ordem):

1. **A18/A19** — o emissor recebe o próprio `CHAT` (e a cópia do `PRIV`)?
2. **A17/A22** — que pedidos recebem `OK`? (em especial `QUIT`)
3. **A34–A39** — os códigos de erro (400/401/404/409/503) e só estes?
4. **A5/A8/A9/A11** — linha máxima e limites dos campos (nick sem `"` e `\`)
5. **A30/A32** — nicks sem olhar a maiúsculas? e o `409` mantém a ligação?

---

## Mensagem para enviar aos grupos

> Olá! Somos o grupo do chat em C (repositório Future_Networks). Para o M3
> temos uma proposta de semântica completa, já implementada e testada, no
> nosso `docs/PROTOCOL.md` (v0.5), e a lista do que falta acordar em
> `docs/DECISIONS.md`. As cinco perguntas que mais partem a interoperabilidade
> são:
>
> 1. O emissor de `MSG` recebe o próprio `CHAT`? E o de `PRIV` recebe uma cópia? (nós: sim e sim, sem eco local)
> 2. Que pedidos recebem `OK`? (nós: `NICK` e `QUIT`; `MSG`/`PRIV` são confirmados pelo `CHAT`, `LIST` pelo `USERS`)
> 3. Códigos de erro: 400 / 401 (antes do `NICK`) / 404 (`PRIV` para quem não está) / 409 (nick em uso) / 503 (cheio)?
> 4. Linha até 4096 bytes; nick 1–32 bytes sem espaços, `"` nem `\`; texto 1–1024 bytes?
> 5. `Ana` e `ana` são o mesmo nick? E depois de um `409` a ligação continua para tentar outro?
>
> Temos também cenários de teste em texto que correm contra qualquer servidor
> (`python3 scripts/interop.py <ip> <porta>`). Combinamos uma sessão de teste
> cruzado: o nosso cliente contra o vosso servidor e o vosso cliente contra o
> nosso?
