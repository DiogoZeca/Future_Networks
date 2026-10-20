"""
test_tls.py — a extensao do M6: TLS numa porta a mais, com certificado
auto-assinado e o fingerprint SHA-256 a autenticar o servidor.

  - o basico: claro e TLS no mesmo chat; o nosso cliente com o fingerprint
    certo, errado e sem ele; portas trocadas; um handshake mudo;
  - o que o TLS muda por baixo: varias linhas num so record (SSL_pending),
    do lado do servidor E do cliente;
  - o que o M4 ja garantia em claro e o TLS NAO pode estragar: um cliente que
    nao le e desligado em 5 s, um record a meio nao encrava ninguem, o fecho
    sai logo (close_notify + FIN) e nao 2 s depois, o Ctrl+C.

Os da ultima parte foram escritos ANTES das correcoes (M6.md, bugs 4 e 5) e
falhavam; as mutacoes que cada teste apanha estao no M6.md.

O certificado de teste e gerado uma vez pelo proprio scripts/gen-cert.sh,
num diretorio temporario (helpers.tls_cert()).
"""
import os
import re
import select
import signal
import socket
import ssl
import subprocess
import threading
import time

import pytest

from helpers import (CLIENT, OK, Conn, chat, free_port, join, no_events, tls_cert,
                     tls_socket)


def run_client(port, stdin, *opts, nick="ana", timeout=15):
    return subprocess.run([CLIENT, *opts, "127.0.0.1", str(port), nick], input=stdin,
                          capture_output=True, text=True, timeout=timeout)


def wrong_fp(fp):
    """O mesmo fingerprint com o primeiro digito trocado."""
    return ("0" if fp[0] != "0" else "1") + fp[1:]


# ---- o basico -------------------------------------------------------------

def test_claro_e_tls_conversam_no_mesmo_chat(tls_server):
    """D-TLS-1: o TLS e uma porta A MAIS. Quem esta em claro e quem esta por
    TLS estao no mesmo chat e ouvem-se nos dois sentidos."""
    claro = join(tls_server.port, "rui")
    cifra = join(tls_server.tls_port, "ana", tls=True)
    assert {"type": "EVENT", "text": "ana joined"} in claro.recv_all()

    claro.send({"type": "MSG", "text": "do claro"})
    cifra.send({"type": "MSG", "text": "do tls"})
    got_c, got_t = no_events(claro.recv_all()), no_events(cifra.recv_all())
    for got in (got_c, got_t):
        assert chat("rui", "do claro") in got and chat("ana", "do tls") in got
    assert tls_server.wait_log("TLS estabelecido")


@pytest.mark.parametrize("fim", ["/quit\n", ""], ids=["quit", "eof"])
def test_o_cliente_com_o_fingerprint_certo_entra_fala_e_sai(tls_server, fim):
    """O ./bin/client com -f: entra, fala, e sai limpo pelo /quit e pelo EOF
    (Ctrl+D) — os dois caminhos do io_shutdown_wr()."""
    _, _, fp = tls_cert()
    rui = join(tls_server.port, "rui")
    p = run_client(tls_server.tls_port, "ola por tls\n" + fim, "-f", fp)
    assert p.returncode == 0, p.stderr
    assert "como ana" in p.stdout
    assert "AVISO" not in p.stderr                  # autenticado: nada a avisar
    assert chat("ana", "ola por tls") in rui.recv_all()


def test_o_fingerprint_errado_recusa_antes_de_o_nick_sair(tls_server):
    """D-TLS-2: com o fingerprint errado o cliente desiste a seguir ao
    handshake. O servidor nunca chega a ver o NICK, e ninguem ve "ana joined"."""
    _, _, fp = tls_cert()
    rui = join(tls_server.port, "rui")
    p = run_client(tls_server.tls_port, "nao devia sair\n", "-f", wrong_fp(fp))
    assert p.returncode != 0
    assert "NAO corresponde ao fingerprint" in p.stderr
    assert "confirma o fingerprint" in p.stderr     # a dica certa para este erro
    assert not any("ana" in m.get("text", "") for m in rui.recv_all(1.0))
    assert "entrou como ana" not in tls_server.log()
    assert tls_server.alive()                       # e o SIGPIPE nao o matou (bug 1)


def test_sem_fingerprint_liga_mas_avisa_e_mostra_o_fingerprint(tls_server):
    _, _, fp = tls_cert()
    p = run_client(tls_server.tls_port, "/quit\n", "-s")
    assert p.returncode == 0
    assert "NAO verificado" in p.stderr
    assert fp in p.stderr                           # para quem o quiser anotar


