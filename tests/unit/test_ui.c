/*
 * test_ui.c — testes da parte pura do cliente (include/ui.h).
 *
 * Numeração: 40-47, a seguir aos da session (M1/M2); 71-75 com os comandos
 * e o USERS (M3); 84 da revisão do M3 (mostrar várias linhas sem deixar um
 * '\033' mexer no terminal, e o que o protocolo cortou). O 43 mudou: o tab
 * passou a ser texto válido. 85-87 da correção do M3: o editor de linha
 * (ui_line_*), tecla a tecla, sem terminal.
 */
#include "ui.h"
#include "test_util.h"

static struct msg  m;
static const char *why;
static char        typed[PROTO_TEXT_MAX + 16];
static char        shown[PROTO_LINE_BUF];

/* 40. Linhas normais: o fim de linha sai, o texto fica intacto. */
static void caso_40_linha_normal(void)
{
    CASE(40, "\"ola\\n\" -> MSG \"ola\"");
    CHECK_INT(ui_input_to_msg("ola\n", &m, &why), 1);
    CHECK_INT(m.type, MT_MSG);
    CHECK_STR(m.text, "ola");

    CASE(40, "\"\\r\\n\" tambem sai");
    CHECK_INT(ui_input_to_msg("ola\r\n", &m, &why), 1);
    CHECK_STR(m.text, "ola");

    CASE(40, "sem '\\n' (ultima linha antes do EOF)");
    CHECK_INT(ui_input_to_msg("fim", &m, &why), 1);
    CHECK_STR(m.text, "fim");

    CASE(40, "espacos e UTF-8 sao texto normal");
    CHECK_INT(ui_input_to_msg("  ol\xC3\xA1 mundo  \n", &m, &why), 1);
    CHECK_STR(m.text, "  ol\xC3\xA1 mundo  ");
}

/* 41. Enter sozinho: nao envia nada (0), nao e erro. */
static void caso_41_linha_vazia(void)
{
    CASE(41, "linha vazia -> 0");
    CHECK_INT(ui_input_to_msg("\n", &m, &why), 0);
    CHECK_INT(ui_input_to_msg("\r\n", &m, &why), 0);
    CHECK_INT(ui_input_to_msg("", &m, &why), 0);
    CHECK_INT(m.type, 0);
}

/* 42. Limite do texto, dos dois lados, com motivo concreto. */
static void caso_42_limite(void)
{
    CASE(42, "1024 bytes + '\\n' aceite");
    memset(typed, 'x', PROTO_TEXT_MAX);
    strcpy(typed + PROTO_TEXT_MAX, "\n");
    CHECK_INT(ui_input_to_msg(typed, &m, &why), 1);
    CHECK_INT(strlen(m.text), PROTO_TEXT_MAX);

    CASE(42, "1025 bytes -> -1 'demasiado longa'");
    memset(typed, 'x', PROTO_TEXT_MAX + 1);
    strcpy(typed + PROTO_TEXT_MAX + 1, "\n");
    why = NULL;
    CHECK_INT(ui_input_to_msg(typed, &m, &why), -1);
    CHECK_STR(why, UI_WHY_TOOLONG);
    CHECK_INT(m.type, 0);
}

/* 43. Caracteres de controlo. Ate a revisao do M3 eram recusados ("a\tb"
 *     dava -1); desde que o text aceita varias linhas, sao texto valido. */
static void caso_43_controlo(void)
{
    CASE(43, "tab e aceite (revisao do M3: o text pode ter controlo)");
    why = NULL;
    CHECK_INT(ui_input_to_msg("a\tb\n", &m, &why), 1);
    CHECK_STR(m.text, "a\tb");

    CASE(43, "o que passa no ui passa no encode (mesmas regras)");
    CHECK_INT(ui_input_to_msg("diz \"oi\" \\\n", &m, &why), 1);
    CHECK_INT(proto_encode(&m, shown, sizeof shown, NULL), 0);
}

