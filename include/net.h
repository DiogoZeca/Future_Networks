/*
 * net.h — funções de sockets partilhadas por cliente e servidor: TCP para o
 * chat e, desde o M5, UDP para a descoberta (disco.h).
 */
#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>   /* struct sockaddr_in (broadcast_targets) */

/* Cria um socket à escuta em todas as interfaces, IPv4 E IPv6 (M3, slide 85):
 * um socket IPv6 com IPV6_V6ONLY=0 ("::"), que também aceita clientes IPv4.
 * Numa máquina sem IPv6 cai para IPv4 só ("0.0.0.0") e avisa no log.
 * Devolve o fd ou -1. */
int tcp_listen(const char *port, int backlog);

/* Liga a <host>:<port> (nome ou IP). Devolve o fd ou -1. */
int tcp_connect(const char *host, const char *port);

/* Envia os len bytes todos (send() pode enviar só parte). 0 ou -1.
 * Bloqueia o tempo que for preciso: usado pelo CLIENTE, que só escreve para
 * um peer (o servidor). */
int send_all(int fd, const void *buf, size_t len);

/* Como send_all(), mas com um PRAZO TOTAL de timeout_ms para enviar tudo.
 * Usado pelo SERVIDOR, que não pode ficar preso por um cliente que não lê.
 *
 *   0   enviado tudo
 *  -1   erro do socket (EPIPE, ECONNRESET...): o peer já foi
 *  -2   prazo esgotado: o peer não está a ler
 *
 * O prazo é para a linha INTEIRA e não por send(): um peer que vá libertando
 * uns bytes de vez em quando não consegue prolongar a espera (com SO_SNDTIMEO,
 * que é por chamada, conseguia).
 *
 * ATENÇÃO (typical-problems.md §1.3): com -1 ou -2 pode já ter saído PARTE da
 * linha. O stream desse peer fica corrompido sem remédio, por isso quem chama
 * TEM de deixar de escrever nesse socket e fechar a ligação. */
#define SEND_TIMEOUT_EXPIRED (-2)
int send_all_timeout(int fd, const void *buf, size_t len, int timeout_ms);

/* M4 — esperar por bytes com prazo, e acordar se o servidor encerrar.
 *
 * Espera que `fd` tenha alguma coisa para ler (bytes, FIN ou erro) até
 * timeout_ms (< 0 = sem prazo), ou que `stop_fd` fique legível (o pipe que o
 * servidor usa no Ctrl+C para acordar todas as threads; -1 = nenhum).
 *   NET_READY    fd legível: o recv() a seguir não bloqueia
 *   NET_TIMEOUT  passou o prazo sem nada
 *   NET_STOP     stop_fd legível (tem prioridade sobre o fd)
 *   -1           erro do poll()
 * Um sinal (EINTR) não acorda quem chama: o prazo continua a contar. */
#define NET_TIMEOUT 0
#define NET_READY   1
#define NET_STOP    2
int wait_readable(int fd, int stop_fd, long timeout_ms);

/* M4 — fechar sem que um RST engula a última resposta.
 *
 * Se fecharmos um socket com bytes do peer ainda por ler, o kernel manda RST
 * em vez de FIN, e o RST pode fazer o peer deitar fora o que lhe tínhamos
 * acabado de enviar (o ERROR 413, o OK ao QUIT, o 408...) antes de o ler
 * (M0 nota 4, typical-problems §2.2). Por isso: shutdown(SHUT_WR) (o nosso
 * FIN segue DEPOIS do que já está no buffer de envio) e ler e deitar fora o
 * que o peer mandar até ele fechar também, ou até timeout_ms.
 *   0  o peer fechou (o fecho final já não tem nada por ler: sai FIN)
 *   1  passou o prazo (o close() ainda pode sair como RST)
 *  -1  erro (o peer já tinha ido: RST)
 * NÃO fecha o fd: isso é de quem é dono dele (o refcount, no servidor). */
int linger_drain(int fd, long timeout_ms);

/* recv() que repete se for interrompido por um sinal (EINTR).
 * >0 bytes recebidos | 0 o outro lado fechou | -1 erro. */
ssize_t recv_retry(int fd, void *buf, size_t len);

/* Escreve "ip:porta" de um endereço em out: "1.2.3.4:5000" para IPv4 (e para
 * IPv4 que chegue por um socket dual-stack, ::ffff:1.2.3.4), "[::1]:5000"
 * para IPv6. */
void addr_to_str(const struct sockaddr *sa, char *out, size_t outlen);

/* Só o IP, sem a porta e sem parênteses retos: "1.2.3.4" (também quando
 * chega como ::ffff:1.2.3.4) ou "::1"; "?" se a família não for IP. É o que
 * a descoberta (M5) guarda de cada resposta: o IP de ORIGEM do datagrama. */
