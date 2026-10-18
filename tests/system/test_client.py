"""
test_client.py — o NOSSO cliente (./bin/client), com o stdin de um pipe.

  - contra o nosso servidor: comandos, 409, eco local do /msg, texto limpo
    (era o scripts/probe_m3.py §7-8);
  - contra servidores FALSOS que se portam como o de outro grupo podia
    portar-se (era o scripts/probe_client.py, que so mostrava o que o
    cliente imprimia — aqui cada caso diz o que se espera);
  - M4: keepalive, servidor que morre calado, Ctrl+C.

O cliente num terminal a serio esta no test_terminal.py.
"""
import signal
import socket
import subprocess
import threading
import time

import pytest

from helpers import CLIENT, Conn, join


def run_client(port, stdin, *opts, nick="ana", timeout=10):
    return subprocess.run([CLIENT, *opts, "127.0.0.1", str(port), nick], input=stdin,
                          capture_output=True, text=True, timeout=timeout)


# ---- contra o nosso servidor ---------------------------------------------

def test_comandos_409_e_eco_local(server):
    dono, outro = join(server.port, "ana"), join(server.port, "rui")
    dono.recv_all()
    # argv pede "ana" (em uso) -> 409 -> o cliente pede outro nick no stdin.
    p = run_client(server.port, "ana2\n/list\n/msg rui ola rui\n/lsit\n/quit\n")
    out = p.stdout
    assert "ja esta em uso" in out and "como ana2" in out           # D19
    assert "* welcome, ana2!" in out                                # D22
    assert "[online: 3] ana ana2 rui" in out                        # /list
    assert "<ana2 -> rui> (privado) ola rui" in out                 # eco local (D14)
    assert any(m.get("text") == "ola rui" and m.get("private") for m in outro.recv_all())
    assert "[ok]" not in out                                        # os OK nao se mostram
    assert "comando desconhecido" in out                            # /lsit
    assert not any(m.get("text") == "/lsit" for m in dono.recv_all())
    assert p.returncode == 0


