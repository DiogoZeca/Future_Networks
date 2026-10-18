"""
test_chat.py — o chat multiutilizador (M2) e os comandos (M3) com clientes
reais contra o nosso servidor. Era o scripts/probe_m2.py §1-5 e §8 e o
scripts/probe_m3.py §2, §3, §5, §6 e §8.
"""
import time

from helpers import OK, Conn, chat, join, no_events, welcome


# ---- M2: entrar, falar, errar, sair ---------------------------------------

def test_entrar_recebe_ok_e_boas_vindas(server):
    a = Conn(server.port)
    a.send({"type": "NICK", "nick": "ana"})
    assert a.recv_all() == [OK, welcome("ana")]          # D6, D22


def test_os_outros_veem_quem_entra_e_quem_sai(server):
    a = join(server.port, "ana")
    b = join(server.port, "rui")
    assert a.recv_all() == [{"type": "EVENT", "text": "rui joined"}]
    assert b.recv_all() == []                            # quem entra nao e avisado de si
    b.close()
    assert a.recv_all() == [{"type": "EVENT", "text": "rui left"}]


def test_broadcast_chega_a_todos_incluindo_o_emissor(server):
    a, b, c = (join(server.port, n) for n in ("ana", "rui", "zed"))
    a.recv_all(); b.recv_all()
    a.send({"type": "MSG", "text": "ola a todos"})
    msg = chat("ana", "ola a todos")
    assert a.recv_all() == [OK, msg]                     # D13, D5
    assert b.recv_all() == [msg]
    assert c.recv_all() == [msg]


def test_erro_so_para_quem_se_enganou(server):
    a, b = join(server.port, "ana"), join(server.port, "rui")
    a.recv_all()
    b.send({"type": "MSG"})                              # sem text
    assert [m["type"] for m in b.recv_all()] == ["ERROR"]
    assert a.recv_all() == []


def test_sair_antes_do_nick_nao_avisa_ninguem(server):
    a = join(server.port, "ana")
    Conn(server.port).close()
    assert a.recv_all() == []


def test_lista_cheia_da_503(server):
    # 70 ligacoes para 64 lugares: as que sobram recebem TODAS um 503, e so isso.
    socks = [Conn(server.port) for _ in range(70)]
    time.sleep(0.5)                      # o 503 ja chegou a quem o vai receber
    recusas = [r for r in (s.recv_all(0.01) for s in socks) if r]
    assert recusas
    assert all([(m["type"], m.get("code")) for m in r] == [("ERROR", 503)] for r in recusas)
    for s in socks:
        s.close()


# ---- M3: comandos -----------------------------------------------------------

def test_nick_em_uso_sem_olhar_a_maiusculas_e_a_ligacao_continua(server):
    ana = join(server.port, "ana")       # guardar a ligacao: sem referencia, o
                                         # Python fecha-a logo (e a ana sai)
    x = Conn(server.port)
    x.send({"type": "NICK", "nick": "ANA"})
    assert [m.get("code") for m in x.recv_all()] == [409]          # D18
    x.send({"type": "NICK", "nick": "Rui"})
    assert x.recv_all() == [OK, welcome("Rui")]                    # D19


def test_priv_so_o_destino_recebe(server):
    a, r, z = join(server.port, "ana"), join(server.port, "Rui"), join(server.port, "zed")
    for c in (a, r, z):
        c.recv_all()
    a.send({"type": "PRIV", "to": "rui", "text": "segredo"})       # destino canonico (D18)
    assert a.recv_all() == [OK]                                    # sem copia (D14)
    assert r.recv_all() == [chat("ana", "segredo", private=True)]
    assert z.recv_all() == []


def test_priv_a_si_proprio_e_a_quem_nao_existe(server):
    a = join(server.port, "ana")
    a.send({"type": "PRIV", "to": "ana", "text": "eu"})
    assert a.recv_all() == [OK, chat("ana", "eu", private=True)]
    a.send({"type": "PRIV", "to": "fantasma", "text": "x"})
    assert [(m["type"], m.get("code")) for m in a.recv_all()] == [("ERROR", 404)]


def test_list_ordenado_com_quem_pede(server):
    outros = [join(server.port, n) for n in ("zed", "Ana", "rui")]
    me = join(server.port, "bea")
    me.send({"type": "LIST"})
    assert no_events(me.recv_all()) == [{"type": "USERS", "nicks": ["Ana", "bea", "rui", "zed"]}]


def test_list_com_64_nicks_de_32_bytes_cabe_numa_linha(server):
    cs = [join(server.port, chr(ord("a") + i // 26) + chr(ord("a") + i % 26) + "x" * 30)
          for i in range(64)]
    cs[0].recv_all()
    cs[0].send({"type": "LIST"})
    users = no_events(cs[0].recv_all(1.0))
    assert len(users) == 1 and len(users[0]["nicks"]) == 64
    assert users[0]["nicks"] == sorted(users[0]["nicks"])


def test_quit_ok_fecho_e_o_que_vem_depois_e_ignorado(server):
    a, r = join(server.port, "ana"), join(server.port, "rui")
    a.recv_all()
    r.send_raw(b'{"type":"QUIT"}\n{"type":"MSG","text":"depois"}\n')
    assert r.next() == OK
    assert r.wait_closed() == "FIN"                               # M4: nunca RST
    assert a.recv_all() == [{"type": "EVENT", "text": "rui left"}]  # e nao o MSG
    r2 = join(server.port, "rui")                                  # o nick ficou livre


def test_linha_de_64k_chega_a_session_e_mais_um_byte_da_413(server):
    a = join(server.port, "ana")
    pre, pos = b'{"type":"MSG","text":"', b'"}\n'
    a.send_raw(pre + b"x" * (65536 - len(pre) - len(pos)) + pos)  # 65536 com o \n
    assert [(m.get("code"), m.get("reason")) for m in a.recv_all(1.0)] == [(400, "text too long")]
    a.send({"type": "MSG", "text": "ainda aqui"})
    assert [m["type"] for m in a.recv_all()] == ["OK", "CHAT"]


def test_mensagem_de_varias_linhas_chega_tal_e_qual(server):
    a, r = join(server.port, "ana"), join(server.port, "rui")
    a.recv_all()
    a.send({"type": "MSG", "text": "l1\nl2\x1b[2J"})
    assert [m.get("text") for m in r.recv_all()] == ["l1\nl2\x1b[2J"]
