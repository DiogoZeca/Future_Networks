# A camada de enquadramento (framing)

Documento da branch `feat/m1-framing-contract`. Explica **o que foi feito, e
porquê cada decisão** — é o material para a defesa desta parte do M1.

> **Estado actual:** contrato e testes escritos; implementação por fazer.
> `make test` dá **11 ok, 88 falhas**, de propósito.

---

## 1. O problema que esta camada resolve

No M0 medimos (está em `docs/milestones/M0.md`): um `send()` único de **10 001
bytes** chegou ao servidor em **três `recv()`** — 4096 + 4096 + 1809.

Para um servidor de eco isso é indiferente: bytes para trás são bytes para trás.
A partir do momento em que as mensagens são JSON, é fatal:

```
recv #1 -> {"type":"MSG","te        <- JSON invalido
recv #2 -> xt":"ola"}               <- JSON invalido
```

Nenhum byte se perdeu na rede. Mas se cada `recv()` for tratado como uma
mensagem, as duas metades são deitadas fora.

E o inverso acontece com a mesma frequência:

```
recv #1 -> {"type":"NICK","nick":"ana"}\n{"type":"MSG","text":"ola"}\n
```

Aqui o cJSON lê o **primeiro** objecto, fica satisfeito, e **ignora o resto em
silêncio**. A `ana` entra no chat, o "ola" nunca existiu, e não há erro nenhum.
Este é o pior dos dois, porque falha sem avisar.

**A regra que resolve ambos:** nunca interpretar o que o `recv()` deu. Acumular
num buffer e extrair só o que terminar em `\n`.

---

## 2. O que foi escrito

| Ficheiro | O que é |
|---|---|
| `include/framing.h` | **o contrato** — constantes, `struct rxbuf`, retornos, API |
| `src/common/framing.c` | **stubs** — a implementação vem a seguir |
| `tests/unit/test_util.h` | runner mínimo (~50 linhas) |
| `tests/unit/test_framing.c` | 14 casos de teste |
| `Makefile` | alvo `make test` |

A camada **não sabe o que é JSON**. Só conhece bytes e o byte `0x0A`. É essa
separação que a torna testável sem rede — e é por isso que foi possível escrever
os testes antes da implementação.

---

## 3. O contrato, decisão a decisão

### 3.1 As capacidades, e porque `RX_CAP` é 8192

```c
#define RX_MAX_LINE 4096                      /* linha INCLUINDO o '\n' */
#define RX_CHUNK    4096                      /* maximo por feed        */
#define RX_CAP      (RX_MAX_LINE + RX_CHUNK)  /* 8192                   */

_Static_assert(RX_CAP >= RX_MAX_LINE + RX_CHUNK, "...");
```

**O 8192 não é um número redondo escolhido por gosto — é a soma de duas
quantidades:**

- depois de extrair todas as linhas completas, o resíduo é uma linha
  *incompleta*, logo **sem `\n`**, logo no máximo **4095** bytes (4096 incluiria
  o `\n` que não está lá);
- o `feed` seguinte traz no máximo `RX_CHUNK` = **4096**;
- pior caso: 4095 + 4096 = **8191 ≤ 8192**. ✅

Isto resolve a armadilha que o `M1.md` assinala como decisão **D1**: os dois
`main.c` tinham `BUF_SIZE 4096` e o limite de linha também é 4096. **Não podem
ser o mesmo número** — o buffer tem de conter o resto da leitura anterior *mais*
uma linha nova. O `_Static_assert` garante que isto continua verdade se alguém
mexer num dos lados: se a soma deixar de bater, **não compila**.

As três constantes vivem todas no mesmo header, de propósito. Enquanto o
`BUF_SIZE` estiver num `main.c` e o `RX_CAP` noutro ficheiro, a relação entre
eles é invisível e ninguém a verifica.

### 3.2 A struct — porquê três campos e não um

```c
struct rxbuf {
    size_t len;      /* bytes validos em data[0..len)                       */
    size_t scanned;  /* data[0..scanned) ja procurado e NAO contem '\n'     */
    int    failed;   /* 1 = envenenado; so rxbuf_init() limpa               */
    char   data[RX_CAP];
};
```

