"""
test_discovery.py — a descoberta UDP (M5, slide 123: "The server answers a
UDP broadcast (DISCOVER) with its TCP port; the client finds servers on the
LAN with no address typed in").

  - o SERVIDOR, com datagramas mandados daqui: a resposta, o que conta como
    pedido (e o silencio a tudo o resto), os users, -N, -u, porta ocupada,
    dois servidores na mesma porta, rajadas e fuzz;
  - o CLIENTE (./bin/client sem endereco), contra servidores nossos e contra
    servidores FALSOS que respondem como outro grupo podia responder.

Tudo na loopback: o broadcast vai para 127.255.255.255, que e um broadcast a
serio mas so entre os programas desta maquina (test_net #103) — por isso
estes testes nao dependem da rede nem da firewall da maquina onde correm. A
descoberta numa rede a serio (255.255.255.255, varias interfaces, firewall)
prova-se a mao: docs/milestones/M5.md, "Como reproduzir".
"""
import json
import random
import socket
import subprocess
import threading
import time

import pytest

from helpers import CLIENT, SERVER, Conn, free_udp_port, join

LOOPBACK_BCAST = "127.255.255.255"
REQUEST = b'{"type":"DISCOVER"}'


def ask(port, data=REQUEST, dst="127.0.0.1", timeout=0.5):
    """Manda um datagrama e devolve [(bytes, (ip, porta)), ...] de tudo o que
    chegar ate ficar `timeout` s sem nada."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(timeout)
    s.sendto(data, (dst, port))
    out = []
    try:
        while True:
            out.append(s.recvfrom(4096))
    except socket.timeout:
        pass
    s.close()
    return out


def ask_json(port, **kw):
    got = ask(port, **kw)
    assert len(got) == 1, f"esperava 1 resposta, chegaram {len(got)}: {got}"
    return json.loads(got[0][0])


# ---- o servidor -----------------------------------------------------------

def test_responde_ao_discover_com_a_porta_tcp(server):
    got = ask(server.udp_port)
    assert len(got) == 1
    data, (ip, port) = got[0]
    assert data.endswith(b"\n")                                     # D28
    m = json.loads(data)
    assert m["type"] == "SERVER" and m["tcp_port"] == server.port   # o slide
    assert m["users"] == 0
    assert m["name"] == socket.gethostname()[:64]                   # D29: o hostname
    assert (ip, port) == ("127.0.0.1", server.udp_port)             # vem da porta UDP


def test_texto_simples_e_broadcast_da_loopback(server):
    assert ask_json(server.udp_port, data=b"DISCOVER\r\n")["tcp_port"] == server.port
    m = ask_json(server.udp_port, dst=LOOPBACK_BCAST)
    assert m["tcp_port"] == server.port


def test_users_conta_so_quem_esta_no_chat(server):
    ana, rui = join(server.port, "ana"), join(server.port, "rui")
    mudo = Conn(server.port)                                        # sem NICK
    time.sleep(0.1)
    assert ask_json(server.udp_port)["users"] == 2
    rui.close()
    time.sleep(0.2)
    assert ask_json(server.udp_port)["users"] == 1
    ana.close()
    mudo.close()


def test_nome_dado_com_N(make_server):
    s = make_server("-N", "sala do Zé")
    assert ask_json(s.udp_port)["name"] == "sala do Zé"


@pytest.mark.parametrize("data", [
    b"",
    b"discover",
    b'{"type":"discover"}',
    b'{"type":"SERVER","tcp_port":5000}',            # sem ping-pong entre servidores
    b'{"type":"NICK","nick":"ana"}',
    b'{"type":"DISCOVER"}{"type":"DISCOVER"}',
    b'["DISCOVER"]',
    b'{"type":"DISCOVER","x":"\xff"}',               # UTF-8 invalido
    b"DISCOVER\x00",
    b'{"type":"DISCOVER","pad":"' + b"a" * 600 + b'"}',   # > 512 bytes
])
def test_ignora_o_que_nao_e_discover(server, data):
    assert ask(server.udp_port, data=data, timeout=0.3) == []
    assert ask_json(server.udp_port)["tcp_port"] == server.port    # e continua


def test_fuzz_udp_sem_respostas_e_o_chat_continua(server):
    rnd = random.Random(5)                       # semente fixa: reproduzivel
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.5)
    for _ in range(500):
        n = rnd.randrange(0, 700)
        s.sendto(bytes(rnd.randrange(256) for _ in range(n)), ("127.0.0.1", server.udp_port))
    with pytest.raises(socket.timeout):
        s.recvfrom(4096)
    s.close()
    assert ask_json(server.udp_port)["tcp_port"] == server.port
    join(server.port, "ana").close()


def test_rajada_de_pedidos_nao_prende_o_accept(server):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for _ in range(300):
        s.sendto(REQUEST, ("127.0.0.1", server.udp_port))
    t0 = time.time()
    join(server.port, "ana").close()             # no meio da rajada
    assert time.time() - t0 < 2
    s.settimeout(0.5)
    n = 0
    try:
        while True:
            s.recvfrom(4096)
            n += 1
    except socket.timeout:
        pass
    s.close()
    assert n >= 100, f"so {n} respostas a 300 pedidos"   # UDP: pode perder alguns


def test_log_debug_mostra_cada_pedido(make_server):
    s = make_server("-v")
    ask(s.udp_port)
    ask(s.udp_port, data=b"lixo")
    assert s.wait_log("descoberta: DISCOVER de 127.0.0.1:")
    assert s.wait_log("nao e DISCOVER: ignorado")
    assert f'descoberta UDP na porta {s.udp_port}' in s.log()


def test_u0_desliga_a_descoberta(make_server):
    s = make_server("-u", "0")
    assert s.wait_log("descoberta UDP desligada (-u 0)")
    join(s.port, "ana").close()                  # o chat funciona na mesma


def test_porta_udp_ocupada_avisa_e_o_chat_continua(make_server):
    other = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)    # sem SO_REUSEADDR
    other.bind(("0.0.0.0", 0))
    s = make_server("-u", str(other.getsockname()[1]))
    assert s.wait_log("WARN  descoberta UDP desligada")
    assert "in use" in s.log()
    join(s.port, "ana").close()
    other.close()


def test_dois_servidores_na_mesma_porta_udp(make_server):
    u = free_udp_port()
    a, b = make_server("-u", str(u), "-N", "a"), make_server("-u", str(u), "-N", "b")
    got = ask(u, dst=LOOPBACK_BCAST)
    ports = sorted(json.loads(d)["tcp_port"] for d, _ in got)
    assert ports == sorted([a.port, b.port])     # o broadcast chega aos dois


@pytest.mark.parametrize("args", [["-N", ""], ["-N", "a" * 65], ["-N", "a\tb"],
                                  ["-u", "70000"], ["-u", "x"]])
def test_opcoes_invalidas_do_servidor(args):
    p = subprocess.run([SERVER, *args, "0"], capture_output=True, text=True, timeout=5)
    assert p.returncode != 0 and p.stderr


# ---- o cliente --------------------------------------------------------------

def discover_client(udp_port, stdin, *opts, wait_ms=400, timeout=10):
    """O nosso cliente SEM endereco, a procurar por broadcast na loopback."""
    return subprocess.run([CLIENT, "-u", str(udp_port), "-w", str(wait_ms),
                           "-b", LOOPBACK_BCAST, *opts],
                          input=stdin, capture_output=True, text=True, timeout=timeout)


class FakeServers:
    """Um 'servidor' de descoberta falso: a cada pedido responde com cada um
    dos datagramas dados — o que outro grupo podia mandar. Conta os pedidos."""

    def __init__(self, replies):
        self.replies = replies
        self.requests = []
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", 0))
        self.sock.settimeout(0.1)
        self.port = self.sock.getsockname()[1]
        self.stop = threading.Event()
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
        while not self.stop.is_set():
            try:
                data, addr = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            self.requests.append(data)
            for r in self.replies:
                self.sock.sendto(r, addr)

    def close(self):
        self.stop.set()
        self.t.join(2)
        self.sock.close()


def test_cliente_sem_servidores_explica_e_sai():
    t0 = time.time()
    p = discover_client(free_udp_port(), "")
    assert p.returncode == 1
    assert "nenhum servidor respondeu" in p.stderr
    assert "ufw allow" in p.stderr                       # a firewall (slide 125)
    assert time.time() - t0 < 3


def test_cliente_um_servidor_liga_logo(server):
    obs = join(server.port, "obs")
    p = discover_client(server.udp_port, "ana\nola a todos\n/quit\n")
    assert p.returncode == 0, p.stderr
    assert f"encontrado: 1) 127.0.0.1:{server.port}" in p.stdout
    assert "escolhe" not in p.stdout                     # um so: nao pergunta
    assert f"ligado a 127.0.0.1:{server.port} como ana" in p.stdout
    assert any(m.get("text") == "ola a todos" for m in obs.recv_all())


def test_cliente_varios_lista_ordenada_e_escolha(make_server):
    u = free_udp_port()
    beta = make_server("-u", str(u), "-N", "beta")
    alfa = make_server("-u", str(u), "-N", "Alfa")
    p = discover_client(u, "9\nx\n2\nrui\n/quit\n")
    out = p.stdout
    assert p.returncode == 0, p.stderr
    assert "2 servidores encontrados" in out
    i1 = out.index(f"1) 127.0.0.1:{alfa.port}  Alfa  (0 online)")   # por nome,
    i2 = out.index(f"2) 127.0.0.1:{beta.port}  beta  (0 online)")   # sem contar maiusculas
    assert i1 < i2
    assert out.count("escreve um numero de 1 a 2") == 2              # "9" e "x"
    assert beta.wait_log("entrou como rui")                          # o 2 e o beta
    assert "entrou como" not in alfa.log()


def test_cliente_eof_na_escolha_nao_liga(make_server):
    u = free_udp_port()
    a, b = make_server("-u", str(u), "-N", "a"), make_server("-u", str(u), "-N", "b")
    p = discover_client(u, "")
    assert p.returncode == 1
    time.sleep(0.2)
    assert "entrou como" not in a.log() + b.log()


def test_cliente_contra_respostas_de_outros_grupos(server):
    """Outro formato ({"port":N}), um IP falso na mensagem, lixo, um datagrama
    grande de mais, outro tipo e um name com um ESC: o cliente fica so com o
    que presta, usa o IP de ORIGEM, e mostra cada servidor uma vez."""
    fake = FakeServers([
        b'{"port":%d,"ip":"10.9.9.9"}' % server.port,
        b"lixo",
        b'{"type":"SERVER","tcp_port":1,"pad":"' + b"a" * 600 + b'"}',
        b'{"type":"OK","tcp_port":2}',
        b'{"type":"SERVER","tcp_port":3,"name":"falso \\u001b[2J","users":7}',
    ])
    try:
        p = discover_client(fake.port, "2\nana\n/quit\n", wait_ms=500)
    finally:
        fake.close()
    out = p.stdout
    assert p.returncode == 0, p.stderr
    assert len(fake.requests) == 3                                   # 3 envios (D30)
    assert set(fake.requests) == {REQUEST}                           # 19 bytes, sem '\n'
    assert "2 servidores encontrados" in out                         # 3 x cada: 1 x
    assert "1) 127.0.0.1:3  falso ?[2J  (7 online)" in out            # ESC limpo
    assert f"2) 127.0.0.1:{server.port}\n" in out                    # sem name nem users
    assert "10.9.9.9" not in out
    assert f"ligado a 127.0.0.1:{server.port} como ana" in out       # o IP de origem


@pytest.mark.parametrize("opts,msg", [(["-u", "0"], "-u"), (["-w", "0"], "-w"),
                                      (["-b", "x"], "-b"), (["-b", "::1"], "-b")])
def test_opcoes_invalidas_do_cliente(opts, msg):
    p = subprocess.run([CLIENT, *opts], capture_output=True, text=True, timeout=5)
    assert p.returncode == 1 and p.stderr.startswith(msg)