def test_o_fingerprint_mal_escrito_e_recusado_antes_de_ligar(tls_server):
    antes = tls_server.log().count("ligou")
    p = run_client(tls_server.tls_port, "", "-f", "AB:CD")
    assert p.returncode != 0 and "fingerprint invalido" in p.stderr
    time.sleep(0.3)
    assert tls_server.log().count("ligou") == antes  # nem chegou a ligar


def test_tls_sem_host_recusa_em_vez_de_ir_a_descoberta():
    """A descoberta (M5) so anuncia a porta em claro: com -s e sem host, o
    cliente diz logo o que fazer, em vez de ligar por TLS a porta errada."""
    for opts in (["-s"], ["-f", tls_cert()[2]]):
        p = subprocess.run([CLIENT, *opts], input="", capture_output=True, text=True,
                           timeout=10)
        assert p.returncode != 0
        assert "indica o host e a porta TLS" in p.stderr
        assert "a procurar servidores" not in p.stderr  # nem chegou a procurar


def test_tls_contra_a_porta_em_claro_da_erro_e_nao_fica_preso(tls_server):
    t0 = time.time()
    p = run_client(tls_server.port, "", "-s")
    assert p.returncode != 0 and "[client] TLS:" in p.stderr
    assert time.time() - t0 < 10                    # o prazo do handshake, no maximo


def test_json_em_claro_na_porta_tls_e_recusado_e_o_servidor_continua(tls_server):
    """O erro mais provavel na pratica: um nc ou um cliente de outro grupo na
    porta TLS. E recusado (wrong version number), e mais nada acontece."""
    raw = Conn(tls_server.tls_port)
    raw.send({"type": "NICK", "nick": "ana"})
    assert raw.wait_closed(3.0) is not None
    assert tls_server.wait_log("handshake TLS falhou")
    assert tls_server.alive()
    join(tls_server.port, "ana")                    # o nick nao ficou preso


def test_um_handshake_mudo_tem_prazo(make_server):
    """Ligar a porta TLS e nao dizer nada (um varrimento de portas) nao pode
    prender a thread para sempre: o prazo e o do NICK (-n)."""
    s = make_server("-n", "1", tls=True)
    raw = socket.create_connection(("127.0.0.1", s.tls_port))
    assert s.wait_log("handshake TLS falhou", timeout=4)
    raw.close()
    join(s.port, "rui")                             # e o resto continua


def test_sem_certificado_o_tls_fica_desligado_e_o_claro_funciona(make_server):
    """O TLS e uma extensao: se o certificado falhar, o servidor avisa e o
    chat em claro (o que interopera com os outros grupos) arranca na mesma."""
    s = make_server("-T", str(free_port()), "-C", "/nao/existe.pem", "-K", "/nao/existe.key")
    assert s.wait_log("TLS desligado")
    assert s.wait_log("gen-cert.sh")                # e diz como se resolve
    join(s.port, "ana")


# ---- o que o TLS muda por baixo: records ---------------------------------

def test_varias_linhas_num_so_record_sao_todas_tratadas(tls_server):
    """Um record TLS com ~15 KB de linhas: o servidor le 4 KB (RX_CHUNK) e o
    resto fica decifrado DENTRO do SSL, com o socket vazio. Sem o
    SSL_pending() no io_wait_readable(), o servidor adormecia ali."""
    c = join(tls_server.tls_port, "ana", tls=True)
    n = 300
    c.send_raw(b"".join(b'{"type":"MSG","text":"m%03d"}\n' % i for i in range(n)))
    got, fim = [], time.time() + 5
    while len(got) < 2 * n and time.time() < fim:
        m = c.next(timeout=fim - time.time())
        if m is None:
            break
        got.append(m)
    assert got.count(OK) == n
    assert [m["text"] for m in got if m.get("type") == "CHAT"] == [f"m{i:03d}" for i in range(n)]


