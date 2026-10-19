/*
 * roster.h — a lista de clientes ligados, partilhada por todas as threads.
 *
 * É a única estrutura do servidor que mais do que uma thread toca, por isso é
 * aqui que vivem todos os bugs de concorrência do M2. As regras abaixo não são
 * sugestões: são o que torna o broadcast correto.
 *
 *
 * O PROBLEMA QUE O REFCOUNT RESOLVE
 * ---------------------------------
 * O broadcast tem de enviar FORA do lock. Se enviasse com o lock agarrado, um
 * único cliente que não lê (portátil suspenso, Ctrl+S no terminal) encheria o
 * buffer de envio, o send() bloquearia, e o chat inteiro parava — é o problema
 * §2.4 / §3.2 do docs/typical-problems.md.
 *
 * Mas enviar fora do lock abre uma janela: entre tirar o retrato da lista e
 * enviar, o cliente pode sair, o struct ser libertado e o fd fechado. Pior: o
 * kernel reutiliza o número do fd na ligação seguinte, e a mensagem iria para
 * o cliente ERRADO — sem crash, sem erro, sem forma de dar por isso.
 *
 * Daí a contagem de referências: roster_snapshot() incrementa `refs` de cada
 * cliente DENTRO do lock, e quem envia larga a referência no fim. O fd só é
 * fechado e a memória só é libertada quando `refs` chega a zero. A janela
 * deixa de existir.
 *
 * O QUE O REFCOUNT NÃO RESOLVE
 * ----------------------------
 * Enviar fora do lock impede que um cliente lento bloqueie o ROSTER (entradas,
 * saídas, retratos). Não impede que bloqueie quem lhe ESCREVE: a thread que
 * está no send() para ele fica presa, e as outras ficam na fila do wlock dele.
 * Num chat com broadcast isso é toda a gente. Isso resolve-se no envio, não
 * aqui: prazo em cada envio e `dead` (src/server/main.c, regra 4).
 *
 *
 * QUEM PROTEGE O QUÊ
 * ------------------
 *   roster.lock   a lista (v[], n) e, em cada cliente, `nick`, `joined`,
 *                 `refs` e `listed`.
 *   client.wlock  a escrita no socket desse cliente. Serializa os send_all()
 *                 de threads diferentes: cada send() é atómico, mas o CICLO do
 *                 send_all() não é, e duas threads a escrever ao mesmo tempo
 *                 no mesmo fd intercalam metade de uma mensagem com metade de
 *                 outra (§3.3). O cliente receberia JSON corrompido.
 *   _Atomic       `dead`: posto a 1 pela primeira thread cujo envio falhe ou
 *                 expire. Atómico e não protegido por lock porque é lido por
 *                 todas as threads antes de esperarem pelo wlock.
 *   ninguém       fd e who: escritos uma vez no roster_add() e só lidos depois.
 *                 O broadcast lê o `fd` de outros, o que é seguro porque não
 *                 muda durante a vida do cliente e o refcount garante que não
 *                 é fechado enquanto houver referências.
 *
 * ORDEM DOS LOCKS: nunca segurar roster.lock e um client.wlock ao mesmo tempo.
 * O broadcast larga o primeiro antes de pegar no segundo. Enquanto esta regra
 * se mantiver, não há deadlock possível — há só dois locks e nunca se aninham.
 *
 *
 * O CICLO DE VIDA DE UM CLIENTE
 * -----------------------------
 *   roster_add()        refs = 1   (a referência da LISTA)
 *   thread dona          +1        (quem chama add fica com uma referência)
 *   roster_snapshot()    +1 por cada cliente do retrato
 *   client_release()     -1        ... e a zero: close(fd) + free()
 *   roster_remove()      tira da lista e larga a referência DA LISTA
 *
 * Regra prática: cada roster_snapshot() tem de ter tantos client_release()
 * quantos os clientes que devolveu. Um release a menos é uma fuga; um a mais é
 * um use-after-free. O `make test SAN=1` apanha ambos.
 */