/* 44. Formato de cada tipo recebido. */
static void caso_44_formato(void)
{
    CASE(44, "CHAT");
    struct msg c = { .type = MT_CHAT, .from = "ana", .text = "hello" };
    ui_format(&c, shown, sizeof shown);
    CHECK_STR(shown, "<ana> hello");

    CASE(44, "CHAT privado");
    c.is_private = 1;
    ui_format(&c, shown, sizeof shown);
    CHECK_STR(shown, "<ana> (privado) hello");

    CASE(44, "ERROR com e sem reason");
    struct msg e = { .type = MT_ERROR, .code = 401, .reason = "nick required" };
    ui_format(&e, shown, sizeof shown);
    CHECK_STR(shown, "[erro 401] nick required");
    e.reason[0] = '\0';
    ui_format(&e, shown, sizeof shown);
    CHECK_STR(shown, "[erro 401]");

    CASE(44, "OK e tipo nenhum");
    struct msg o = { .type = MT_OK };
    ui_format(&o, shown, sizeof shown);
    CHECK_STR(shown, "[ok]");
    struct msg z = { .type = 0 };
    ui_format(&z, shown, sizeof shown);
    CHECK_STR(shown, "[?]");
}

/* 45. Buffer pequeno: trunca, termina em '\0', nao escreve fora. Sob SAN=1
 *     o ASan confirma que nao ha escrita depois de out[cap-1]. */
static void caso_45_truncar(void)
{
    CASE(45, "cap pequeno trunca com '\\0'");
    char small[6];
    struct msg c = { .type = MT_CHAT, .from = "ana", .text = "hello" };
    ui_format(&c, small, sizeof small);
    CHECK_STR(small, "<ana>");

    CASE(45, "cap 0 nao escreve nada");
    char guard = 'G';
    ui_format(&c, &guard, 0);
    CHECK_INT(guard, 'G');
}

/* 46. Validacao do nick da linha de comando (antes de ligar). */
static void caso_46_nick_argv(void)
{
    CASE(46, "proto_valid_nick para o nick do argv");
    CHECK_INT(proto_valid_nick("ana"), 1);
    CHECK_INT(proto_valid_nick("a b"), 0);
    CHECK_INT(proto_valid_nick(""), 0);
    CHECK_INT(proto_valid_nick("123456789012345678901234567890123"), 0);
}

/* 47. O cliente tem de MOSTRAR os EVENT (M2).
 *
 *     O switch do ui_format tem um `default`, por isso o compilador nao avisa
 *     se faltar um caso: sem este teste, um EVENT aparecia como "[?]" e as
 *     entradas e saidas passavam despercebidas. O '*' a frente distingue a
 *     vista um aviso do servidor de uma mensagem de alguem, que leva "<nick>". */
static void caso_47_event(void)
{
    char out[256];

    CASE(47, "EVENT de entrada");
    struct msg e = { .type = MT_EVENT };
    memcpy(e.text, "rui joined", sizeof "rui joined");
    ui_format(&e, out, sizeof out);
    CHECK_STR(out, "* rui joined");

    CASE(47, "EVENT de saida");
    memcpy(e.text, "ana left", sizeof "ana left");
    ui_format(&e, out, sizeof out);
    CHECK_STR(out, "* ana left");

    CASE(47, "nao cai no default \"[?]\"");
    CHECK(strcmp(out, "[?]") != 0);

    CASE(47, "um EVENT longo e truncado, nunca escreve fora do buffer");
    char curto[8];
    memset(e.text, 'x', 100);
    e.text[100] = '\0';
    ui_format(&e, curto, sizeof curto);
    CHECK_INT(strlen(curto), sizeof curto - 1);
}


/* ---- M3: comandos ------------------------------------------------------- */

/* 71. /list e /quit. */
static void caso_71_list_quit(void)
{
    CASE(71, "/list -> LIST");
    CHECK_INT(ui_input_to_msg("/list\n", &m, &why), UI_SEND);
    CHECK_INT(m.type, MT_LIST);
    CHECK_INT(proto_encode(&m, shown, sizeof shown, NULL), 0);
    CHECK_STR(shown, "{\"type\":\"LIST\"}\n");

    CASE(71, "/quit -> QUIT (espacos no fim tolerados)");
    CHECK_INT(ui_input_to_msg("/quit  \r\n", &m, &why), UI_SEND);
    CHECK_INT(m.type, MT_QUIT);

    CASE(71, "/list com argumentos -> erro local, nada enviado");
    why = NULL;
    CHECK_INT(ui_input_to_msg("/list todos\n", &m, &why), UI_BAD);
    CHECK_STR(why, UI_WHY_ARGS);
    CHECK_INT(m.type, 0);
}

