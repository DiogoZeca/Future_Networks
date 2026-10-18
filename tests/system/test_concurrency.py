"""
test_concurrency.py — as corridas e os clientes que nao leem (M2, M3).
Sao os testes demorados (marcados `slow`): cada um so apanha o bug que
procura se repetir a situacao muitas vezes. Era o scripts/probe_m2.py §6-7
e o scripts/probe_m3.py §1 e §4.
"""
import json
import select
import socket
import threading
import time

import pytest

from helpers import OK, Conn, join

pytestmark = pytest.mark.slow


def leitor(conn, parar, guarda=None):
    """Le sem parar (quem esta no chat tem de ler, senao e desligado).
    Espera com select() e nao com settimeout(): o timeout e do socket inteiro
    e tambem se aplicaria aos sendall() de outra thread."""
    s, buf = conn.sock, b""
    s.settimeout(None)
    while not parar.is_set():
        if not select.select([s], [], [], 0.2)[0]:
            continue
        try:
            d = s.recv(65536)
        except OSError:
            return
        if not d:
            return
        if guarda is None:
            continue
        buf += d
        while b"\n" in buf:
            l, buf = buf.split(b"\n", 1)
            guarda.append(json.loads(l))


def test_a_primeira_linha_depois_do_nick_e_sempre_o_ok(server):
    """D9: 1000 entradas enquanto alguem fala sem parar. A janela do bug do
    M2 era de ~1% das entradas, dai serem tantas."""
    a, b = join(server.port, "ana"), join(server.port, "rui")
    parar = threading.Event()

    def tagarela():
        while not parar.is_set():
            a.send({"type": "MSG", "text": "z"})

    ths = [threading.Thread(target=f, args=x, daemon=True)
           for f, x in ((tagarela, ()), (leitor, (a, parar)), (leitor, (b, parar)))]
    for t in ths:
        t.start()
    maus = 0
    for i in range(1000):
        s = Conn(server.port)
        s.send({"type": "NICK", "nick": f"n{i}"})
        if s.next(timeout=1.5) != OK:
            maus += 1
        s.sock.close()
    parar.set()
    for t in ths:
        t.join()
    assert maus == 0


def test_um_cliente_que_nao_le_e_desligado_e_o_chat_continua(server):
    """Regra 4 (slide 125): ~3 MB para um cliente que nunca le. Ao fim de 5 s
    e desligado, e os outros continuam a falar e a ouvir."""
    a, b = join(server.port, "ana"), join(server.port, "rui")
    lento = socket.socket()
    lento.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    lento.connect(("127.0.0.1", server.port))
    lento.sendall(b'{"type":"NICK","nick":"lento"}\n')      # e nunca mais le
    time.sleep(0.3)
    a.recv_all(); b.recv_all()

    recebido = {"ana": [], "rui": []}
    parar = threading.Event()
    ths = [threading.Thread(target=leitor, args=(c, parar, recebido[n]), daemon=True)
           for n, c in (("ana", a), ("rui", b))]
    for t in ths:
        t.start()
    for _ in range(3000):
        a.send({"type": "MSG", "text": "x" * 1000})
    time.sleep(6.5)                                        # SEND_TIMEOUT_MS = 5000
    b.send({"type": "MSG", "text": "ainda ha alguem?"})
    b.send({"type": "MSG", "text": "segunda"})
    # Esperar ATE a "segunda" chegar aos dois, e nao um tempo fixo (M3.md, P29).
    fim = time.time() + 10
    while time.time() < fim and not all(
            any(m.get("text") == "segunda" for m in recebido[n]) for n in recebido):
        time.sleep(0.1)
    parar.set()
    for t in ths:
        t.join()
    lento.close()
    assert any(m.get("text") == "lento left" for m in recebido["ana"])
    assert any(m.get("text") == "segunda" for m in recebido["rui"])
    assert any(m.get("text") == "segunda" for m in recebido["ana"])
    assert server.wait_log("nao le ha 5000 ms")


def test_nick_igual_ao_mesmo_tempo_so_um_ganha(server):
    """P20: 50 rondas de 8 NICK "dup" no mesmo instante — sempre 1 OK e 7 x
    409 (verificar e reservar e UMA operacao no roster)."""
    mas = 0
    for _ in range(50):
        socks = [Conn(server.port) for _ in range(8)]
        time.sleep(0.05)                                   # todas aceites antes do tiro
        barreira = threading.Barrier(len(socks))
        res = [None] * len(socks)

        def tenta(i):
            barreira.wait()
            socks[i].send({"type": "NICK", "nick": "dup"})
            res[i] = socks[i].next(timeout=1.0)

        ths = [threading.Thread(target=tenta, args=(i,)) for i in range(len(socks))]
        for t in ths:
            t.start()
        for t in ths:
            t.join()
        oks = sum(1 for r in res if r == OK)
        e409 = sum(1 for r in res if r and r.get("code") == 409)
        if oks != 1 or e409 != len(socks) - 1:
            mas += 1
        for s in socks:
            s.close()
    assert mas == 0


def test_priv_para_quem_entra_e_sai_sem_parar(server):
    """P22: 1500 PRIV para um cliente que entra e sai sem parar. So OK ou
    404, nunca outra coisa, e o servidor sobrevive (refcount no find)."""
    a = join(server.port, "ana")
    parar = threading.Event()

    def pisca():
        while not parar.is_set():
            try:
                s = Conn(server.port)
                s.send({"type": "NICK", "nick": "pisca"})
                s.next(timeout=0.3)
                s.sock.close()
            except OSError:
                pass

    t = threading.Thread(target=pisca, daemon=True)
    t.start()
    aceites = nao_ha = estranhas = 0
    for i in range(1500):
        a.send({"type": "PRIV", "to": "pisca", "text": str(i)})
        m = a.next(timeout=2.0, skip_events=True)
        if m == OK:
            aceites += 1
        elif m and m.get("code") == 404:
            nao_ha += 1
        else:
            estranhas += 1
    parar.set()
    t.join()
    assert estranhas == 0
    assert aceites > 0 and nao_ha > 0                      # a corrida aconteceu mesmo
    assert server.alive()
