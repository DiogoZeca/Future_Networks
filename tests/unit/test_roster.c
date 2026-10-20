/*
 * test_roster.c — testes da lista de clientes partilhada.
 *
 * Três famílias:
 *
 *   1-10   API sequencial, sem threads. Verificam o refcount, que é o que
 *          torna o broadcast fora do lock seguro.
 *   11-13  Concorrência a sério. São estes que justificam o milestone: um bug
 *          de lock não aparece com um cliente, aparece com cinco a entrar e
 *          sair ao mesmo tempo. Correr SEMPRE com `make test SAN=1` — é o
 *          AddressSanitizer que transforma "não deu crash" em prova. E com o
 *          ThreadSanitizer numa compilação à parte: o ASan NÃO vê data races.
 *   14     Quem ainda não fez NICK está ligado mas fora do chat. Nasceu de um
 *          bug que o scripts/probe_m2.py apanhou, não de uma ideia à partida.
 *   15-18  Nicks (M3): reserva única, procura por nick, lista de nicks, e a
 *          corrida de dois NICK iguais ao mesmo tempo (18, com threads).
 *   19     Quantos estão no chat (M5: o "users" da resposta ao DISCOVER).
 *
 * Os clientes de teste usam fd = -1 (o contrato permite), exceto o caso 9 que
 * precisa de um fd real para verificar que o close() acontece.
 *
 * Escritos ANTES da implementação (commit do contrato), contra stubs.
 */
#include "roster.h"
#include "test_util.h"

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

static struct roster R;

/* Um teste deve FALHAR, não crashar: um segfault não diz que caso falhou nem
 * em que linha. Enquanto roster_add() for um stub devolve NULL, e qualquer
 * acesso a seguir seria um segfault que apagava o relatório todo. Isto regista
 * a falha e abandona o caso. */
#define NEED(p) do {                          \
    CHECK((p) != NULL);                       \
    if ((p) == NULL) { roster_destroy(&R); return; } \
} while (0)

/* add + join: e o par que o servidor faz, o add no accept() e o join quando o
 * NICK e aceite. Quase todos os casos querem um cliente que JA esta no chat,
 * porque e esse que aparece no retrato. O caso 14 testa o estado intermedio. */
static struct client *add_joined(const char *who, const char *nick)
{
    struct client *c = roster_add(&R, -1, who);
    if (c != NULL)
        roster_join(&R, c, nick);
    return c;
}

/* ------------------------------------------------------------------------ */

/* 1. Roster vazio. */
static void caso_1_vazio(void)
{
    CASE(1, "roster vazio: 0 clientes, retrato vazio");
    struct client *snap[ROSTER_MAX];

    roster_init(&R);
    CHECK_INT(roster_count(&R), 0);
    CHECK_INT(roster_snapshot(&R, snap, ROSTER_MAX, NULL), 0);
    roster_destroy(&R);
}

/* 2. Um cliente entra e aparece no retrato. */
static void caso_2_entrar(void)
{
    CASE(2, "add: entra na lista e aparece no retrato");
    struct client *snap[ROSTER_MAX];

    roster_init(&R);
    struct client *c = add_joined("1.2.3.4:5000", "ana");
    NEED(c);
    CHECK_INT(roster_count(&R), 1);

    size_t n = roster_snapshot(&R, snap, ROSTER_MAX, NULL);
    CHECK_INT(n, 1);
    CHECK(n == 1 && snap[0] == c);
    CHECK_STR(c->who, "1.2.3.4:5000");
    CHECK_STR(c->nick, "ana");

    for (size_t i = 0; i < n; i++)
        client_release(&R, snap[i]);
    roster_remove(&R, c);
    client_release(&R, c);                     /* a referencia da thread dona */
    roster_destroy(&R);
}

/* 3. Sai e deixa de aparecer. */
static void caso_3_sair(void)
{
    CASE(3, "remove: sai da lista");
    struct client *snap[ROSTER_MAX];

    roster_init(&R);
    struct client *c = add_joined("a", "ana");
    NEED(c);
    roster_remove(&R, c);

    CHECK_INT(roster_count(&R), 0);
    CHECK_INT(roster_snapshot(&R, snap, ROSTER_MAX, NULL), 0);

    client_release(&R, c);
    roster_destroy(&R);
}