#ifndef ROSTER_H
#define ROSTER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

#include "protocol.h"

/* Limite de clientes em simultâneo. Array fixo em vez de lista ligada: o
 * servidor é de uma turma, não de uma rede social, e um array torna o retrato
 * um memcpy em vez de um passeio por ponteiros com o lock agarrado. */
#define ROSTER_MAX 64

/* Um USERS (M3) leva os nicks de todos: tem de caber no que o protocolo
 * aceita enviar numa linha (protocol.h, D20). */
_Static_assert(ROSTER_MAX <= PROTO_USERS_MAX,
               "um USERS com toda a gente tem de caber numa linha");

struct client {
    /* --- da thread dona; o broadcast só lê o fd --- */
    int  fd;                          /* -1 é válido (usado nos testes)       */
    char who[64];                     /* "ip:porta", para os logs             */

    /* --- serializa as escritas neste socket --- */
    pthread_mutex_t wlock;

    /* --- 1 = um envio falhou ou expirou: não voltar a tentar. Atómico
     *     porque é lido por todas as threads que fazem broadcast. --- */
    _Atomic int dead;

    /* --- protegidos por roster.lock --- */
    char nick[PROTO_NICK_MAX + 1];    /* "" até o NICK ser aceite; reservado  */
                                      /* pelo roster_claim() (M3)             */
    int  joined;                      /* 1 = está no chat (ver roster_join)   */
    int  refs;                        /* 0 => fechar o fd e libertar          */
    int  listed;                      /* 1 enquanto estiver em v[]            */
};

struct roster {
    pthread_mutex_t lock;
    struct client  *v[ROSTER_MAX];
    size_t          n;
};

/* ---- Vida do roster ----------------------------------------------------- */

void roster_init(struct roster *r);

/* Liberta todos os clientes que ainda estejam na lista, ignorando o refcount.
 * Só para o fim do programa e para os testes: se houver um broadcast a decorrer
 * noutra thread, isto é um use-after-free. */
void roster_destroy(struct roster *r);

/* ---- Entrada e saída ---------------------------------------------------- */

/* Cria um cliente e põe-no na lista.
 *
 * Devolve o cliente com refs = 2: uma referência da lista e outra para quem
 * chamou (a thread dona). Devolve NULL se a lista estiver cheia ou faltar
 * memória — nesse caso o chamador fecha o fd e recusa a ligação. */
struct client *roster_add(struct roster *r, int fd, const char *who);

/* Tira o cliente da lista e larga a referência DA LISTA.
 *
 * O cliente não é necessariamente libertado aqui: se um broadcast ainda o tiver
 * no retrato, só desaparece quando esse largar a referência dele. A partir
 * deste momento deixa de aparecer em roster_snapshot().
 *
 * Chamar duas vezes é seguro (a segunda não faz nada). */
void roster_remove(struct roster *r, struct client *c);

/* Marca o cliente como participante do chat e guarda o nick dele.
 *
 * Entre o accept() e o NICK aceite, um cliente ESTÁ na lista mas ainda não faz
 * parte do chat: está a meio do handshake aplicacional. Até esta chamada não
 * aparece em roster_snapshot(), logo não recebe EVENT nem CHAT.
 *
 * Sem isto, um cliente que acabou de ligar recebia os EVENT de quem entrasse
 * entretanto, e a primeira linha que via não era o OK ao seu NICK mas o aviso
 * de outra pessoa. O nosso cliente aguenta; um de outro grupo que espere o OK
 * como primeira resposta parte-se — e é precisamente o tipo de divergência que
 * só aparece no dia da interoperabilidade.
 *
 * A ORDEM conta: o servidor chama isto DEPOIS de o OK ter sido enviado, não
 * antes. Chamado antes, abria-se uma janela em que um broadcast de outra
 * thread chegava ao cliente antes do OK (ver o ciclo de saídas em
 * src/server/main.c).
 *
 * Desde o M3 o nick já foi reservado antes, pelo roster_claim() (é aí que se
 * garante que é único); o servidor passa aqui o MESMO nick. O join não volta
 * a verificar nada: só torna o cliente visível.
 *
 * Idempotente. */
