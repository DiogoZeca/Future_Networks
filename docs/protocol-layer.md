# A camada de protocolo (linhas <-> mensagens)

Par do [`framing.md`](framing.md): explica **o que foi feito e porquê** na
segunda camada do M1. Material para a defesa.

> **Estado:** implementado. `make test` e `make test SAN=1` dão
> **protocol: 125 ok, 0 falhas** (15 casos). Testes escritos antes do código,
> contra stubs (43 ok, 78 falhas), e validados com testes de mutação (§6).

```
socket --bytes--> framing --linhas--> protocol --struct msg--> lógica
                  (rxbuf)             (cJSON)
```

| Ficheiro | O que é |
|---|---|
| `include/protocol.h` | o contrato: limites, `struct msg`, `proto_parse`, `proto_encode` |
| `src/common/protocol.c` | a implementação, a única que conhece o cJSON |
| `tests/unit/test_protocol.c` | 15 casos (11–16, 18, 25–32) |
| `Makefile` | `LDLIBS := -lcjson` |

---

## 1. A ideia

O framing entrega uma linha. Esta camada devolve uma **`struct msg` em que todos
os campos já foram verificados** — existem, são do tipo certo, respeitam os
limites. O resto do programa nunca toca em cJSON: se tem uma `struct msg`,
pode confiar nela. Tal como o framing, não toca em sockets, por isso testa-se
sem rede.

Só os tipos do M1: `NICK`, `MSG` (C→S) e `OK`, `CHAT`, `ERROR` (S→C). Os
restantes do slide 124 entram no M3; até lá são `unknown type`.

> **Desde o M3** a camada tem todos os tipos do slide 124 (`EVENT` no M2;
> `PRIV`, `LIST`, `QUIT`, `USERS` no M3), o primeiro array (`USERS.nicks`), um
> campo opcional `to` no `CHAT`, e nicks sem `"` nem `\` (D20). O porquê de
> cada mudança está em [`milestones/M3.md`](milestones/M3.md).

---

## 2. Decisões do contrato

### 2.1 Buffers fixos na `struct msg`, sem `malloc`

`nick[33]`, `text[1025]`, … — o `cJSON` é libertado dentro do `proto_parse()` e
o chamador fica com uma cópia. **Não há `free` do lado do chamador para
esquecer**, e a struct pode viver na stack ou dentro da futura `struct conn`.

### 2.2 Limites (decisão D4 — proposta, *TO AGREE*)

| Campo | Regra |
|---|---|
| `nick` / `from` | 1–32 bytes, sem espaços nem caracteres de controlo |
| `text` | 1–1024 bytes, sem caracteres de controlo |
| `reason` | 0–128 bytes, sem caracteres de controlo |
| `code` | inteiro 100–999 |

- **Bytes, não caracteres.** `"olá"` são 4 bytes. Não se valida UTF-8 (D3).
- **Nick sem espaços** porque no M3 o `/msg <nick> texto` parte por espaço.
- **M3 (D20):** o nick também deixou de aceitar `"` e `\` — o pior `USERS`
  (64 nicks) não cabia numa linha com eles. Nicks vindos de um *servidor*
  (`CHAT.from`, `USERS`) continuam a aceitá-los: estrito a enviar, tolerante a
  receber.
- **Sem caracteres de controlo** (< 0x20 e 0x7F) — logo **sem mensagens de
  várias linhas**. Tem uma segunda razão, a da §2.3.

São validados **no parse e no encode**: nunca enviamos o que recusaríamos.

### 2.3 O pior caso cabe numa linha — e é verificado ao compilar

O cJSON escapa `"` e `\` com 2 bytes, e caracteres de controlo com até 6
(`\u0001`). Como estes últimos são recusados, **nenhum byte cresce mais de 2×**.
A maior mensagem possível é um `CHAT` com `from` e `text` no limite, cheios de `"`:

```
{"type":"CHAT","from":"","text":"","private":false}\n   52 bytes fixos
+ 2×32 + 2×1024                                          2112
                                                         2164  <= 4096
```

O `_Static_assert` no `protocol.h` faz esta conta: se alguém subir os limites
sem pensar, **não compila**. O teste #27 executa o pior caso. É o mesmo
raciocínio do `RX_CAP` no framing.

Sem esta regra, um `text` de 1024 caracteres de controlo daria ~6 KB
codificado — maior do que a linha máxima do outro lado, que fecharia a ligação.

### 2.4 Lixo depois do objeto é recusado