/* 4. O CASO CENTRAL: o retrato segura o cliente vivo.
 *
 *    Simula a corrida real — a thread A tira o retrato, a thread B sai, e a A
 *    ainda vai usar o ponteiro. Sem refcount isto seria use-after-free; o
 *    AddressSanitizer apanha-o se a implementação estiver errada. */
static void caso_4_retrato_segura_o_cliente(void)
{
    CASE(4, "o retrato mantem o cliente vivo depois de sair da lista");
    struct client *snap[ROSTER_MAX];

    roster_init(&R);
    struct client *c = add_joined("a", "ana");
    NEED(c);

    size_t n = roster_snapshot(&R, snap, ROSTER_MAX, NULL);  /* refs += 1 */
    CHECK_INT(n, 1);

    roster_remove(&R, c);          /* larga a ref da lista */
    client_release(&R, c);         /* larga a ref da thread dona */

    /* Ainda vivo: o retrato tem uma referencia. Ler daqui tem de ser seguro. */
    CHECK_INT(roster_count(&R), 0);
    if (n == 1) {
        CHECK_STR(snap[0]->who, "a");
        client_release(&R, snap[0]);   /* a ultima: so agora e libertado */
    }
    roster_destroy(&R);
}

/* 5. `except` tira o proprio do retrato (para os EVENT de entrada/saida). */
static void caso_5_except(void)
{
    CASE(5, "except exclui o proprio do retrato");
    struct client *snap[ROSTER_MAX];

    roster_init(&R);
    struct client *a = add_joined("a", "ana");
    NEED(a);
    struct client *b = add_joined("b", "rui");
    NEED(b);

    size_t n = roster_snapshot(&R, snap, ROSTER_MAX, a);
    CHECK_INT(n, 1);
    CHECK(n == 1 && snap[0] == b);
    for (size_t i = 0; i < n; i++)
        client_release(&R, snap[i]);

    n = roster_snapshot(&R, snap, ROSTER_MAX, NULL);        /* sem except */
    CHECK_INT(n, 2);
    for (size_t i = 0; i < n; i++)
        client_release(&R, snap[i]);

    roster_remove(&R, a); client_release(&R, a);
    roster_remove(&R, b); client_release(&R, b);
    roster_destroy(&R);
}

/* 6. O retrato respeita o `cap` do chamador. */
static void caso_6_cap(void)
{
    CASE(6, "snapshot nao escreve mais do que cap");
    struct client *snap[2];

    roster_init(&R);
    struct client *c[3];
    for (int i = 0; i < 3; i++) {
        c[i] = add_joined("x", "n");
        NEED(c[i]);
    }

    size_t n = roster_snapshot(&R, snap, 2, NULL);
    CHECK_INT(n, 2);                           /* truncado, nao estourado */
    for (size_t i = 0; i < n; i++)
        client_release(&R, snap[i]);

    for (int i = 0; i < 3; i++) { roster_remove(&R, c[i]); client_release(&R, c[i]); }
    roster_destroy(&R);
}

/* 7. Lista cheia: add recusa em vez de escrever fora do array. */
static void caso_7_cheio(void)
{
    CASE(7, "lista cheia: add devolve NULL");
    static struct client *c[ROSTER_MAX];

    roster_init(&R);
    for (int i = 0; i < ROSTER_MAX; i++) {
        c[i] = roster_add(&R, -1, "x");
        CHECK(c[i] != NULL);
    }
    CHECK_INT(roster_count(&R), ROSTER_MAX);
    CHECK(roster_add(&R, -1, "a mais") == NULL);

    for (int i = 0; i < ROSTER_MAX; i++) { roster_remove(&R, c[i]); client_release(&R, c[i]); }
    roster_destroy(&R);
}

/* 8. remove duas vezes nao faz estragos (o contrato promete-o). */
static void caso_8_remove_duas_vezes(void)
{
    CASE(8, "remove duas vezes e seguro");
    roster_init(&R);
    struct client *c = add_joined("a", "ana");
    NEED(c);

    roster_remove(&R, c);
    roster_remove(&R, c);                      /* nao larga a ref outra vez */
    CHECK_INT(roster_count(&R), 0);

    client_release(&R, c);                     /* a ultima */
    roster_destroy(&R);
}