void roster_join(struct roster *r, struct client *c, const char *nick);

/* ---- Nicks (M3) --------------------------------------------------------- */

/* Reserva `nick` para o cliente c, SE mais ninguém na lista o tiver (D18:
 * comparação proto_nick_eq, sem olhar a maiúsculas). Devolve 1 se reservou,
 * 0 se já está em uso (-> ERROR 409).
 *
 * Verificar e reservar acontecem DENTRO do mesmo lock. Separados ("existe?" e
 * depois "guarda"), dois clientes a pedir "ana" ao mesmo tempo passavam os
 * dois na verificação antes de qualquer um guardar — e ficavam duas "ana".
 * É o test_roster #18 (16 threads, o mesmo nick, 200 rondas).
 *
 * Conta como "em uso" quem já reservou e ainda não entrou no chat (está entre
 * o NICK aceite e o roster_join): esse vai entrar a seguir. O nick fica livre
 * quando o dono sai da lista (roster_remove). */
int roster_claim(struct roster *r, struct client *c, const char *nick);

/* Procura alguém NO CHAT (joined) com este nick (proto_nick_eq). Se existir,
 * copia o nick tal como foi registado para canon (pode ser NULL) e devolve 1;
 * senão 0. Não guarda referência nenhuma: no instante seguinte o cliente pode
 * já ter saído — serve para decidir o 404, não para enviar. */
int roster_lookup(struct roster *r, const char *nick, char canon[PROTO_NICK_MAX + 1]);

/* Como roster_lookup(), mas devolve o cliente com +1 referência, para lhe
 * enviar. O chamador TEM de chamar client_release(). NULL se não existir. */
struct client *roster_find(struct roster *r, const char *nick);

/* Copia para out[] os nicks de quem está NO CHAT (joined), pela ordem da
 * lista (sem significado). Devolve quantos (no máximo cap). A ordem do USERS
 * é decisão da session (D16), não daqui. */
size_t roster_nicks(struct roster *r, char (*out)[PROTO_NICK_MAX + 1], size_t cap);

/* Larga UMA referência. A zero: fecha o fd (se >= 0), destrói o wlock e liberta
 * a memória. Depois desta chamada o ponteiro pode já não ser válido. */
void client_release(struct roster *r, struct client *c);

/* ---- Broadcast ---------------------------------------------------------- */

/* Copia para out[] os clientes que JÁ ENTRARAM no chat (ver roster_join),
 * incrementando o refcount de cada um. Devolve quantos copiou (no máximo
 * `cap`). Quem ainda não fez NICK fica de fora.
 *
 * O CHAMADOR TEM DE CHAMAR client_release() EM CADA UM. Sem isso, nenhum
 * cliente é libertado e o servidor fica a segurar fds para sempre.
 *
 * Se `except` não for NULL, esse cliente fica de fora do retrato — serve para
 * os EVENT de entrada e saída, que não interessam a quem os provoca. */
size_t roster_snapshot(struct roster *r, struct client **out, size_t cap,
                       const struct client *except);

/* ---- Leitura ------------------------------------------------------------ */

/* Quantos clientes estão na lista, INCLUINDO os que ainda não fizeram NICK.
 * Para logs e testes: no instante a seguir já pode ser outro número, por isso
 * nunca decidir nada com base nisto. */
size_t roster_count(struct roster *r);

/* M5: quantos estão NO CHAT (joined) — o "users" da resposta ao DISCOVER
 * (disco.h, D29). Não conta quem ainda não fez NICK: esses ainda não são
 * utilizadores, e um port scan ou um `nc` calado não deve aparecer como
 * gente na sala. O mesmo aviso do roster_count(): é um retrato. */
size_t roster_users(struct roster *r);

#endif /* ROSTER_H */