def test_o_cliente_mostra_logo_uma_linha_maior_do_que_o_chunk(tls_server):
    """O mesmo caso do lado do cliente: um CHAT com 4096 bytes de texto ocupa
    mais do que um RX_CHUNK, e o que sobra fica dentro do SSL. Tem de aparecer
    JA, e nao quando chegar a mensagem seguinte.

    Nota: tirar o SSL_pending() NAO faz este falhar (M6.md, mutacao c) — o
    read_line() do cliente volta ao io_recv() antes de esperar, por isso le o
    resto sem passar pelo poll(). Fica como guarda, para o caso de alguem
    mudar essa ordem."""
    _, _, fp = tls_cert()
    rui = join(tls_server.port, "rui")
    p = subprocess.Popen([CLIENT, "-f", fp, "127.0.0.1", str(tls_server.tls_port), "ana"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL)
    try:
        assert tls_server.wait_log("entrou como ana")
        rui.recv_all()
        big = "y" * 4096
        rui.send({"type": "MSG", "text": big})
        out, fim = b"", time.time() + 3
        while big.encode() not in out and time.time() < fim:
            if select.select([p.stdout], [], [], 0.1)[0]:
                out += os.read(p.stdout.fileno(), 65536)
        assert big.encode() in out
    finally:
        p.communicate(b"/quit\n", timeout=10)


@pytest.mark.slow
def test_o_cliente_continua_a_ler_enquanto_espera_para_escrever():
    """M6.md §4, bug 7. Contra um servidor TLS falso que NUNCA le mas vai
    mandando "tick N": o nosso cliente recebe megabytes no stdin e o buffer
    de envio enche. Antes da correcao, a thread principal esperava pelo
    espaco COM o lock da ligacao, e a de leitura — que precisa do mesmo lock
    para o SSL_read — deixava de mostrar os ticks. Agora a espera e fora do
    lock: os ticks continuam a aparecer."""
    cert, key, fp = tls_cert()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    lst = socket.socket()
    lst.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)   # enche depressa
    lst.bind(("127.0.0.1", 0))
    lst.listen(1)
    port = lst.getsockname()[1]
    parar = threading.Event()

    def falso():
        raw, _ = lst.accept()
        with ctx.wrap_socket(raw, server_side=True) as s:
            buf = b""
            while b"\n" not in buf:                 # o NICK; e mais nada se le
                buf += s.recv(4096)
            s.sendall(b'{"type":"OK"}\n')
            i = 0
            while not parar.is_set():
                s.sendall(b'{"type":"CHAT","from":"srv","text":"tick %d","private":false}\n' % i)
                i += 1
                time.sleep(0.1)

    threading.Thread(target=falso, daemon=True).start()
    p = subprocess.Popen([CLIENT, "-k", "0", "-f", fp, "127.0.0.1", str(port), "ana"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, bufsize=0)  # sem flush no fim
    linha = ("x" * 4000 + "\n").encode()

    def escritor():                                 # ~20 MB: muito mais do que os buffers
        try:
            for _ in range(5000):
                p.stdin.write(linha)
        except (BrokenPipeError, ValueError, OSError):
            pass

    threading.Thread(target=escritor, daemon=True).start()
    try:
        out, fim = b"", time.time() + 8
        # Primeiro, dar tempo a que o envio fique mesmo preso (buffers cheios)...
        time.sleep(3)
        # ... e depois os ticks TEM de continuar a chegar.
        while time.time() < fim:
            if select.select([p.stdout], [], [], 0.1)[0]:
                out += os.read(p.stdout.fileno(), 65536)
        assert b"tick" in out
        ticks = [int(t) for t in re.findall(rb"tick (\d+)", out)]
        assert max(ticks) >= 70, f"o cliente parou de ler no tick {max(ticks)}"
    finally:
        parar.set()
        p.kill()
        p.wait()
        lst.close()


# ---- o que o M4 ja garantia, agora por TLS ----------------------------------

def leitor(conn, parar, guarda):
    """Le sem parar e guarda as mensagens (como o do test_concurrency, mas
    pelo Conn, que sabe do SSL pending)."""
    while not parar.is_set():
        m = conn.next(timeout=0.2)
        if m is not None:
            guarda.append(m)
        elif conn.closed:
            return


@pytest.mark.slow
def test_um_cliente_tls_que_nao_le_e_desligado_e_o_chat_continua(tls_server):
    """Regra 4 (slide 125), agora por TLS. Antes da correcao o servidor voltava
    o socket a bloqueante depois do handshake: o SSL_write ficava preso com o
    wlock na mao, o prazo de 5 s nunca disparava, e quem escrevia ficava
    encravado para sempre (M6.md, bug 4)."""
    a, b = join(tls_server.port, "ana"), join(tls_server.port, "rui")
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    s.connect(("127.0.0.1", tls_server.tls_port))
    lento = tls_socket(sock=s)
    lento.sendall(b'{"type":"NICK","nick":"lento"}\n')      # e nunca mais le
    assert tls_server.wait_log("entrou como lento")
    a.recv_all(); b.recv_all()

    recebido = {"ana": [], "rui": []}
    parar = threading.Event()
    ths = [threading.Thread(target=leitor, args=(c, parar, recebido[n]), daemon=True)
           for n, c in (("ana", a), ("rui", b))]
    for t in ths:
        t.start()
    for _ in range(3000):
        a.send({"type": "MSG", "text": "x" * 1000})
    assert tls_server.wait_log("nao le ha 5000 ms", timeout=10)
    b.send({"type": "MSG", "text": "segunda"})
    fim = time.time() + 10
    while time.time() < fim and not all(
            any(m.get("text") == "segunda" for m in recebido[n]) for n in recebido):
        time.sleep(0.1)
    parar.set()
    for t in ths:
        t.join()
    lento.close()
    for n in recebido:
        assert any(m.get("text") == "segunda" for m in recebido[n]), n
    assert any(m.get("text") == "lento left" for m in recebido["ana"])
    assert "deixou de ler (prazo de envio)" in tls_server.log()    # e nao "Success"


def test_um_record_tls_a_meio_nao_encrava_ninguem(tls_server):
    """5 bytes: o cabecalho de um record de 64 bytes que nunca chega. Com o
    socket bloqueante, o SSL_read do servidor ficava a espera do resto COM o
    wlock desse cliente — e todos os que lhe quisessem escrever (um CHAT, um
    "joined") ficavam presos atras. Um cliente parava o chat (M6.md, bug 4)."""
    loris = Conn(tls_server.tls_port, sock=tls_socket(tls_server.tls_port))
    loris.send({"type": "NICK", "nick": "loris"})
    assert loris.next() == OK
    os.write(loris.sock.fileno(), b"\x17\x03\x03\x00\x40")   # e mais nada

    time.sleep(0.3)
    a = join(tls_server.port, "ana")                 # o "ana joined" vai para o loris
    b = join(tls_server.port, "rui")
    a.send({"type": "MSG", "text": "ainda ha chat?"})
    assert chat("ana", "ainda ha chat?") in no_events(b.recv_all(1.0))
    assert chat("ana", "ainda ha chat?") in no_events(a.recv_all(1.0))
    loris.sock.close()


@pytest.mark.parametrize("modo", ["claro", "tls"])
@pytest.mark.parametrize("caso", ["quit", "413"])
def test_quando_o_servidor_fecha_o_fecho_chega_logo(tls_server, modo, caso):
    """M4 (typical-problems §2.2): quem fecha e o servidor -> resposta, FIN
    logo a seguir, e so depois esperar pelo cliente. Por TLS isto tem de ser
    close_notify + FIN; antes da correcao nao saia nada e o cliente so via o
    fecho 2 s depois (LINGER_MS), com o io_linger_drain() a espera dele
    (M6.md, bug 5). O 1 s de prazo aqui e o que separa os dois."""
    port = tls_server.tls_port if modo == "tls" else tls_server.port
    c = join(port, "ana", tls=(modo == "tls"))
    if caso == "quit":
        c.send({"type": "QUIT"})
        want = OK
    else:
        c.send_raw(b'{"type":"MSG","text":"' + b"x" * 70000 + b'"}\n')
        want = {"type": "ERROR", "code": 413, "reason": "line too long"}
    assert c.next() == want
    assert c.wait_closed(1.0) == "FIN"


def test_ctrl_c_com_clientes_tls_ligados(tls_server):
    """D24 tambem por TLS: cada um recebe o aviso e o fecho, e o servidor sai
    com 0."""
    cs = [join(tls_server.tls_port, f"n{i}", tls=True) for i in range(3)]
    claro = join(tls_server.port, "rui")
    for c in cs + [claro]:
        c.recv_all()
    tls_server.proc.send_signal(signal.SIGINT)
    for c in cs + [claro]:
        assert {"type": "EVENT", "text": "server shutting down"} in c.recv_all(1.0)
        assert c.wait_closed(3.0) == "FIN"
    # Esperar aqui que saia: se o finish() do conftest o visse ainda vivo,
    # mandava-lhe um SEGUNDO Ctrl+C, que termina logo (M4) — e o teste via -2.
    assert tls_server.proc.wait(timeout=10) == 0
