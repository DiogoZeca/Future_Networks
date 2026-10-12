# third_party

C não tem JSON na biblioteca standard. Plano: usar **cJSON**
(https://github.com/DaveGamble/cJSON, licença MIT) — são só dois ficheiros
(`cJSON.c`, `cJSON.h`) copiados para aqui, sem dependências.

Porquê não escrever um parser à mão: outros grupos podem enviar `"ç"`,
`"\/"`, espaços entre tokens, chaves em qualquer ordem… tudo JSON válido que um
parser caseiro facilmente rejeita, quebrando a interoperabilidade.

> Confirmar com o professor que usar uma biblioteca de JSON em C é aceitável.
