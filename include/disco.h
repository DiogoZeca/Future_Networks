/*
 * disco.h — M5: descobrir servidores na LAN por UDP (slide 123: "The server
 * answers a UDP broadcast (DISCOVER) with its TCP port; the client finds
 * servers on the LAN with no address typed in").
 *
 * Como o framing e o protocol, esta camada NÃO toca em sockets: recebe e
 * devolve os bytes de um datagrama. Os sockets UDP (bind, SO_BROADCAST, os
 * endereços de broadcast de cada interface) ficam no net.c e a canalização
 * nos main.c — por isso tudo o que aqui está se testa à unidade.
 *
 *   cliente                                       servidor (UDP 5001)
 *   {"type":"DISCOVER"}  ---- broadcast ---->     disco_is_request()
 *   disco_parse_reply()  <--- só para ele ---     disco_build_reply()
 *     + o IP de ORIGEM do datagrama (recvfrom)
 *   disco_list_add() / disco_format()  ->  lista para o utilizador escolher
 *
 *
 * O FORMATO (D28, D29 — PROPOSAL, PROTOCOL.md)
 * --------------------------------------------
 *   pedido:    {"type":"DISCOVER"}                      (ou o texto DISCOVER)
 *   resposta:  {"type":"SERVER","tcp_port":5000,"name":"diogolap","users":3}\n
 *
 *   tcp_port  a porta TCP do chat (o que o slide pede)
 *   name      o nome do servidor: o hostname, ou o que se der com -N
 *   users     quantos nicks estão no chat (não conta quem ainda não fez NICK)
 *
 * Um datagrama UDP já É uma mensagem: o kernel entrega-o inteiro ou não o
 * entrega. Por isso aqui não há framing — nada chega partido nem colado (ao
 * contrário do TCP, slide 125). O '\n' no fim da resposta não faz falta
 * nenhuma; vai lá para quem reaproveitar o leitor de linhas do TCP. Aceitamos
 * os dois, com e sem.
 *
 *
 * ESTRITO A ENVIAR, TOLERANTE A RECEBER (como no resto do protocolo)
 * -----------------------------------------------------------------
 * O SERVIDOR responde a:
 *   - {"type":"DISCOVER"}, com ou sem campos a mais (A4);
 *   - o texto simples DISCOVER (para um grupo que não use JSON nisto);
 * com ou sem espaços, '\r' e '\n' à volta. Os nomes contam maiúsculas
 * (DECISIONS.md S3): "discover" não é um pedido.
 *
 * E a NADA mais — lixo, outros tipos, UTF-8 inválido, '\0', datagramas
 * maiores do que DISCO_DGRAM_MAX: silêncio, nem sequer um ERROR. Porquê:
 *   - um servidor que respondesse a tudo responderia também à RESPOSTA de
 *     outro servidor (um bug, ou um pedido com o IP de origem falsificado),
 *     e os dois ficavam a responder um ao outro para sempre;
 *   - a resposta é maior do que o pedido: responder a qualquer coisa fazia
 *     do servidor um amplificador de tráfego contra um IP falsificado.
 *
 * O CLIENTE aceita qualquer objeto JSON com a porta em "tcp_port" (o nosso
 * formato e o do grupo 1) ou em "port" (o de outros grupos), com "type"
 * ausente ou "SERVER"; "name" e "users" são opcionais e, se vierem mal, são
 * ignorados sem deitar fora a resposta (sem porta é que não serve de nada).
 *
 * O IP NÃO vem da mensagem: quem chama usa o IP de ORIGEM do datagrama. Um
 * servidor com várias interfaces (Wi-Fi, Ethernet, docker0, VPN) não sabe
 * qual é "o seu IP" para quem pergunta; o pacote que chegou ao cliente sabe —
 * veio por um caminho que funciona (typical-problems.md §4.4). Um campo "ip"
 * ou "host" na resposta é ignorado de propósito.
 */
#ifndef DISCO_H
#define DISCO_H

#include <stddef.h>

/* Porta UDP por omissão (D28: a do grupo 1). O servidor muda-a com -u e o
 * cliente também; os testes usam portas livres. */
#define DISCO_PORT_DEFAULT "5001"

/* O pedido que o NOSSO cliente envia: 19 bytes, sem '\n' — a forma mais
 * literal, que qualquer servidor lê (json.loads aceita-o tal e qual, e quem
 * faz strip() também). Cada datagrama é uma mensagem: não precisa de '\n'. */
#define DISCO_REQUEST "{\"type\":\"DISCOVER\"}"

/* O maior datagrama aceite, nos dois sentidos. O nosso pedido tem 19 bytes e
 * a nossa maior resposta 192 (ver abaixo); 512 dá margem para os campos a
 * mais de outros grupos. É o mesmo número do DNS sobre UDP, pela mesma razão:
 * 512 + 8 (UDP) + 20 (IP) = 540 <= 576, o datagrama que qualquer máquina IPv4
 * tem de conseguir receber (RFC 791). Um datagrama maior é ignorado inteiro:
 * quem o lê com um buffer mais pequeno recebe-o CORTADO (MSG_TRUNC), e meio
 * JSON não se aproveita. */
#define DISCO_DGRAM_MAX  512

/* "name": até 64 bytes (o HOST_NAME_MAX do Linux também é 64). */
#define DISCO_NAME_MAX   64

/* O pior caso da nossa resposta — porta 65535, users no máximo de um int e um
 * name de 64 bytes só de '"' ou '\\', que o JSON escapa com 2 bytes cada:
 *   {"type":"SERVER","tcp_port":65535,"name":"","users":2147483647}\n
 *   = 64 bytes fixos + 2 * 64 = 192
 * Sem caracteres de controlo no name (disco_valid_name), nada cresce mais. */
