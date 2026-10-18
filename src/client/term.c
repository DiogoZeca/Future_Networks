/*
 * term.c — o ecrã e o teclado do cliente. Contrato e porquê em include/term.h.
 */
#include "term.h"
#include "ui.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define ERASE_LINE "\r\033[K"   /* início da linha + apagar até ao fim */

/* O mutex do ecrã: protege `line` e o que está escrito no terminal. */
static pthread_mutex_t screen = PTHREAD_MUTEX_INITIALIZER;
static struct ui_line  line;

/* 1 = modo raw com prompt. _Atomic porque a thread de leitura o lê no
 * term_out() e o term_stop() pode correr noutra thread (o term_exit() da de
 * leitura corre os atexit nela). */
static _Atomic int interactive = 0;

static struct termios orig, raw;
/* Os handlers de sinais só podem usar variáveis deste tipo. */
static volatile sig_atomic_t raw_on      = 0;
static volatile sig_atomic_t need_redraw = 0;
static volatile sig_atomic_t quit_req    = 0;   /* M4: Ctrl+C = sair como o /quit */

static char   prompt[PROTO_NICK_MAX + 3];   /* "<nick>> " */
static size_t prompt_cols;

static sigset_t held, saved_mask;

/* ---- desenhar (sempre com o mutex) -------------------------------------- */

static size_t term_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/* Apaga a linha atual e escreve o prompt e o FIM do que se está a escrever
 * (o que cabe na largura; ui_line_tail nunca corta um carácter a meio). */
static void redraw_locked(void)
{
    size_t w    = term_width();
    size_t cols = w > prompt_cols + 1 ? w - prompt_cols - 1 : 1;
    size_t from = ui_line_tail(&line, cols);
    size_t to   = ui_line_shown(&line);         /* sem um carácter a meio */

    fputs(ERASE_LINE, stdout);
    fputs(prompt, stdout);
    fwrite(line.buf + from, 1, to - from, stdout);
    fflush(stdout);
}

/* ---- sinais ------------------------------------------------------------- */

/* Ctrl+\ (e o 2.º Ctrl+C, M4): repor o terminal e morrer como antes.
 * Só funções async-signal-safe (tcsetattr, write, sigaction, raise). O sinal
 * está bloqueado dentro do handler: o raise() fica pendente e é entregue,
 * já com a ação por omissão, quando o handler retornar. */
static void on_fatal(int sig)
{
    if (raw_on) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig);
        if (write(STDOUT_FILENO, "\r\n", 2) < 0) { /* nada a fazer */ }
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, NULL);
    raise(sig);
}

static void install(int sig, void (*fn)(int));

/* M4: Ctrl+C, SIGTERM, SIGHUP — sair como o /quit (o main manda o QUIT e
 * espera o fecho). Só se marca e deixa o read()/fgets() do teclado voltar com
 * EINTR. Um SEGUNDO sinal destes já mata (on_fatal, que repõe o terminal): é
 * a saída para quem não quiser esperar por um servidor que não fecha. */
static void on_quit(int sig)
{
    quit_req = 1;
    install(sig, on_fatal);
}

/* Ctrl+Z: repor o terminal, suspender a sério, e ao continuar (fg) voltar
 * ao modo raw e pedir para redesenhar. */
static void on_tstp(int sig)
{
    int e = errno;
    if (raw_on) {
        if (write(STDOUT_FILENO, ERASE_LINE, sizeof ERASE_LINE - 1) < 0) { }
        tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, NULL);

    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, sig);
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);
    raise(sig);                                 /* pára AQUI até ao fg */

    install(sig, on_tstp);
    if (raw_on)
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    need_redraw = 1;
    errno = e;
}

/* SIGCONT (fg depois de Ctrl+Z, ou kill -CONT): a shell pode ter mexido no
 * terminal entretanto. */
static void on_cont(int sig)
{
    (void)sig;
    int e = errno;
    if (raw_on)
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    need_redraw = 1;
    errno = e;
}

static void on_winch(int sig)                   /* a janela mudou de tamanho */
{
    (void)sig;
    need_redraw = 1;
}

/* Sem SA_RESTART de propósito: o read() do teclado tem de voltar com EINTR
 * para se redesenhar logo. (O resto do cliente já repete com EINTR:
 * send_all, recv_retry.) */
