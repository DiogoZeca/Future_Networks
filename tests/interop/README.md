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
- cada cenário corre numa ligação nova

| Cenário | O que verifica |
|---|---|
| `01_nick_ok` | `NICK` → `OK` (D6) |
| `02_msg_antes_de_nick` | pedido antes do `NICK` → `ERROR 401`; a ligação continua |
| `03_msg_chat` | `MSG` → `CHAT` ao próprio emissor (D5) |
| `04_json_invalido` | não-JSON → `ERROR 400`; a ligação continua |
| `05_campos_extra` | campos desconhecidos ignorados; `from` do cliente não conta |
| `06_utf8_e_escapes` | UTF-8 partido entre `recv()` (no modo byte a byte) e escapes |

Os cenários 01–03 dependem de D5/D6 e o 02 do código 401 — enquanto estiverem
*TO AGREE* no `PROTOCOL.md`, uma falha contra outro grupo pode ser uma decisão
diferente e não um bug.