**`scanned`** — sem ele, cada chamada varre o buffer inteiro à procura de `\n`.
Com entrega byte-a-byte (que é o caso de teste #2) isso é O(n²): cerca de **8
milhões de comparações** para uma linha de 4 KB. Mais importante do que a
velocidade: só é escrito em dois sítios (`= len` quando não se encontrou nada,
`= 0` depois de extrair), e saber *porquê* zero é a prova de que o invariante se
percebeu — o `memmove` desloca tudo, portanto o que já tinha sido procurado
mudou de sítio.

**`failed`** — torna a decisão **D2** ("nunca ressincronizar") uma propriedade
**estrutural** em vez de uma convenção que alguém esquece. Depois de uma linha
demasiado longa não se sabe onde começa a mensagem seguinte; tentar apanhar o
próximo `\n` parece simpático mas é pior, porque não se sabe se esse `\n` é o
fim da linha gigante ou o meio de outra coisa. O buffer fica inutilizável de
propósito, e só o `rxbuf_init()` o recupera.

A struct é **pública, não opaca**: no M2 vai ser embebida *por valor* dentro de
uma `struct conn`, sem `malloc` — logo sem caminho de `free` para esquecer.

### 3.3 Quatro valores de retorno, não dois

```c
RX_LINE    =  1   /* linha extraida                           */
RX_NEED    =  0   /* faltam bytes: chamar recv() outra vez     */
RX_TOOLONG = -1   /* culpa DELES: linha > 4096. Envenenado     */
RX_FAULT   = -2   /* culpa NOSSA: out==NULL, cap pequeno demais*/
```

O plano original do `M1.md` propunha o ciclo:

```c
while (rxbuf_next_line(&rx, line, sizeof line) == 1)   /* <- ERRADO */
```

**Isto tem o mesmo defeito que o documento denuncia duas linhas acima.** Com
`== 1`, o `-1` (erro) é indistinguível do `0` (faltam bytes): o ciclo pára nos
dois casos e volta ao `recv()` com o buffer envenenado. O correcto é ramificar
em três:

```c
int r = rxbuf_next_line(&rx, line, sizeof line, &len);
if (r == RX_NEED) break;             /* faltam bytes */
if (r != RX_LINE) { /* fechar */ }   /* erro         */
processar(line, len);
```

**`RX_TOOLONG` vs `RX_FAULT` é "culpa deles" vs "culpa nossa".** Os dois levam a
fechar a ligação, mas o log é diferente — e é essa a diferença entre depurar em
2 minutos ou em 2 horas. O `RX_FAULT` nunca deve aparecer em produção; se
aparecer num teste, é um bug de chamada, não uma mensagem malformada.

### 3.4 `out_len` — porque `strlen()` não chega

```c
int rxbuf_next_line(struct rxbuf *rx, char *out, size_t cap, size_t *out_len);
```

Uma linha pode conter um `'\0'` embebido (um peer com bugs, ou malicioso). Com
`strlen(out)` o resto da linha **desaparecia em silêncio**. É o caso de teste
#20, e é também o que permite usar `cJSON_ParseWithLength()` em vez de
`cJSON_Parse()` quando a camada de protocolo chegar.

### 3.5 Copiar para fora, em vez de devolver ponteiro

A alternativa mais rápida seria devolver um ponteiro para dentro do `data` e
trocar o `\n` por `\0`. Funciona — e cria uma regra de tempo de vida:
*"o ponteiro é inválido depois da próxima chamada"*.

Essa regra rebenta exactamente no **M2**, quando a linha for passada para dentro
de um broadcast e sobreviver à iteração seguinte. Com cópia, o `char
line[RX_LINE_BUF]` é do chamador e não há ponteiro pendente possível.

Custo: 4 KB por linha. Comprá-lo é barato.

### 3.6 Linhas vazias são saltadas *aqui dentro*

Pós-condição forte: **`RX_LINE` implica `*out_len >= 1`**.

Se as linhas vazias fossem devolvidas, cada chamador precisaria do mesmo
`if (len == 0) continue;` — e o do cliente seria esquecido. De borda, isto trata
um `"\r\n"` isolado como keep-alive, que é o que um peer de outro grupo mais
provavelmente há-de mandar.

### 3.7 UTF-8: não se faz nada, e é seguro

Os bytes são **opacos** até haver linha completa (decisão **D3**).

**Partir em `\n` é sempre seguro em UTF-8**, porque todos os bytes de
continuação de uma sequência multibyte são ≥ `0x80` — logo `0x0A` nunca ocorre
*dentro* de um caracter. O bug de acentos corrompidos aparece quando se faz
`printf("%s")` sobre um buffer *parcial*; a regra é não haver nenhum `printf`
sobre o `data`, e nos logs usar `%.*s` com o `out_len`.

---

## 4. Porquê stubs em vez de só os testes

`src/common/framing.c` tem as três funções a devolver erro, com `/* TODO M1 */`.

Em C, um ficheiro de teste sem a implementação **nem sequer linka** — dá
`undefined reference`. E um `make test` que não compila é indistinguível de um
Makefile partido.

Com stubs, o vermelho é um **relatório legível**:

```
framing: 11 ok, 88 falhas
RESULTADO: FALHOU
```

E isso é a prova de que os testes foram escritos **antes** da implementação, que
era a decisão tomada no plano. (Os 11 que passam são os que por acaso batem
certo com os stubs — por exemplo, `rxbuf_pending()` devolver 0 quando se esperava
0.)

---

## 5. Os testes

Sem rede. É isso que permite reproduzir à vontade os casos que em produção
acontecem ao acaso.

| # | O que provoca | Esperado |
|---|---|---|
| 1 | mensagem completa num feed | 1 linha, buffer vazio |
| **2** | **a mesma, byte a byte** | 1 linha, só no último byte |
| 3 | duas mensagens coladas | 2 linhas |
| 4 | uma mensagem e meia | 1 linha; o resto preservado |
| 5 | `\r\n`, e `\r\r\n` | um só `\r` removido |
| **6** | **5000 bytes sem `\n`** | `RX_TOOLONG`, sem overflow |
| 7 | linha exactamente no limite | aceite |
| 8 | `\n\n\n`, `\r\n` isolado | ignoradas, não são erro |
| 9 | `"á"` partido entre dois feed | linha correcta, 4 bytes |
| 10 | feed depois de erro | recusa; só `init` limpa |
| 19 | `feed(rx, "", 0)` | no-op, não é erro |
| 20 | `'\0'` embebido | `out_len` ≠ `strlen(out)` |
| 21 | 4095+`\n` / 4096+`\n` | aceite / rejeitado |
| **22** | **resíduo 4095 + chunk 4096** | 8191 cabe em `RX_CAP` |

Três merecem destaque:

- **#2 (byte a byte)** é o mais valioso de todos: apanha quase todos os bugs de
  enquadramento e é trivial de escrever.
- **#6** só prova alguma coisa sob `make test SAN=1` — é o AddressSanitizer que
  confirma que não houve escrita fora do `data[RX_CAP]`. Sem ele, o teste está a
  acreditar na própria implementação.
- **#22 executa** a aritmética da §3.1 em vez de a argumentar. Mete 4095 bytes
  sem `\n`, drena (tem de dar `RX_NEED`), e mete mais 4096. É o pior caso real,
  e vale tanto como os outros todos juntos para a defesa.

### Porquê um runner escrito à mão

São casos sobre funções puras: sem rede, sem mocks, sem fixtures, sem
setup/teardown. "Chamar uma função, comparar, contar" são as ~50 linhas do
`test_util.h`.

Um framework (Unity, Check, CUnit) seria mais uma dependência a justificar, e o
Check e o CUnit exigem instalação no sistema — mais uma barreira para quem
clonar o repositório. O trabalho pesado (overflows, fugas) é do `make SAN=1`,
não do runner.

---

## 6. Como correr

```sh
make test          # rapido, da o feedback
make test SAN=1    # o que realmente conta: ASan + UBSan
```

O `make test` corre **todas** as suites (não pára na primeira — queremos o
panorama todo) e devolve `!= 0` se alguma falhar. O `make` normal continua a
**não** construir os testes.

Acrescentar uma suite nova não exige mexer no Makefile: o `wildcard` sobre
`tests/unit/test_*.c` apanha-a sozinho.

---

## 7. O que falta

A implementação, em `src/common/framing.c`. A ordem das operações dentro do
`rxbuf_next_line()` não é negociável — cada passo protege o seguinte:

1. se `failed` → `RX_TOOLONG` (idempotente)
2. se `out == NULL` ou `cap` pequeno demais → `RX_FAULT`
3. `p = memchr(data + scanned, '\n', len - scanned)`
4. `p == NULL`: `scanned = len`; se `len >= RX_MAX_LINE` → envenenar; senão `RX_NEED`
5. `consume = (p - data) + 1`; se `consume > RX_MAX_LINE` → envenenar
6. tirar um `\r` final, se existir
7. **copiar para `out` PRIMEIRO**
8. **só agora** compactar com `memmove`, e `scanned = 0`
9. se o conteúdo ficou vazio → `continue`
10. `*out_len = content` → `RX_LINE`

Duas armadilhas neste bloco:

- **Passo 7 antes do 8.** Compactar antes de copiar destrói a linha — e com
  `memmove` o bug é *silencioso*, porque os dados ainda lá estão, só no sítio
  errado.
- **`memmove`, nunca `memcpy`**, no passo 8: as regiões sobrepõem-se, e `memcpy`
  nessas condições é comportamento indefinido. Só o ASan ou o valgrind apanham.

E o `>=` do passo 4 (e não `>`) é o que faz o teste #7 passar e o #6 falhar: com
`len == 4096` e ainda sem `\n`, qualquer linha que acabe ali terá pelo menos
4097 bytes com o `\n`.

---

## Invariantes a manter

```
I1.  len <= RX_CAP
I2.  scanned <= len
I3.  nao existe '\n' em data[0..scanned)
I4.  failed != 0  =>  len == 0 && scanned == 0
I5.  RX_LINE  =>  out[*out_len] == '\0'  &&  1 <= *out_len <= RX_MAX_LINE-1
```

Vale a pena pô-los em `assert()` à entrada e à saída de cada função: são
gratuitos em release (`-DNDEBUG`) e, nos testes, transformam um bug de índice
num *abort* com linha exacta em vez de dados corrompidos 200 ms depois.
