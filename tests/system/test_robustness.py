"""
test_robustness.py — o M4 (slide 123): "Idle timeout, clean handling of
abrupt disconnects and bad JSON, Ctrl+C shutdown that closes sockets,
logging". Os casos de fecho abrupto do antigo scripts/probe_m0.py (RST e
half-close) estao aqui, agora com passa/falha.
"""
import json
import os
import random
import re
import signal
import socket
import struct
import subprocess
import time

import pytest

from helpers import CLIENT, OK, Conn, join, welcome


# ---- idle timeout (D23) ---------------------------------------------------

def test_quem_nao_faz_nick_a_tempo_recebe_408_e_e_fechado(make_server):
    s = make_server("-n", "1")
    c = Conn(s.port)
    t = time.time()
    assert c.next(timeout=3) == {"type": "ERROR", "code": 408, "reason": "nick timeout"}
    assert c.wait_closed() == "FIN"
    assert 0.9 < time.time() - t < 2.0
    assert s.wait_log("saiu sem nick: sem NICK em 1 s (408)")


def test_linhas_recusadas_nao_renovam_o_prazo_do_nick(make_server):
    """Lixo de 0,2 em 0,2 s durante 2,5 s, sem nunca fazer NICK: o 408 chega
    ao fim de ~1 s na mesma (o prazo do NICK conta desde a ligacao)."""
    s = make_server("-n", "1")
    c = Conn(s.port)
    t, quando = time.time(), None
    while time.time() - t < 2.5 and quando is None:
        try:
            c.send_raw(b"lixo\n")
        except OSError:
            break
        for m in c.recv_all(0.2):
            if m.get("code") == 408:
                quando = time.time() - t
    assert quando is not None and quando < 1.6, quando


def test_mensagens_renovam_o_prazo_e_o_silencio_da_408(make_server):
    s = make_server("-i", "1")
    a, r = join(s.port, "ana"), join(s.port, "rui")
    for _ in range(6):                                  # 3 s a falar: nunca 408
        a.send({"type": "LIST"})
        r.send({"type": "LIST"})
        time.sleep(0.5)
    assert not any(m.get("code") == 408 for m in a.recv_all(0.1))
    r.recv_all(0.1)
    # Agora so o rui fala: a ana fica calada e cai.
    for _ in range(4):
        r.send({"type": "LIST"})
        time.sleep(0.4)
    assert {"type": "ERROR", "code": 408, "reason": "idle timeout"} in a.recv_all(0.5)
    assert a.closed
    assert {"type": "EVENT", "text": "ana left"} in r.recv_all()
    assert s.wait_log("(ana) saiu: inativo ha 1 s (408)")


# ---- fechar sem RST (linger_drain) ----------------------------------------

def test_o_413_chega_sempre_e_o_fecho_e_fin(server):
    """Antes do M4, fechar com o resto da linha por ler gerava RST, e o RST
    engolia o 413 de vez em quando (M0 nota 4). Agora: 20 em 20."""
    for i in range(20):
        c = join(server.port, f"g{i}")
        try:
            c.send_raw(b'{"type":"MSG","text":"' + b"y" * 70000 + b'"}\n' + b"z" * 50000)
        except OSError:
            pass
        assert c.next(timeout=3) == {"type": "ERROR", "code": 413, "reason": "line too long"}
        assert c.wait_closed() == "FIN"
        c.sock.close()


def test_o_ok_ao_quit_chega_mesmo_com_lixo_atras(server):
    for i in range(20):
        c = join(server.port, f"q{i}")
        c.send_raw(b'{"type":"QUIT"}\n' + b"x" * 200000)
        assert c.next(timeout=3) == OK
        assert c.wait_closed() == "FIN"
        c.sock.close()


# ---- fechos abruptos ------------------------------------------------------

def test_cliente_que_aborta_com_rst(server):
    a = join(server.port, "ana")
    x = join(server.port, "rst")
    a.recv_all()
    x.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    x.send_raw(b'{"type":"MSG","text":"a meio')        # sem '\n', e fecha com RST
    x.sock.close()
    assert a.recv_all() == [{"type": "EVENT", "text": "rst left"}]
    assert server.wait_log("(rst) saiu: ligacao abortada (RST)")
    assert server.alive()


def test_cliente_que_fecha_so_o_envio(server):
    """Half-close: o servidor ve recv()==0, trata como saida e fecha tambem."""
    a, x = join(server.port, "ana"), join(server.port, "meio")
    a.recv_all()
    x.sock.shutdown(socket.SHUT_WR)
    assert x.wait_closed() == "FIN"
    assert a.recv_all() == [{"type": "EVENT", "text": "meio left"}]
    assert server.wait_log("(meio) saiu: fechou a ligacao (FIN)")