#define DISCO_REPLY_OVERHEAD 64
_Static_assert(DISCO_REPLY_OVERHEAD + 2 * DISCO_NAME_MAX <= DISCO_DGRAM_MAX,
               "a nossa pior resposta tem de caber num datagrama que aceitamos");

/* Tamanho do out[] de disco_build_reply(): um datagrama e o '\0'. */
#define DISCO_REPLY_BUF  (DISCO_DGRAM_MAX + 1)

/* Quantos servidores diferentes o cliente guarda numa procura. Numa sala de
 * aula, um por grupo; o resto é ignorado (e dito ao utilizador). */
#define DISCO_SERVERS_MAX 32

/* Um IP em texto: IPv4 "255.255.255.255" ou IPv6 com ::ffff:a.b.c.d. É o
 * INET6_ADDRSTRLEN (46), sem obrigar quem inclui isto a incluir <netinet/in.h>
 * (o disco.c verifica que são iguais). */
#define DISCO_IP_BUF     46

/* ---- Servidor ----------------------------------------------------------- */

/* Este datagrama é um pedido de descoberta? 1 = sim (responder); 0 = não
 * (ignorar em silêncio — ver acima). `len` é o que o recvfrom() devolveu; o
 * buffer pode ter '\0' embebidos (é recusado) e não precisa de terminar em
 * '\0'. */
int disco_is_request(const char *buf, size_t len);

/* O name que o servidor anuncia é válido? 1..DISCO_NAME_MAX bytes, UTF-8
 * válido, sem caracteres de controlo (< 0x20 e 0x7F). O servidor verifica o
 * -N com isto ao arrancar; o hostname normalmente já cumpre. */
int disco_valid_name(const char *name);

/* Monta a resposta: JSON compacto + '\n', terminado em '\0' (que NÃO é para
 * enviar). As chaves vão sempre pela mesma ordem (type, tcp_port, name,
 * users), como no exemplo acima.
 *
 *   tcp_port  1..65535       name  disco_valid_name()     users  >= 0
 *   out       pelo menos DISCO_REPLY_BUF bytes chegam sempre
 *   out_len   bytes a enviar, INCLUINDO o '\n'. Pode ser NULL.
 *
 * Devolve 0, ou -1 se os valores não forem válidos, se out/cap não chegarem
 * ou se o cJSON falhar a alocar — sempre "culpa nossa", nunca do peer. */
int disco_build_reply(int tcp_port, const char *name, int users,
                      char *out, size_t cap, size_t *out_len);

/* ---- Cliente ------------------------------------------------------------ */

/* Um servidor encontrado. */
struct disco_server {
    char ip[DISCO_IP_BUF];         /* de QUEM CHAMA: o IP de origem do datagrama */
    int  tcp_port;                 /* 1..65535                                   */
    char name[DISCO_NAME_MAX + 1]; /* "" se não veio; já limpo para mostrar      */
    int  users;                    /* -1 se não veio (ou veio mal)               */
};

/* Lê uma resposta de um servidor (nosso ou de outro grupo).
 *
 * Devolve 0 e preenche tcp_port, name e users; ou -1 se não for uma resposta
 * aproveitável. Em ambos os casos *out é primeiro posto a zeros (users a -1),
 * e o `ip` fica SEMPRE vazio: é quem chama que o preenche, com o IP de origem.
 *
 * O name é limpo para se poder mostrar num terminal: cortado aos
 * DISCO_NAME_MAX bytes sem partir um caracter UTF-8 a meio, e cada caracter
 * de controlo trocado por '?' (um "\u001b[2J" num name limparia o ecrã de
 * quem procura — DECISIONS.md C10). */
int disco_parse_reply(const char *buf, size_t len, struct disco_server *out);

/* Os servidores encontrados numa procura, sem repetidos.
 *
 * O cliente manda o pedido 3 vezes (o UDP pode perder pacotes) e para vários
 * endereços de broadcast (um por interface) — por isso o mesmo servidor
 * responde várias vezes. "O mesmo" = mesmo IP de origem e mesma porta TCP:
 * dois servidores na mesma máquina, em portas diferentes, são dois; um
 * servidor que responde por duas interfaces aparece duas vezes, com os dois
 * IPs — e os dois funcionam.
 *
 * A lista está sempre ORDENADA: primeiro os que têm name, por name (sem
 * contar maiúsculas, como os nicks: proto_nick_cmp); depois os sem name; em
 * empate pelo IP (como número: 10.0.0.9 antes de 10.0.0.10) e pela porta.
 * Pela ordem de chegada, o mesmo servidor podia ser o 1 numa procura e o 2
 * na seguinte; assim, os mesmos servidores dão sempre os mesmos números. */
struct disco_list {
    struct disco_server s[DISCO_SERVERS_MAX];
    size_t              n;
};

void disco_list_init(struct disco_list *l);

/* Acrescenta `srv` à lista, no lugar certo da ordem. Devolve:
 *    1  novo
 *    0  repetido: atualiza o name e o users (e o lugar, se o name mudou)
 *   -1  lista cheia (DISCO_SERVERS_MAX), não ficou */
int disco_list_add(struct disco_list *l, const struct disco_server *srv);

/* Uma linha da lista que o cliente mostra, sem '\n':
 *     "1) 192.168.1.10:5000  diogolap  (3 online)"
 *     "2) 10.0.0.7:5000"                 (sem name nem users)
 *     "3) [fe80::1]:5000  sala"          (IPv6 entre parênteses retos)
 * `idx` é o número a mostrar (a partir de 1). Devolve 0, ou -1 se não
 * couber em cap (out fica com uma string vazia). */
int disco_format(const struct disco_server *srv, size_t idx, char *out, size_t cap);

#endif /* DISCO_H */