def test_texto_de_varias_linhas_e_mostrado_indentado_e_limpo(server):
    ana = join(server.port, "ana")
    p = subprocess.Popen([CLIENT, "127.0.0.1", str(server.port), "zed"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(0.5)
    ana.send({"type": "MSG", "text": "linha 1\nlinha 2\x1b[2J"})
    time.sleep(0.4)
    out, _ = p.communicate("/quit\n", timeout=10)
    assert "<ana> linha 1\n    linha 2?[2J" in out                  # P32


# ---- contra servidores falsos --------------------------------------------

OK = b'{"type":"OK"}\n'


def chat(text):
    return b'{"type":"CHAT","from":"ana","text":"' + text + b'","private":false}\n'


def fake_server(behave):
    """Um servidor que responde a cada linha recebida com behave(i) (i = 0 e
    o NICK). behave devolve bytes, ("BYTES", b) para enviar byte a byte, ou
    (b, None) para enviar e fechar. Devolve a porta."""
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)

    def serve():
        conn, _ = listener.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        buf, i = b"", 0
        try:
            while True:
                while b"\n" not in buf:
                    d = conn.recv(4096)
                    if not d:
                        return
                    buf += d
                _, _, buf = buf.partition(b"\n")
                out = behave(i)
                if isinstance(out, tuple) and out[0] == "BYTES":
                    for b in out[1]:
                        conn.sendall(bytes([b]))
                        time.sleep(0.002)
                elif isinstance(out, tuple):
                    conn.sendall(out[0])
                    return
                else:
                    conn.sendall(out)
                i += 1
        except OSError:
            pass
        finally:
            time.sleep(0.2)
            conn.close()
            listener.close()

    threading.Thread(target=serve, daemon=True).start()
    return listener.getsockname()[1]


# (nome, comportamento, o que tem de aparecer, o codigo de saida)
FAKES = [
    ("sem_ok", lambda i: b"",
     ["sem resposta do servidor em 5 s"], 1),
    ("byte_a_byte", lambda i: ("BYTES", OK if i == 0 else chat(b"partido")),
     ["<ana> partido"], 0),
    ("colado_com_event", lambda i: OK + b'{"type":"EVENT","text":"rui joined"}\n' if i == 0
     else chat(b"depois do event"),
     ["* rui joined", "<ana> depois do event"], 0),
    ("nick_recusado", lambda i: b'{"type":"ERROR","code":409,"reason":"nick taken"}\n',
     ["[erro 409] nick taken", "ja esta em uso; escolhe outro"], 1),
    ("fecha_a_meio", lambda i: OK if i == 0 else (b'{"type":"CHA', None),
     ["ligado a"], 0),
    ("linha_gigante", lambda i: OK if i == 0 else b"Z" * 5000,
     ["ligado a"], 0),
    ("linha_acima_64k", lambda i: OK if i == 0 else b"Z" * 70000 + b"\n" + chat(b"depois da gigante"),
     ["descartada", "<ana> depois da gigante"], 0),
    ("ok_a_cada_pedido", lambda i: OK if i == 0 else OK + chat(b"linha 1\\nlinha 2\\u001b[31m"),
     ["<ana> linha 1\n    linha 2?[31m"], 0),
    ("crlf_e_ordem", lambda i: b'{"type":"OK"}\r\n' if i == 0 else
     b'{"private":false,"text":"crlf","from":"ana","type":"CHAT","extra":1}\r\n',
     ["<ana> crlf"], 0),
    ("nao_json", lambda i: OK if i == 0 else b"isto nao e json\n",
     ["mensagem nao suportada (invalid JSON)] isto nao e json"], 0),
]


@pytest.mark.parametrize("name,behave,expect,code", FAKES, ids=[f[0] for f in FAKES])
def test_cliente_contra_servidor_estranho(name, behave, expect, code):
    port = fake_server(behave)
    p = run_client(port, "ola\n", timeout=30)
    out = p.stdout + p.stderr
    for e in expect:
        assert e in out, f"{name}: falta {e!r} em:\n{out}"
    assert "[ok]" not in out
    assert p.returncode == code


# ---- M4 -----------------------------------------------------------------

def test_keepalive_mantem_um_utilizador_calado_ligado(make_server):
    """D2: com o servidor a desligar ao fim de 2 s sem mensagens e o cliente
    a mandar um LIST silencioso a cada 1 s, o cliente calado 5 s continua
    ligado — e o USERS dos keepalives nao aparece no ecra."""
    s = make_server("-v", "-i", "2")
    p = subprocess.Popen([CLIENT, "-k", "1", "127.0.0.1", str(s.port), "ana"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True)
    time.sleep(5)
    assert p.poll() is None
    out, err = p.communicate("/list\n/quit\n", timeout=10)
    assert out.count("[online: 1] ana") == 1          # so o do /list
    assert p.returncode == 0
    # Os silenciosos chegaram (o total menos o do /list). Pelo menos 2, e nao
    # "4 em 5 s": com o relogio a andar aos saltos de 250 ms, numa maquina
    # carregada sao 3 ou 4 — um teste que depende disso falha de vez em quando.
    # O que prova o keepalive e o cliente estar vivo aos 5 s, com o servidor a
    # desligar ao fim de 2 s.
    assert s.log().count('<- {"type":"LIST"}') - 1 >= 2
    assert "408" not in s.log()


def test_sem_keepalive_o_servidor_desliga_e_o_cliente_mostra_o_408(make_server):
    s = make_server("-i", "1")
    p = subprocess.Popen([CLIENT, "-k", "0", "127.0.0.1", str(s.port), "ana"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True)
    rc = p.wait(timeout=10)                            # o stdin continua aberto
    out = p.stdout.read() + p.stderr.read()
    assert "[erro 408] idle timeout" in out
    assert "o servidor fechou a ligacao" in out
    assert rc == 0


def test_cliente_da_pela_morte_silenciosa_do_servidor():
    """Um servidor que aceita o NICK e depois nao responde a nada, nem fecha
    (cabo arrancado do lado dele): o LIST silencioso fica sem resposta e o
    cliente desiste em ~2 intervalos, em vez de ficar a espera para sempre."""
    port = fake_server(lambda i: OK if i == 0 else b"")
    t = time.time()
    p = subprocess.Popen([CLIENT, "-k", "1", "127.0.0.1", str(port), "ana"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True)
    rc = p.wait(timeout=10)
    assert rc == 1
    assert "o servidor nao responde" in p.stderr.read()
    assert time.time() - t < 5


def test_ctrl_c_no_cliente_manda_quit(server):
    """M4: Ctrl+C = /quit. O servidor ve um QUIT (e nao uma queda), o rui ve
    o "ana left", e o cliente sai com 0."""
    rui = join(server.port, "rui")
    p = subprocess.Popen([CLIENT, "127.0.0.1", str(server.port), "ana"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True)
    assert server.wait_log("entrou como ana")
    rui.recv_all()
    p.send_signal(signal.SIGINT)
    rc = p.wait(timeout=5)
    assert rc == 0
    assert "Ctrl+C: a sair (QUIT)" in p.stderr.read()
    assert server.wait_log("(ana) saiu: QUIT")
    assert rui.recv_all() == [{"type": "EVENT", "text": "ana left"}]