/* 9. O fd e mesmo fechado quando a ultima referencia desaparece.
 *    Sem isto, o servidor esgota os descritores ao fim de ~1024 ligacoes. */
static void caso_9_fecha_o_fd(void)
{
    CASE(9, "a ultima referencia fecha o fd");
    int p[2];

    CHECK_INT(pipe(p), 0);
    roster_init(&R);
    struct client *c = roster_add(&R, p[1], "a");
    NEED(c);
    roster_join(&R, c, "ana");

    CHECK_INT(fcntl(p[1], F_GETFD) != -1, 1);  /* ainda aberto */
    roster_remove(&R, c);
    client_release(&R, c);                     /* refs -> 0 */
    CHECK_INT(fcntl(p[1], F_GETFD), -1);       /* fechado: EBADF */

    close(p[0]);
    roster_destroy(&R);
}

/* 10. roster_destroy limpa o que ficou para tras. */
static void caso_10_destroy(void)
{
    CASE(10, "destroy liberta os clientes que sobraram");
    roster_init(&R);
    roster_add(&R, -1, "a");
    roster_add(&R, -1, "b");
    CHECK_INT(roster_count(&R), 2);
    roster_destroy(&R);                        /* o ASan confirma que nao fugiu nada */
    CHECK(1);
}

/* ---- Concorrencia ------------------------------------------------------- */

#define NTHREADS 5
#define NLOOPS   200

/* 11. Entrar e sair em paralelo.
 *     Cada thread faz o ciclo de vida completo, muitas vezes. No fim a lista
 *     tem de estar exatamente vazia: se um lock estiver em falta, o `n` fica
 *     errado ou ha um use-after-free que o ASan apanha. */
static void *worker_entra_sai(void *arg)
{
    (void)arg;
    for (int i = 0; i < NLOOPS; i++) {
        struct client *c = roster_add(&R, -1, "t");
        if (c == NULL)
            continue;                          /* lista cheia: normal aqui */
        roster_join(&R, c, "t");
        roster_remove(&R, c);
        client_release(&R, c);
    }
    return NULL;
}

static void caso_11_entra_sai_em_paralelo(void)
{
    CASE(11, "5 threads a entrar e sair 200x: lista consistente");
    pthread_t th[NTHREADS];

    roster_init(&R);
    for (int i = 0; i < NTHREADS; i++)
        CHECK_INT(pthread_create(&th[i], NULL, worker_entra_sai, NULL), 0);
    for (int i = 0; i < NTHREADS; i++)
        pthread_join(th[i], NULL);

    CHECK_INT(roster_count(&R), 0);            /* nem a mais nem a menos */
    roster_destroy(&R);
}

/* 12. A CORRIDA QUE O REFCOUNT EXISTE PARA RESOLVER.
 *     Metade das threads tira retratos e usa os ponteiros; a outra metade
 *     entra e sai sem parar. Sem refcount, isto e use-after-free — e o
 *     AddressSanitizer aponta a linha exata. */
static void *worker_broadcast(void *arg)
{
    (void)arg;
    struct client *snap[ROSTER_MAX];
    for (int i = 0; i < NLOOPS; i++) {
        size_t n = roster_snapshot(&R, snap, ROSTER_MAX, NULL);
        for (size_t j = 0; j < n; j++) {
            /* Ler fora do lock, como faz o broadcast a serio. */
            volatile int fd = snap[j]->io.fd;   /* M6: o fd vive no conn_io */
            (void)fd;
        }
        for (size_t j = 0; j < n; j++)
            client_release(&R, snap[j]);
    }
    return NULL;
}