def test_kill_9_ao_nosso_cliente(server):
    a = join(server.port, "ana")
    p = subprocess.Popen([CLIENT, "127.0.0.1", str(server.port), "morto"],
                         stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    assert server.wait_log("entrou como morto")
    a.recv_all()
    p.kill()
    p.wait()
    assert a.recv_all(1.0) == [{"type": "EVENT", "text": "morto left"}]
    assert server.alive()


# ---- JSON invalido --------------------------------------------------------

def lixo(rng):
    """Uma linha que NAO e uma mensagem valida (sem '\\n' la dentro)."""
    tipo = rng.randrange(8)
    if tipo == 0:                                       # bytes ao calhas
        b = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 200)))
    elif tipo == 1:                                     # JSON cortado
        b = b'{"type":"MSG","text":"' + b"a" * rng.randrange(50)
    elif tipo == 2:                                     # muito aninhado
        d = rng.randrange(500, 5000)
        b = b"[" * d + b"]" * d
    elif tipo == 3:                                     # tipos errados
        b = json.dumps(rng.choice([{"type": 1}, {"type": "MSG", "text": 5},
                                   {"type": "PRIV", "to": [], "text": "x"},
                                   {"type": "NICK", "nick": None}, [1, 2], "MSG",
                                   {"type": "MSG", "text": "x" * 5000}])).encode()
    elif tipo == 4:                                     # UTF-8 invalido
        b = b'{"type":"MSG","text":"\xc3\x28\xff"}'
    elif tipo == 5:                                     # numeros enormes
        # O numero vai no "text", que tem de ser string: e 400 com qualquer
        # cJSON. Ate ao M6 ia num campo a mais ("n":999...), e ai a resposta
        # dependia da VERSAO do cJSON: ate a 1.7.18 um numero com mais de 63
        # caracteres e JSON invalido (400); a partir da 1.7.19 (que alocou o
        # buffer) e um MSG valido com um campo desconhecido, que a spec manda
        # ignorar (OK + CHAT) — e o teste falhava no Arch e passava no Ubuntu.
        b = b'{"type":"MSG","text":' + b"9" * 400 + b"}"
    elif tipo == 6:                                     # NUL no meio
        b = b'{"type":"MSG",\x00"text":"x"}'
    else:                                               # duas mensagens numa linha
        b = b'{"type":"LIST"}{"type":"LIST"}'
    b = b.replace(b"\n", b"?").replace(b"\r", b"?")
    return b if b.strip() else b"?"


def test_lixo_de_toda_a_especie_da_400_e_a_ligacao_continua(server):
    rng = random.Random(4)                              # sempre o mesmo lixo
    a = join(server.port, "ana")
    linhas = [lixo(rng) for _ in range(500)]
    a.send_raw(b"".join(l + b"\n" for l in linhas))
    erros = []
    while len(erros) < len(linhas):
        m = a.next(timeout=5)
        assert m is not None, f"so {len(erros)} respostas a {len(linhas)} linhas"
        erros.append(m)
    assert all(m["type"] == "ERROR" and m["code"] == 400 for m in erros)
    a.send({"type": "MSG", "text": "ainda aqui"})
    assert [m["type"] for m in a.recv_all()] == ["OK", "CHAT"]
    assert server.alive()


# ---- Ctrl+C no servidor (D24) ---------------------------------------------

def test_ctrl_c_avisa_toda_a_gente_fecha_e_sai_com_0(server):
    a, b = join(server.port, "ana"), join(server.port, "rui")
    pre = Conn(server.port)                             # ainda sem NICK
    p = subprocess.Popen([CLIENT, "127.0.0.1", str(server.port), "zed"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True)
    assert server.wait_log("entrou como zed")
    a.recv_all(); b.recv_all()

    t = time.time()
    assert server.stop(signal.SIGINT) == 0
    assert time.time() - t < 3
    shut = {"type": "EVENT", "text": "server shutting down"}
    for c in (a, b, pre):
        assert c.recv_all() == [shut]                   # e nenhum "x left"
        assert c.wait_closed() == "FIN"
    out, err = p.communicate(timeout=5)                 # o nosso cliente mostra e sai
    assert "* server shutting down" in out
    assert "o servidor fechou a ligacao" in err and p.returncode == 0
    log = server.log()
    assert "SIGINT (Ctrl+C): a encerrar" in log
    assert "encerrado: todas as ligacoes fechadas" in log


def test_sigterm_tambem_encerra_e_a_porta_fica_logo_livre(make_server):
    s = make_server()
    a = join(s.port, "ana")
    assert s.stop(signal.SIGTERM) == 0
    assert a.recv_all() == [{"type": "EVENT", "text": "server shutting down"}]
    # Sem "Address already in use" (SO_REUSEADDR, slide 125): mesma porta, ja.
    s2 = make_server(port=s.port)
    join(s2.port, "rui")


def test_ctrl_c_com_um_cliente_que_nao_le(server):
    """Um cliente parado com os buffers cheios nao pode prender o encerramento
    para sempre: no pior caso, um envio a meio (5 s) + o linger (1 s)."""
    a = join(server.port, "ana")
    lento = socket.socket()
    lento.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    lento.connect(("127.0.0.1", server.port))
    lento.sendall(b'{"type":"NICK","nick":"lento"}\n')
    time.sleep(0.2)
    for _ in range(300):                                # encher-lhe os buffers
        a.send({"type": "MSG", "text": "x" * 1000})
    a.recv_all(0.5)
    t = time.time()
    assert server.stop(signal.SIGINT, timeout=15) == 0
    assert time.time() - t < 9
    lento.close()


# ---- logging --------------------------------------------------------------

LINHA = re.compile(r"^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3} (ERROR|WARN |INFO |DEBUG) \S")


def test_cada_linha_do_log_tem_hora_e_nivel(server):
    a = join(server.port, "ana")
    a.send_raw(b"lixo\n")
    a.close()
    server.stop()
    linhas = server.log().splitlines()
    assert linhas and all(LINHA.match(l) for l in linhas), linhas
    assert any("-> ERROR 400 (invalid JSON)" in l for l in linhas)
    assert any("(ana) saiu: fechou a ligacao (FIN)" in l for l in linhas)


def test_q_so_avisos_e_v_cada_mensagem(make_server):
    q = make_server("-q")
    join(q.port, "ana").close()
    q.stop()
    assert "INFO" not in q.log() and "DEBUG" not in q.log()

    v = make_server("-v")
    a = join(v.port, "ana")
    a.send({"type": "MSG", "text": "ola"})
    a.recv_all()
    assert v.wait_log('DEBUG ') and v.wait_log('<- {"type": "MSG", "text": "ola"}')