static void install(int sig, void (*fn)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static const int handled[] = { SIGINT, SIGQUIT, SIGTERM, SIGHUP,
                               SIGTSTP, SIGCONT, SIGWINCH };

/* M4: o Ctrl+C gracioso vale com e sem terminal. */
static void install_quit(void)
{
    install(SIGINT,  on_quit);
    install(SIGTERM, on_quit);
    install(SIGHUP,  on_quit);
}

int term_quit_requested(void)
{
    return quit_req != 0;
}

void term_hold_signals(void)
{
    sigemptyset(&held);
    for (size_t i = 0; i < sizeof handled / sizeof handled[0]; i++)
        sigaddset(&held, handled[i]);
    pthread_sigmask(SIG_BLOCK, &held, &saved_mask);
}

void term_release_signals(void)
{
    pthread_sigmask(SIG_SETMASK, &saved_mask, NULL);
}

/* ---- arrancar e parar --------------------------------------------------- */

int term_start(const char *nick)
{
    install_quit();                         /* M4: também sem terminal */
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return 0;
    if (tcgetattr(STDIN_FILENO, &orig) != 0)
        return 0;

    raw = orig;
    /* Sem ICANON (tecla a tecla) nem ECHO (somos nós que desenhamos), nem
     * IEXTEN (o Ctrl+V chegava a nós). Fica o ISIG: Ctrl+C e Ctrl+Z
     * continuam a ser sinais. Fica o OPOST: o '\n' continua a voltar ao
     * início da linha. Fica o IXON: o Ctrl+S continua a parar o ecrã. */
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | IEXTEN);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
        return 0;
    raw_on = 1;

    static int registered = 0;
    if (!registered) {
        atexit(term_stop);                      /* no fim normal do main() */
        registered = 1;
    }

    install(SIGQUIT,  on_fatal);
    install(SIGTSTP,  on_tstp);
    install(SIGCONT,  on_cont);
    install(SIGWINCH, on_winch);

    snprintf(prompt, sizeof prompt, "%s> ", nick);
    prompt_cols = 0;
    for (const char *p = prompt; *p != '\0'; p++)
        if (((unsigned char)*p & 0xC0) != 0x80)  /* caracteres, não bytes */
            prompt_cols++;

    pthread_mutex_lock(&screen);
    ui_line_init(&line);
    interactive = 1;
    redraw_locked();
    pthread_mutex_unlock(&screen);
    return 1;
}

void term_stop(void)
{
    /* Sem o mutex: pode ser chamado pelo atexit enquanto outra thread o tem.
     * No pior caso, uma linha sai desalinhada no fim. */
    if (!raw_on)
        return;
    interactive = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    raw_on = 0;
    fputs(ERASE_LINE, stdout);                  /* não deixar o prompt pendurado */
    fflush(stdout);
}

/* ---- mostrar e ler ------------------------------------------------------ */

void term_out(FILE *f, const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&screen);
    int on = interactive;
    if (on) {
        fputs(ERASE_LINE, stdout);
        fflush(stdout);                         /* antes de escrever no stderr */
    }
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
    if (on)
        redraw_locked();
    pthread_mutex_unlock(&screen);
}

/* Sem terminal: como o cliente fazia antes da correção. */
static int read_cooked(char *out, size_t cap)
{
    /* Um Ctrl+C (M4) interrompe o fgets() (sem SA_RESTART): NULL, e sai-se
     * como no fim do stdin. */
    if (quit_req || fgets(out, (int)cap, stdin) == NULL)
        return TERM_EOF;
    /* Linha maior do que o buffer: o fgets parte-a, e o resto viria como se
     * fosse a linha seguinte — o mesmo problema do framing, agora do lado do
     * teclado. Deitar fora até ao '\n'. */
    if (strchr(out, '\n') == NULL && !feof(stdin)) {
        int c;
        while ((c = getchar()) != '\n' && c != EOF)
            ;
        return TERM_TOOLONG;
    }
    return TERM_LINE;
}

int term_readline(char *out, size_t cap)
{
    if (!interactive)
        return read_cooked(out, cap);

    for (;;) {
        if (need_redraw) {                      /* depois de um fg ou resize */
            need_redraw = 0;
            pthread_mutex_lock(&screen);
            redraw_locked();
            pthread_mutex_unlock(&screen);
        }

        if (quit_req)                           /* M4: Ctrl+C = /quit */
            return TERM_EOF;

        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);  /* fora do mutex */
        if (n == 0)
            return TERM_EOF;                    /* o terminal fechou */
        if (n < 0) {
            if (errno == EINTR)
                continue;                       /* resize, fg, Ctrl+C: ver acima */
            return TERM_ERR;
        }

        pthread_mutex_lock(&screen);
        int r = ui_line_key(&line, c);
        switch (r) {
        case UI_KEY_REDRAW:
            redraw_locked();
            break;
        case UI_KEY_FULL:
            fputs("\a", stdout);                /* campainha: não cabe mais */
            fflush(stdout);
            break;
        case UI_KEY_SUBMIT:
            snprintf(out, cap, "%s", line.buf);
            /* Apagar o que se escreveu: o que fica no ecrã é a resposta do
             * servidor (o nosso CHAT, D5) ou o eco local do /msg. Era o
             * "duplica no ecrã". */
            ui_line_init(&line);
            redraw_locked();
            pthread_mutex_unlock(&screen);
            return TERM_LINE;
        case UI_KEY_EOF:
            pthread_mutex_unlock(&screen);
            return TERM_EOF;
        default:                                /* UI_KEY_NONE */
            break;
        }
        pthread_mutex_unlock(&screen);
    }
}

void term_exit(int code)
{
    /* O mutex do ecrã: que nenhuma outra thread esteja a meio de uma linha.
     * Nunca se larga — o processo acaba aqui. */
    pthread_mutex_lock(&screen);
    term_stop();                               /* o atexit não corre com _exit() */
    fflush(stdout);                            /* stdout/stderr: ninguém está */
    fflush(stderr);                            /* parado com o lock deles */
    _exit(code);
}