/* 72. /msg <nick> <texto> -> PRIV. */
static void caso_72_msg(void)
{
    CASE(72, "/msg rui ola -> PRIV com os bytes do slide");
    CHECK_INT(ui_input_to_msg("/msg rui hi\n", &m, &why), UI_SEND);
    CHECK_INT(m.type, MT_PRIV);
    CHECK_STR(m.to, "rui");
    CHECK_STR(m.text, "hi");
    CHECK_INT(proto_encode(&m, shown, sizeof shown, NULL), 0);
    CHECK_STR(shown, "{\"type\":\"PRIV\",\"to\":\"rui\",\"text\":\"hi\"}\n");

    CASE(72, "o texto fica tal e qual, com espacos (parte so no 1.o)");
    CHECK_INT(ui_input_to_msg("/msg rui  dois  espacos \n", &m, &why), UI_SEND);
    CHECK_STR(m.to, "rui");
    CHECK_STR(m.text, " dois  espacos ");

    CASE(72, "espacos a mais antes do nick sao tolerados");
    CHECK_INT(ui_input_to_msg("/msg   rui oi\n", &m, &why), UI_SEND);
    CHECK_STR(m.to, "rui");

    CASE(72, "/msg sem nick ou sem texto -> uso");
    why = NULL;
    CHECK_INT(ui_input_to_msg("/msg\n", &m, &why), UI_BAD);
    CHECK_STR(why, UI_WHY_MSG_USE);
    CHECK_INT(ui_input_to_msg("/msg rui\n", &m, &why), UI_BAD);
    CHECK_STR(why, UI_WHY_MSG_USE);
    CHECK_INT(ui_input_to_msg("/msg rui \n", &m, &why), UI_BAD);
    CHECK_STR(why, UI_WHY_MSG_USE);
    CHECK_INT(m.type, 0);

    CASE(72, "nick invalido (aspas, D20; ou 33 bytes) -> erro local");
    CHECK_INT(ui_input_to_msg("/msg a\"b oi\n", &m, &why), UI_BAD);
    CHECK_STR(why, UI_WHY_BADNICK);
    CHECK_INT(ui_input_to_msg("/msg 123456789012345678901234567890123 oi\n", &m, &why), UI_BAD);
    CHECK_STR(why, UI_WHY_BADNICK);

    CASE(72, "texto com tab passa; longo demais -> o mesmo motivo do MSG");
    CHECK_INT(ui_input_to_msg("/msg rui a\tb\n", &m, &why), UI_SEND);
    CHECK_STR(m.text, "a\tb");
    static char longo[PROTO_TEXT_MAX + 32];
    strcpy(longo, "/msg rui ");
    memset(longo + 9, 'x', PROTO_TEXT_MAX + 1);
    longo[9 + PROTO_TEXT_MAX + 1] = '\0';
    CHECK_INT(ui_input_to_msg(longo, &m, &why), UI_BAD);
    CHECK_STR(why, UI_WHY_TOOLONG);
    longo[9 + PROTO_TEXT_MAX] = '\0';                /* exatamente 4096: passa */
    CHECK_INT(ui_input_to_msg(longo, &m, &why), UI_SEND);
}

/* 73. /help, comandos desconhecidos e "//". */
static void caso_73_outros(void)
{
    CASE(73, "/help -> UI_HELP, nada a enviar");
    CHECK_INT(ui_input_to_msg("/help\n", &m, &why), UI_HELP);
    CHECK_INT(m.type, 0);

    CASE(73, "comando desconhecido -> erro local (nao vai para o chat por engano)");
    why = NULL;
    CHECK_INT(ui_input_to_msg("/lsit\n", &m, &why), UI_BAD);
    CHECK_STR(why, UI_WHY_UNKNOWN);
    CHECK_INT(ui_input_to_msg("/\n", &m, &why), UI_BAD);
    CHECK_INT(ui_input_to_msg("/msgrui oi\n", &m, &why), UI_BAD);
    CHECK_STR(why, UI_WHY_UNKNOWN);

    CASE(73, "//texto -> MSG \"/texto\"");
    CHECK_INT(ui_input_to_msg("//list nao e comando\n", &m, &why), UI_SEND);
    CHECK_INT(m.type, MT_MSG);
    CHECK_STR(m.text, "/list nao e comando");

    CASE(73, "uma '/' a meio nao e comando");
    CHECK_INT(ui_input_to_msg("a/list\n", &m, &why), UI_SEND);
    CHECK_INT(m.type, MT_MSG);
}

