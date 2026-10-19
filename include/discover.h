/*
 * discover.h — M5: o lado do CLIENTE da descoberta ("the client finds servers
 * on the LAN with no address typed in", slide 123).
 *
 * `./bin/client` sem endereço:
 *
 *   1. manda DISCO_REQUEST para cada destino — 255.255.255.255 e o broadcast
 *      de cada interface (broadcast_targets, net.h), ou os que se derem com -b;
 *   2. repete-o DISCOVER_TRIES vezes ao longo da espera, porque o UDP pode
 *      perder pacotes e ninguém avisa (nem quem envia nem quem recebe);
 *   3. junta as respostas durante wait_ms, sem repetidos (disco_list), com o
 *      IP de ORIGEM de cada uma (nunca um IP que venha na mensagem);
 *   4. mostra a lista e o utilizador escolhe (discover_choose): um servidor
 *      só → liga-se logo; nenhum → explica porquê pode ter falhado.
 *
 * Os bytes (o que é uma resposta, a lista, a linha mostrada) estão no disco.c
 * e os sockets no net.c, ambos testados à unidade. Aqui fica só a ordem das
 * coisas e o tempo — testado com servidores a sério (tests/system).
 */
#ifndef DISCOVER_H
#define DISCOVER_H

#include <stddef.h>

#include "disco.h"

#define DISCOVER_WAIT_MS    1500   /* -w: quanto tempo se esperam respostas   */
#define DISCOVER_TRIES      3      /* envios do pedido: a 0, 1/5 e 2/5 da espera */
#define DISCOVER_TARGETS_MAX 8     /* -b repetido, no máximo                   */
#define DISCOVER_DEST_MAX   16     /* destinos de broadcast_targets()          */

/* O broadcast da loopback: chega a todos os programas DESTA máquina. É o
 * recurso quando não há rede nenhuma (ver discover()), e o que os testes usam
 * com -b. */
#define DISCOVER_LOOPBACK   "127.255.255.255"

struct discover_opts {
    int         udp_port;                        /* 1..65535 (-u)             */
    long        wait_ms;                         /* > 0 (-w)                  */
    const char *targets[DISCOVER_TARGETS_MAX];   /* IPv4 em texto (-b), ou    */
    size_t      n_targets;                       /* 0 = broadcast_targets()   */
};

/* O que aconteceu, para o cliente explicar ao utilizador. */
struct discover_info {
    size_t n_dest;        /* para quantos destinos se mandou o pedido          */
    int    loopback_only; /* 1 = sem rede: só se procurou nesta máquina        */
    size_t dropped;       /* respostas de servidores que já não couberam       */
};

/* Procura servidores e deixa-os em *out (pela ordem de chegada). Demora
 * sempre wait_ms: não se sabe quantos servidores há, por isso não há "já
 * chegou a última resposta" — espera-se o tempo todo.
 *
 * Se não se conseguir mandar o pedido para NENHUM destino (uma máquina sem
 * rede: o 255.255.255.255 dá ENETUNREACH e não há interfaces) e não se deu
 * -b, tenta-se a loopback (DISCOVER_LOOPBACK): assim um servidor nesta mesma
 * máquina é encontrado na mesma — numa demonstração sem Wi-Fi, por exemplo —
 * e info->loopback_only diz que foi isso que aconteceu.
 *
 * Devolve 0 (a lista pode estar vazia), ou -1 se não houve nada a fazer: um
 * -b que não é um IPv4, nenhum socket, ou nenhum envio possível (errno). */
int discover(const struct discover_opts *o, struct disco_list *out,
             struct discover_info *info);

/* Mostra os servidores encontrados e devolve o índice (0..n-1) do escolhido:
 *   - 1 servidor:  mostra-o e escolhe-o, sem perguntar;
 *   - vários:      lista numerada e "escolhe um servidor [1-n]:" até ser
 *                  válido (ui_parse_choice);
 *   - 0, ou EOF no teclado: -1.
 * Lê do stdin com fgets(), como o pedido do nick: é antes do term_start(). */
int discover_choose(const struct disco_list *l);

#endif /* DISCOVER_H */