static void caso_12_retrato_vs_saida(void)
{
    CASE(12, "retratos em paralelo com entradas/saidas: sem use-after-free");
    pthread_t th[NTHREADS * 2];

    roster_init(&R);
    for (int i = 0; i < NTHREADS; i++) {
        CHECK_INT(pthread_create(&th[i], NULL, worker_entra_sai, NULL), 0);
        CHECK_INT(pthread_create(&th[NTHREADS + i], NULL, worker_broadcast, NULL), 0);
    }
    for (int i = 0; i < NTHREADS * 2; i++)
        pthread_join(th[i], NULL);

    CHECK_INT(roster_count(&R), 0);
    roster_destroy(&R);
}

/* 13. Clientes de longa duracao a receber retratos: o count nunca excede o
 *     numero real, e no fim bate certo. */
static void caso_13_estaveis_e_retratos(void)
{
    CASE(13, "clientes estaveis + retratos concorrentes");
    pthread_t th[NTHREADS];
    struct client *fixos[3];

    roster_init(&R);
    for (int i = 0; i < 3; i++) {
        fixos[i] = add_joined("fixo", "f");
        NEED(fixos[i]);
    }

    for (int i = 0; i < NTHREADS; i++)
        CHECK_INT(pthread_create(&th[i], NULL, worker_broadcast, NULL), 0);
    for (int i = 0; i < NTHREADS; i++)
        pthread_join(th[i], NULL);

    CHECK_INT(roster_count(&R), 3);            /* os fixos continuam la */
    for (int i = 0; i < 3; i++) { roster_remove(&R, fixos[i]); client_release(&R, fixos[i]); }
    CHECK_INT(roster_count(&R), 0);
    roster_destroy(&R);
}

/* 14. Quem ainda NAO fez NICK esta na lista mas NAO recebe broadcasts.
 *
 *     Entre o accept() e o NICK aceite o cliente esta a meio do handshake
 *     aplicacional. Sem esta separacao, a primeira linha que via nao era o OK
 *     ao seu proprio NICK mas o EVENT de outra pessoa a entrar — e um cliente
 *     de outro grupo que espere o OK como primeira resposta parte-se. */
static void caso_14_so_entra_no_chat_depois_do_nick(void)
{
    struct client *snap[ROSTER_MAX];

    CASE(14, "ligado mas sem NICK: conta na lista, fora do retrato");
    roster_init(&R);
    struct client *novo = roster_add(&R, -1, "sem-nick");
    NEED(novo);
    CHECK_INT(roster_count(&R), 1);                /* esta na lista      */
    CHECK_INT(roster_snapshot(&R, snap, ROSTER_MAX, NULL), 0);  /* mas nao no chat */
    CHECK_INT(novo->joined, 0);
    CHECK_STR(novo->nick, "");

    CASE(14, "depois do roster_join entra no retrato, com o nick");
    roster_join(&R, novo, "ana");
    CHECK_INT(novo->joined, 1);
    CHECK_STR(novo->nick, "ana");
    size_t n = roster_snapshot(&R, snap, ROSTER_MAX, NULL);
    CHECK_INT(n, 1);
    for (size_t i = 0; i < n; i++)
        client_release(&R, snap[i]);

    CASE(14, "um que entrou nao ve no retrato outro que ainda nao fez NICK");
    struct client *mudo = roster_add(&R, -1, "mudo");
    NEED(mudo);
    CHECK_INT(roster_count(&R), 2);
    n = roster_snapshot(&R, snap, ROSTER_MAX, NULL);
    CHECK_INT(n, 1);                                /* so o que ja entrou */
    CHECK(n == 1 && snap[0] == novo);
    for (size_t i = 0; i < n; i++)
        client_release(&R, snap[i]);

    CASE(14, "roster_join e idempotente");
    roster_join(&R, novo, "ana");
    n = roster_snapshot(&R, snap, ROSTER_MAX, NULL);
    CHECK_INT(n, 1);
    for (size_t i = 0; i < n; i++)
        client_release(&R, snap[i]);

    roster_remove(&R, novo); client_release(&R, novo);
    roster_remove(&R, mudo); client_release(&R, mudo);
    roster_destroy(&R);
}


/* ---- Nicks (M3) --------------------------------------------------------- */