/* 74. Mostrar um CHAT privado (D14). */
static void caso_74_privado(void)
{
    CASE(74, "com \"to\": diz de quem e para quem");
    struct msg c = { .type = MT_CHAT, .from = "ana", .to = "rui", .text = "hi", .is_private = 1 };
    ui_format(&c, shown, sizeof shown);
    CHECK_STR(shown, "<ana -> rui> (privado) hi");

    CASE(74, "sem \"to\" (servidor de outro grupo): como no M2");
    c.to[0] = '\0';
    ui_format(&c, shown, sizeof shown);
    CHECK_STR(shown, "<ana> (privado) hi");
}

/* 75. Mostrar um USERS. */
static void caso_75_users(void)
{
    CASE(75, "USERS -> [online: N] nicks");
    struct msg u = { .type = MT_USERS };
    proto_users_add(&u, "ana");
    proto_users_add(&u, "rui");
    ui_format(&u, shown, sizeof shown);
    CHECK_STR(shown, "[online: 2] ana rui");

    CASE(75, "USERS vazio");
    struct msg v = { .type = MT_USERS };
    ui_format(&v, shown, sizeof shown);
    CHECK_STR(shown, "[online: 0]");

    CASE(75, "nao cai no default \"[?]\"");
    CHECK(strcmp(shown, "[?]") != 0);
}


/* 84. Revisao do M3: mostrar texto de varias linhas e o que vem de outros
 *     servidores, sem deixar caracteres de controlo mexer no terminal. */
static void caso_84_mostrar_em_seguranca(void)
{
    struct msg c = { .type = MT_CHAT, .from = "ana" };

    CASE(84, "varias linhas: as seguintes aparecem indentadas");
    strcpy(c.text, "linha 1\nlinha 2");
    ui_format(&c, shown, sizeof shown);
    CHECK_STR(shown, "<ana> linha 1\n    linha 2");

    CASE(84, "'\\033' (cores, apagar o ecra) e '\\r' (escrever por cima) viram '?'");
    strcpy(c.text, "a\033[2Jb\rc\x7f");
    ui_format(&c, shown, sizeof shown);
    CHECK_STR(shown, "<ana> a?[2Jb?c?");

    CASE(84, "o tab e o UTF-8 ficam");
    strcpy(c.text, "ol\xC3\xA1\tmundo");
    ui_format(&c, shown, sizeof shown);
    CHECK_STR(shown, "<ana> ol\xC3\xA1\tmundo");

    CASE(84, "um EVENT e um reason tambem sao limpos");
    struct msg e = { .type = MT_EVENT, .text = "x\033y" };
    ui_format(&e, shown, sizeof shown);
    CHECK_STR(shown, "* x?y");
    struct msg r = { .type = MT_ERROR, .code = 400, .reason = "a\nb" };
    ui_format(&r, shown, sizeof shown);
    CHECK_STR(shown, "[erro 400] a\n    b");

    CASE(84, "o que o protocolo cortou acaba em [...]");
    strcpy(c.text, "muito texto");
    c.truncated = 1;
    ui_format(&c, shown, sizeof shown);
    CHECK_STR(shown, "<ana> muito texto [...]");

    CASE(84, "um buffer pequeno continua a truncar sem escrever fora, mesmo com '\\n'");
    char small[12];
    strcpy(c.text, "a\nb\nc\nd\ne\nf");
    c.truncated = 0;
    ui_format(&c, small, sizeof small);
    CHECK_INT(strlen(small), sizeof small - 1);

    CASE(84, "o eco local do /msg (m->to preenchido)");
    struct msg echo = { .type = MT_CHAT, .from = "ana", .to = "rui", .is_private = 1,
                        .text = "ola\nrui" };
    ui_format(&echo, shown, sizeof shown);
    CHECK_STR(shown, "<ana -> rui> (privado) ola\n    rui");
}

/* ---- Correcao do M3: o editor de linha --------------------------------- */

static struct ui_line ln;

/* Passa uma sequencia de bytes pelo editor; devolve o resultado da ULTIMA. */
static int keys(const char *k)
{
    int r = UI_KEY_NONE;
    for (; *k != '\0'; k++)
        r = ui_line_key(&ln, (unsigned char)*k);
    return r;
}

