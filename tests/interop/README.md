# Cenários de interoperabilidade

Ficheiros de texto independentes da linguagem: qualquer grupo pode corrê-los
contra qualquer servidor (com `nc` ou com `scripts/`).

Formato proposto:

```
# 01_nick_ok.txt
> {"type":"NICK","nick":"ana"}
< {"type":"OK"}
```

- `>` linha enviada pelo cliente
- `<` linha esperada do servidor (comparar como JSON, não como texto)
- `# ` comentário