void addr_ip(const struct sockaddr *sa, char *out, size_t outlen);

/* A porta local de um socket (depois do bind, ou do 1.º envio), ou -1. Para
 * quem fez bind à porta "0" (o kernel escolhe: os testes) e para o servidor
 * anunciar a porta TCP em que REALMENTE está à escuta (a porta pode ter sido
 * dada como nome de serviço, "http"). */
int sock_port(int fd);

/* ===== M5 — UDP, para a descoberta ======================================
 *
 * Só IPv4 (D32): o slide pede um BROADCAST, e o IPv6 não tem broadcast — o
 * equivalente é o multicast para ff02::1 (todos os nós da ligação), que
 * pede outra API (IPV6_JOIN_GROUP). Fica documentado no M5.md.
 *
 * O que um datagrama tem de diferente de uma linha TCP: chega INTEIRO ou não
 * chega (nada de partido nem colado), pode perder-se sem ninguém saber, e
 * cada um traz o endereço de quem o mandou — que é para onde se responde.
 */

/* Socket UDP à escuta em 0.0.0.0:<port> (o servidor da descoberta). Tem de
 * ser 0.0.0.0 e não o IP de uma interface: em Linux um socket ligado a um IP
 * unicast NÃO recebe broadcasts.
 *
 * Com SO_REUSEADDR, para dois servidores na MESMA máquina poderem ambos usar
 * a 5001. Em Linux um broadcast é entregue a TODOS os sockets assim ligados à
 * porta, e um unicast só a UM deles (test_net #103): por isso a descoberta
 * por broadcast encontra os dois, e um DISCOVER mandado diretamente a esse IP
 * só encontra um. Um socket que já lá esteja SEM SO_REUSEADDR (outro
 * programa) impede o bind: EADDRINUSE.
 *
 * Devolve o fd, ou -1 com o motivo em *why (pode ser NULL), sem escrever
 * nada: o servidor avisa no log e continua sem descoberta. port "0" = o
 * kernel escolhe (ver sock_port). */
int udp_bind(const char *port, const char **why);

/* Socket UDP do cliente, com SO_BROADCAST, numa porta qualquer (o kernel
 * escolhe-a no 1.º envio; as respostas voltam para ela). Sem SO_BROADCAST o
 * kernel recusa enviar para um endereço de broadcast (EACCES): é preciso
 * pedi-lo explicitamente, para um programa não inundar a rede por engano.
 * Devolve o fd ou -1 (errno). */
int udp_broadcast_socket(void);

/* Envia UM datagrama, sem nunca bloquear (MSG_DONTWAIT): com o buffer de
 * envio cheio o datagrama perde-se — como se perderia na rede, e quem usa
 * UDP já conta com isso. Um datagrama sai inteiro ou não sai (não há "send
 * parcial" como no TCP). 0 ou -1 (errno). */
int udp_send(int fd, const void *buf, size_t len,
             const struct sockaddr *to, socklen_t tolen);

/* Recebe UM datagrama, sem nunca bloquear (MSG_DONTWAIT; esperar é com o
 * wait_readable). `from` recebe o endereço de origem (pode ser NULL).
 *   >= 0           bytes do datagrama (0 é um datagrama vazio)
 *   NET_TRUNCATED  o datagrama era MAIOR do que cap: foi deitado fora
 *                  inteiro (o resto não fica para a leitura seguinte: em UDP
 *                  o que não cabe perde-se), e meio JSON não se aproveita
 *   -1             erro; EAGAIN = não havia nada */
#define NET_TRUNCATED (-2)
ssize_t udp_recv(int fd, void *buf, size_t cap, struct sockaddr_storage *from);

/* Os destinos de um DISCOVER, com a porta `port` (1..65535):
 *   1. 255.255.255.255 — o broadcast "limitado". Em Linux sai só pela
 *      interface da rota para esse endereço (normalmente a da rota por
 *      omissão): numa máquina com Wi-Fi + Ethernet + docker0 + VPN, só uma;
 *   2. o broadcast de CADA interface IPv4 ligada (IFF_UP, IFF_BROADCAST, não
 *      loopback) — o broadcast "dirigido": 192.168.1.255 para 192.168.1.0/24.
 *      É o que chega às outras.
 * Sem repetidos (duas interfaces na mesma rede dão o mesmo). Escreve até
 * `max` em out[] e devolve quantos; 0 se port ou max forem inválidos.
 *
 * A loopback fica de fora: o 127.255.255.255 também é um broadcast a sério
 * (só entre os programas desta máquina) e é o que os testes usam, mas quem o
 * quiser pede-o explicitamente (o cliente com -b). */
size_t broadcast_targets(int port, struct sockaddr_in *out, size_t max);

#endif