`cJSON_Parse("{...}{...}")` lê o primeiro objeto e **ignora o resto em
silêncio** — exatamente a falha que o framing existe para evitar. Por isso
usa-se `cJSON_ParseWithLengthOpts()`, que diz onde o parse acabou, e exige-se
que daí até ao fim só haja espaços (#25).

O mesmo para `'\0'` embebido (#29): o framing deixa-o passar (caso #20 de lá);
o cJSON pararia nele e leria só metade.

### 2.5 Três razões de erro, sempre `400`

| `reason` | Quando |
|---|---|
| `invalid JSON` | não é JSON, lixo a seguir, `'\0'`, raiz não é objeto |
| `unknown type` | `type` é string mas não é um tipo do M1 |
| `invalid field` | campo em falta, tipo errado, fora dos limites |

Vão para o fio tal e qual, por isso em inglês, como o `nick taken` do slide.
O `401` (`MSG` antes de `NICK`) **não é desta camada**: depende do estado da
ligação, por isso é da máquina de estados do servidor (próxima etapa, teste #17).

### 2.6 Opcionais com valor por omissão

`private` ausente = `false`; `reason` ausente = `""`. Se presentes, têm de ser
do tipo certo (`"private":"yes"` → 400). Campos desconhecidos são ignorados
(PROTOCOL.md §3). Chaves e valores de `type` são **case-sensitive** (#15, #32).

---

## 3. A implementação

### Parse — validar antes de tocar

Para cada campo: (1) existe? (2) é do tipo certo? (3) respeita as regras? Só
depois se lê `valuestring`, que é `NULL` num campo ausente — o segfault
clássico. Números: o cJSON guarda-os como `double`, e `409.5` tem
`valueint == 409`; compara-se um com o outro para recusar frações.

### Encode — `PrintUnformatted`, nunca `Print`

`cJSON_Print()` mete `\n` e indentação, e um `\n` a meio parte a mensagem em
duas no framing do outro lado. `"type"` é acrescentado primeiro para os bytes
serem **iguais aos exemplos do slide** (#31) — a ordem não importa a quem lê,
mas assim qualquer grupo que siga o slide lê-nos de certeza.

### Memória — uma saída por função

Cada função tem um único `out:` com o único `cJSON_Delete()` (e `cJSON_free()`
no encode). Todos os erros fazem `goto out`. Com vários `return` espalhados,
basta um esquecido para uma fuga que só aparece com o servidor a correr horas.

---

## 4. Os testes

| # | O que provoca | Esperado |
|---|---|---|
| 11 | `NICK`, `MSG` válidos, chaves por outra ordem | parse OK |
| 12 | campo obrigatório ou `type` em falta | 400, sem segfault; `*out` a zeros |
| 13 | `text` numérico, `nick` null, `type` 5, `text` array | 400 |
| 14 | não-JSON, objeto por fechar, linha vazia | 400 `invalid JSON` |
| 15 | `XPTO`, `"nick"` minúsculo, `LIST` | 400 `unknown type` |
| 16 | campos extra, também aninhados | ignorados |
| 18 | `"` e `\` no texto | escapados no encode, recuperados no parse |
| 25 | `{...}{...}`, lixo a seguir; espaços à volta | 400 / aceites |
| 26 | limites dos dois lados; vazio, espaço, `\t`, `\n`; UTF-8 | conforme §2.2 |
| **27** | **pior CHAT possível** | ≤ 4096, um só `\n`, volta intacto |
| 28 | raiz array / string / null / número | 400 |
| 29 | `'\0'` embebido | 400 |
| 30 | `OK`, `CHAT`, `ERROR` (o que o nosso cliente lê) | defaults, tipos, frações |
| **31** | **encode dos exemplos do slide 124** | bytes exatamente iguais |
| 32 | `"Type"`, `"Text"` | 400 — chaves case-sensitive |

Os testes **nunca rebentam**, mesmo contra uma implementação errada: falham com
um relatório. (Os primeiros, contra os stubs, davam segfault — um `len - 1` com
`len == 0` dava a volta para `SIZE_MAX`. Corrigido com `sem_nl()`.)

---

## 5. Como correr

```sh
make test          # as duas suites
make test SAN=1    # ASan + UBSan + LeakSanitizer — apanha cJSON_Delete esquecidos
```

---

## 6. Testes de mutação

Um bug de cada vez, `make test SAN=1`:

| Bug introduzido | Apanhado por |
|---|---|
| esquecer `cJSON_Delete` no parse | LeakSanitizer |
| esquecer `cJSON_free` no encode | LeakSanitizer |
| `cJSON_Print` em vez de `PrintUnformatted` | #18, #27, #31 |
| aceitar lixo a seguir ao objeto | #25 |
| não verificar `'\0'` | #29 |
| `type` case-insensitive | #15 |
| aceitar códigos fracionários | #30 |
| `private` não-booleano aceite | #30 |
| encode sem validar | #27 |
| nick com espaços | #26 |
| limite com off-by-one | #26 |
| chaves case-insensitive | **ninguém** → criado o #32 |
| não limpar `*out` ao falhar | **ninguém** → corrigido o #12 |

O #12 verificava a limpeza num caminho que falha **antes** de o `type` ser
escrito — passava sempre. Agora verifica depois de `{"type":"MSG"}`, em que o
tipo já foi reconhecido.

---

## 7. Limitações conhecidas

- `"\u0000"` dentro de uma string JSON: o cJSON descodifica-o e a string fica
  cortada nesse ponto (`"ana\u0000x"` → `"ana"`). Não é um risco no M1; no M3,
  com nicks únicos, continua a ser simplesmente `"ana"`.
- Os limites da §2.2 são **propostas** até serem acordados com os outros grupos
  (D4 no `PROTOCOL.md`).