/* 15. roster_claim: nicks unicos, sem olhar a maiusculas (D18). */
static void caso_15_claim(void)
{
    roster_init(&R);
    struct client *a = roster_add(&R, -1, "a");
    struct client *b = roster_add(&R, -1, "b");
    NEED(a); NEED(b);

    CASE(15, "o primeiro a pedir \"ana\" fica com ela");
    CHECK_INT(roster_claim(&R, a, "ana"), 1);
    CHECK_STR(a->nick, "ana");

    CASE(15, "o segundo nao: 409, mesmo ANTES do join (reservado conta)");
    CHECK_INT(a->joined, 0);
    CHECK_INT(roster_claim(&R, b, "ana"), 0);
    CHECK_STR(b->nick, "");

    CASE(15, "nem com outras maiusculas (D18)");
    CHECK_INT(roster_claim(&R, b, "ANA"), 0);
    CHECK_INT(roster_claim(&R, b, "Ana"), 0);

    CASE(15, "outro nick passa; o proprio pode voltar a pedir o seu");
    CHECK_INT(roster_claim(&R, b, "rui"), 1);
    CHECK_INT(roster_claim(&R, a, "ana"), 1);

    CASE(15, "quando a dona sai da lista, \"ana\" fica livre");
    roster_remove(&R, a);
    struct client *c = roster_add(&R, -1, "c");
    NEED(c);
    CHECK_INT(roster_claim(&R, c, "ana"), 1);

    client_release(&R, a);
    roster_remove(&R, b); client_release(&R, b);
    roster_remove(&R, c); client_release(&R, c);
    roster_destroy(&R);
}

/* 16. roster_lookup / roster_find: so quem esta no chat; find segura o cliente. */
static void caso_16_find(void)
{
    char canon[PROTO_NICK_MAX + 1];

    roster_init(&R);
    struct client *a = roster_add(&R, -1, "a");
    NEED(a);
    roster_claim(&R, a, "Ana");

    CASE(16, "reservado mas ainda nao no chat: nao e encontrado (PRIV -> 404)");
    CHECK_INT(roster_lookup(&R, "Ana", canon), 0);
    CHECK(roster_find(&R, "Ana") == NULL);

    CASE(16, "depois do join e encontrado, sem olhar a maiusculas, com o nick registado");
    roster_join(&R, a, "Ana");
    CHECK_INT(roster_lookup(&R, "ana", canon), 1);
    CHECK_STR(canon, "Ana");
    CHECK_INT(roster_lookup(&R, "rui", NULL), 0);

    CASE(16, "find da +1 referencia: o cliente sobrevive a saida ate ao release");
    struct client *f = roster_find(&R, "ANA");
    CHECK(f == a);
    roster_remove(&R, a);
    client_release(&R, a);                    /* a da thread dona */
    if (f != NULL) {
        CHECK_STR(f->nick, "Ana");            /* ainda valido: o ASan diria se nao */
        client_release(&R, f);                /* agora sim, liberta */
    }

    CASE(16, "depois de sair ja nao e encontrado");
    CHECK_INT(roster_lookup(&R, "ana", NULL), 0);
    roster_destroy(&R);
}

/* 17. roster_nicks: os nicks de quem esta no chat. */
static void caso_17_nicks(void)
{
    char out[ROSTER_MAX][PROTO_NICK_MAX + 1];

    roster_init(&R);
    CASE(17, "vazio");
    CHECK_INT(roster_nicks(&R, out, ROSTER_MAX), 0);

    struct client *a = add_joined("a", "ana");
    struct client *b = add_joined("b", "rui");
    struct client *m = roster_add(&R, -1, "mudo");
    NEED(a); NEED(b); NEED(m);
    roster_claim(&R, m, "zed");               /* reservado, nao entrou */

    CASE(17, "so os que estao no chat");
    size_t n = roster_nicks(&R, out, ROSTER_MAX);
    CHECK_INT(n, 2);
    int viu_ana = 0, viu_rui = 0;
    for (size_t i = 0; i < n; i++) {
        viu_ana |= strcmp(out[i], "ana") == 0;
        viu_rui |= strcmp(out[i], "rui") == 0;
    }
    CHECK(viu_ana && viu_rui);

    CASE(17, "respeita cap");
    CHECK_INT(roster_nicks(&R, out, 1), 1);

    roster_remove(&R, a); client_release(&R, a);
    roster_remove(&R, b); client_release(&R, b);
    roster_remove(&R, m); client_release(&R, m);
    roster_destroy(&R);
}

