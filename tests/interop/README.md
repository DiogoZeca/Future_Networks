# Cenários de interoperabilidade

Ficheiros de texto independentes da linguagem: qualquer grupo pode corrê-los
contra qualquer servidor (com `nc` ou com `scripts/`).

Correr: `python3 scripts/interop.py <host> <porta>` (ver `scripts/README.md`).

Formato:

```
# 01_nick_ok.txt
> {"type":"NICK","nick":"ana"}
< {"type":"OK"}
```

- `>` linha enviada pelo cliente
- `<` linha esperada do servidor, comparada como JSON e como **subconjunto**:
  os campos escritos têm de existir com aquele valor, campos a mais são
  ignorados (`{"type":"ERROR","code":400}` passa com qualquer `reason`)
- `# ` comentário
- cada cenário corre em ligações novas

Desde o M3 (o 409 e o `PRIV` precisam de dois clientes):

- `a>`, `b<`, ... — a letra diz **que ligação**. Cada uma abre na primeira vez
  que aparece, pela ordem do cenário. Sem letra é a ligação por omissão.
- `< CLOSED` — espera que o servidor feche essa ligação (`QUIT`).
- Quando se espera algo que **não** é um `EVENT`, os `EVENT` que aparecerem
  pelo caminho são saltados: quem recebe os avisos de entrada e saída é
  semântica de cada grupo (D10) e não deve fazer falhar um cenário de `PRIV`.

```
# 07_nick_em_uso.txt (excerto)
a> {"type":"NICK","nick":"ana"}
a< {"type":"OK"}
b> {"type":"NICK","nick":"ana"}
b< {"type":"ERROR","code":409}
```

| Cenário | O que verifica |
|---|---|
| `01_nick_ok` | `NICK` → `OK` (D6) |
| `02_msg_antes_de_nick` | pedido antes do `NICK` → `ERROR 401`; a ligação continua |
| `03_msg_chat` | `MSG` → `CHAT` ao próprio emissor (D5) |
| `04_json_invalido` | não-JSON → `ERROR 400`; a ligação continua |
| `05_campos_extra` | campos desconhecidos ignorados; `from` do cliente não conta |
| `06_utf8_e_escapes` | UTF-8 partido entre `recv()` (no modo byte a byte) e escapes |
| `07_nick_em_uso` | 409, também com outras maiúsculas (D18); depois do 409 entra com outro nick (D19) |
| `08_priv` | o emissor recebe o `OK`, o destino o privado; nem o emissor (sem cópia, D14) nem um terceiro recebem o `CHAT` |
| `09_priv_inexistente` | `PRIV` para quem não está → 404 (D15); a ligação continua |
| `10_list` | `USERS` com quem pediu, ordenado, sem quem ainda não fez `NICK` (D16) |
| `11_quit` | `QUIT` → `OK` + fecho; os outros veem `"rui left"` (D17) |
| `12_m3_antes_de_nick` | `LIST`/`PRIV` antes do `NICK` → 401; `QUIT` → `OK` + fecho |
| `13_varias_linhas` | `MSG` com `\n` e `\t` → `OK` + `CHAT` igual; texto vazio → 400 |
| `14_text_too_long` | 4096 bytes passam; 4097 → 400 `"text too long"`, e a ligação continua |

Desde a revisão do M3 (acordo com o grupo 1) todo o pedido aceite recebe `OK`
antes do `CHAT` (D13), por isso os cenários com `MSG`/`PRIV` esperam primeiro
o `OK`.

Os cenários dependem das decisões do `PROTOCOL.md`. As que estão **AGREED**
com um grupo (`OK`, eco do `MSG`, sem cópia do `PRIV`, limites) devem passar
contra o servidor desse grupo; as que ainda são **PROPOSTA** (ex.: maiúsculas
no 07, a ordem do `USERS` no 10) podem falhar por uma decisão diferente e não
por um bug. O [`docs/DECISIONS.md`](../../docs/DECISIONS.md) diz qual.