/* 85. As teclas que mexem na linha. */
static void caso_85_editor_teclas(void)
{
    CASE(85, "texto acrescenta e pede para redesenhar");
    ui_line_init(&ln);
    CHECK_INT(keys("ola"), UI_KEY_REDRAW);
    CHECK_STR(ln.buf, "ola");
    CHECK_INT(ln.len, 3);

    CASE(85, "Enter com texto -> SUBMIT, com a linha intacta (o chamador limpa)");
    CHECK_INT(keys("\n"), UI_KEY_SUBMIT);
    CHECK_STR(ln.buf, "ola");
    CHECK_INT(keys("\r"), UI_KEY_SUBMIT);     /* sem ICRNL o Enter e '\r' */

    CASE(85, "Enter com a linha vazia -> NADA (o 'spam de Enters' ja nao sobe o ecra)");
    ui_line_init(&ln);
    CHECK_INT(keys("\n"), UI_KEY_NONE);
    CHECK_INT(keys("\n\n\n\r\r"), UI_KEY_NONE);
    CHECK_INT(ln.len, 0);

    CASE(85, "Backspace apaga um CARACTER: os 2 bytes de um 'e' com acento");
    ui_line_init(&ln);
    keys("ol\xC3\xA9");                        /* "olé" = 4 bytes */
    CHECK_INT(ln.len, 4);
    CHECK_INT(keys("\x7f"), UI_KEY_REDRAW);
    CHECK_STR(ln.buf, "ol");
    keys("\xE2\x82\xAC");                      /* "€" = 3 bytes */
    keys("\x08");                               /* ^H tambem e Backspace */
    CHECK_STR(ln.buf, "ol");

    CASE(85, "um caracter UTF-8 so pede para redesenhar quando chega inteiro");
    ui_line_init(&ln);
    keys("a");
    CHECK_INT(ui_line_key(&ln, 0xE2), UI_KEY_NONE);    /* "€", 1.o de 3 bytes */
    CHECK_INT(ui_line_shown(&ln), 1);                 /* meio caracter nao se mostra */
    CHECK_INT(ui_line_key(&ln, 0x82), UI_KEY_NONE);
    CHECK_INT(ui_line_key(&ln, 0xAC), UI_KEY_REDRAW);
    CHECK_STR(ln.buf, "a\xE2\x82\xAC");
    CHECK_INT(ui_line_shown(&ln), 4);

    CASE(85, "um caracter que fica a meio sai inteiro; bytes soltos sao ignorados");
    ui_line_init(&ln);
    keys("a\xC3" "b");                          /* 0xC3 sem continuacao */
    CHECK_STR(ln.buf, "ab");
    keys("\xA9\x80\xFF");                       /* soltos e 0xFF: nunca UTF-8 */
    CHECK_STR(ln.buf, "ab");
    keys("\xC3\x7f");                           /* Backspace a meio: so o meio */
    CHECK_STR(ln.buf, "ab");
    keys("\xC3\n");                             /* Enter a meio: envia sem ele */
    CHECK_STR(ln.buf, "ab");

    CASE(85, "Backspace com a linha vazia nao faz nada");
    ui_line_init(&ln);
    CHECK_INT(keys("\x7f"), UI_KEY_NONE);

    CASE(85, "Ctrl+U apaga a linha; Ctrl+W a ultima palavra");
    ui_line_init(&ln);
    keys("/msg rui ola  ");
    CHECK_INT(keys("\x17"), UI_KEY_REDRAW);
    CHECK_STR(ln.buf, "/msg rui ");
    CHECK_INT(keys("\x15"), UI_KEY_REDRAW);
    CHECK_STR(ln.buf, "");
    CHECK_INT(keys("\x15"), UI_KEY_NONE);

    CASE(85, "Ctrl+D: com a linha vazia sai; com texto e ignorado");
    ui_line_init(&ln);
    CHECK_INT(keys("\x04"), UI_KEY_EOF);
    keys("a");
    CHECK_INT(keys("\x04"), UI_KEY_NONE);
    CHECK_STR(ln.buf, "a");

    CASE(85, "Tab e outros controlos sao ignorados (o texto nao muda)");
    CHECK_INT(keys("\t\x01\x02\x7"), UI_KEY_NONE);
    CHECK_STR(ln.buf, "a");
}

/* 86. Sequencias de escape: as teclas que mandam varios bytes (setas, Home,
 *     Delete, F1...) sao engolidas INTEIRAS, sem deixar "[A" na linha. */