/* 18. A CORRIDA DOS NICKS UNICOS.
 *     16 threads, cada uma com o seu cliente, pedem "ana" no mesmo instante
 *     (barreira). Exatamente UMA pode ganhar. Repetido 200 vezes. Se a
 *     verificacao e a reserva estivessem em locks separados, varias rondas
 *     acabavam com duas "ana" — a mutacao "lookup + claim" faz isto falhar. */
#define NCLAIM 16
static pthread_barrier_t  barreira;
static struct client     *claimers[NCLAIM];
static int                ganhou[NCLAIM];

static void *worker_claim(void *arg)
{
    int i = (int)(long)arg;
    pthread_barrier_wait(&barreira);
    ganhou[i] = roster_claim(&R, claimers[i], "ana");
    return NULL;
}

static void caso_18_claim_em_paralelo(void)
{
    CASE(18, "16 threads pedem \"ana\" ao mesmo tempo, 200x: ganha sempre UMA");
    int rondas_mas = 0;

    for (int ronda = 0; ronda < NLOOPS; ronda++) {
        pthread_t th[NCLAIM];
        roster_init(&R);
        pthread_barrier_init(&barreira, NULL, NCLAIM);
        for (int i = 0; i < NCLAIM; i++) {
            claimers[i] = roster_add(&R, -1, "t");
            ganhou[i] = 0;
        }
        for (int i = 0; i < NCLAIM; i++)
            pthread_create(&th[i], NULL, worker_claim, (void *)(long)i);
        int vencedores = 0;
        for (int i = 0; i < NCLAIM; i++) {
            pthread_join(th[i], NULL);
            vencedores += ganhou[i];
        }
        if (vencedores != 1)
            rondas_mas++;
        pthread_barrier_destroy(&barreira);
        for (int i = 0; i < NCLAIM; i++) {
            roster_remove(&R, claimers[i]);
            client_release(&R, claimers[i]);
        }
        roster_destroy(&R);
    }
    CHECK_INT(rondas_mas, 0);
}

/* 19. roster_users() (M5): so quem esta NO CHAT. Quem ligou e ainda nao fez
 *     NICK, ou reservou e ainda nao entrou, nao conta. */
static void caso_19_users(void)
{
    roster_init(&R);
    CASE(19, "vazio");
    CHECK_INT(roster_users(&R), 0);

    struct client *a = add_joined("a", "ana");
    struct client *m = roster_add(&R, -1, "mudo");      /* ligou, sem NICK */
    struct client *z = roster_add(&R, -1, "zed");
    NEED(a); NEED(m); NEED(z);
    roster_claim(&R, z, "zed");                         /* reservou, nao entrou */

    CASE(19, "so os que estao no chat (roster_count conta todos)");
    CHECK_INT(roster_users(&R), 1);
    CHECK_INT(roster_count(&R), 3);

    CASE(19, "entra mais um, sai o primeiro");
    roster_join(&R, z, "zed");
    CHECK_INT(roster_users(&R), 2);
    roster_remove(&R, a); client_release(&R, a);
    CHECK_INT(roster_users(&R), 1);

    roster_remove(&R, m); client_release(&R, m);
    roster_remove(&R, z); client_release(&R, z);
    CHECK_INT(roster_users(&R), 0);
    roster_destroy(&R);
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    caso_1_vazio();
    caso_2_entrar();
    caso_3_sair();
    caso_4_retrato_segura_o_cliente();
    caso_5_except();
    caso_6_cap();
    caso_7_cheio();
    caso_8_remove_duas_vezes();
    caso_9_fecha_o_fd();
    caso_10_destroy();
    caso_14_so_entra_no_chat_depois_do_nick();
    caso_11_entra_sai_em_paralelo();
    caso_12_retrato_vs_saida();
    caso_13_estaveis_e_retratos();
    caso_15_claim();
    caso_16_find();
    caso_17_nicks();
    caso_18_claim_em_paralelo();
    caso_19_users();
    return t_report("roster");
}