static void caso_86_editor_escapes(void)
{
    CASE(86, "setas (ESC [ A..D) e ESC O x");
    ui_line_init(&ln);
    keys("a\033[Ab\033[Dc\033OAd");
    CHECK_STR(ln.buf, "abcd");

    CASE(86, "com parametros: Delete (ESC [ 3 ~), Ctrl+seta (ESC [ 1 ; 5 C)");
    ui_line_init(&ln);
    keys("x\033[3~y\033[1;5Cz");
    CHECK_STR(ln.buf, "xyz");

    CASE(86, "Alt+tecla (ESC x): a tecla e engolida");
    ui_line_init(&ln);
    keys("1\033f2");
    CHECK_STR(ln.buf, "12");

    CASE(86, "o byte final da sequencia nao entra na linha; o Enter a seguir envia");
    ui_line_init(&ln);
    keys("q\033[");
    CHECK_INT(ui_line_key(&ln, 'B'), UI_KEY_NONE);   /* fim da sequencia */
    CHECK_INT(ui_line_key(&ln, '\n'), UI_KEY_SUBMIT);
    CHECK_STR(ln.buf, "q");
}

/* 87. Limites: a linha nunca passa de UI_LINE_MAX, e um caracter UTF-8 entra
 *     inteiro ou nao entra. E o fim da linha que se mostra quando nao cabe. */
static void caso_87_editor_limites(void)
{
    CASE(87, "cheia: o byte seguinte da FULL e a linha nao muda");
    ui_line_init(&ln);
    for (size_t i = 0; i < UI_LINE_MAX; i++)
        ui_line_key(&ln, 'x');
    CHECK_INT(ln.len, UI_LINE_MAX);
    CHECK_INT(ui_line_key(&ln, 'y'), UI_KEY_FULL);
    CHECK_INT(ln.len, UI_LINE_MAX);
    CHECK_INT(ln.buf[UI_LINE_MAX], '\0');

    CASE(87, "com 1 byte livre, um 'e' (2 bytes) nao entra — nem o 1.o byte, nem o 2.o");
    keys("\x7f");                               /* fica 1 livre */
    CHECK_INT(ui_line_key(&ln, 0xC3), UI_KEY_FULL);
    CHECK_INT(ui_line_key(&ln, 0xA9), UI_KEY_NONE); /* a continuacao e deitada fora */
    CHECK_INT(ln.len, UI_LINE_MAX - 1);
    CHECK_INT(ui_line_key(&ln, 'z'), UI_KEY_REDRAW); /* um ASCII ainda cabe */
    CHECK_INT(ln.len, UI_LINE_MAX);

    CASE(87, "a linha mais longa que o editor deixa escrever da um /msg valido");
    ui_line_init(&ln);
    keys("/msg 12345678901234567890123456789012 ");
    while (ln.len < UI_LINE_MAX)
        ui_line_key(&ln, 'x');
    CHECK_INT(ui_input_to_msg(ln.buf, &m, &why), UI_SEND);
    CHECK_INT(strlen(m.text), PROTO_TEXT_MAX);

    CASE(87, "tail: cabe tudo -> 0; nao cabe -> os ultimos cols caracteres");
    ui_line_init(&ln);
    keys("abcdef");
    CHECK_INT(ui_line_tail(&ln, 10), 0);
    CHECK_INT(ui_line_tail(&ln, 6), 0);
    CHECK_INT(ui_line_tail(&ln, 4), 2);         /* "cdef" */
    CHECK_INT(ui_line_tail(&ln, 0), 6);

    CASE(87, "tail conta caracteres, nao bytes, e nunca corta um a meio");
    ui_line_init(&ln);
    keys("a\xC3\xA9\xC3\xA9");                 /* "aéé" = 5 bytes, 3 caracteres */
    CHECK_INT(ui_line_tail(&ln, 3), 0);
    CHECK_INT(ui_line_tail(&ln, 2), 1);         /* "éé" */
    CHECK_INT(ui_line_tail(&ln, 1), 3);         /* "é" */
}

int main(void)
{
    caso_40_linha_normal();
    caso_41_linha_vazia();
    caso_42_limite();
    caso_43_controlo();
    caso_44_formato();
    caso_45_truncar();
    caso_46_nick_argv();
    caso_47_event();
    caso_71_list_quit();
    caso_72_msg();
    caso_73_outros();
    caso_74_privado();
    caso_75_users();
    caso_84_mostrar_em_seguranca();
    caso_85_editor_teclas();
    caso_86_editor_escapes();
    caso_87_editor_limites();
    return t_report("ui");
}
