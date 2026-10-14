#!/usr/bin/env python3
"""
probe_m3.py — verificacao dos comandos do M3 com clientes reais.

Como o probe_m2.py: arranca o seu proprio ./bin/server, liga clientes a serio
e verifica o que CADA UM recebe — e o que NAO deve receber. E passa/falha.

Seccoes:
  1. nicks unicos com corrida: 8 clientes pedem o mesmo nick no mesmo instante,
     50 vezes — ganha sempre exatamente UM (os outros: 409)
  2. maiusculas (D18): "ANA" e recusado se "ana" estiver
  3. PRIV: chega ao destino, a copia ao emissor, e a mais NINGUEM
  4. PRIV para quem entra e sai sem parar: nem crash nem respostas estranhas
     (o destino pode sair entre o "existe?" e o envio — refcount do find)
  5. LIST com a lista cheia: 64 nicks de 32 bytes, ordenados, numa linha
  6. QUIT: OK, fecho, "x left" aos outros; o que vem colado DEPOIS do QUIT e
     ignorado; o nick fica livre a seguir
  7. o nosso cliente (./bin/client) de ponta a ponta: 409 -> pede outro nick,
     /list, /msg, comando desconhecido nao vai para o chat, /quit

Uso:
    python3 scripts/probe_m3.py          # porta 5310 por omissao

Espera encontrar ./bin/server e ./bin/client (correr a partir da raiz).
"""
import atexit, json, socket, subprocess, sys, tempfile, threading, time

PORT = sys.argv[1] if len(sys.argv) > 1 else "5310"


def conn():
    s = socket.create_connection(("127.0.0.1", int(PORT)), timeout=3)
    s.settimeout(1.5)
    return s


def send(s, obj):
    s.sendall((json.dumps(obj) + "\n").encode())


def recv_all(s, timeout=0.5):
    """Le tudo o que chegar ate ao timeout. Devolve lista de dicts (e "CLOSED"
    no fim se o servidor fechou)."""
    s.settimeout(timeout); out = []; buf = b""
    try:
        while True:
            d = s.recv(65536)
            if not d:
                out.append("CLOSED"); break
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if line.strip():
                    out.append(json.loads(line))
    except socket.timeout:
        pass
    return out


def entra(nick):
    s = conn(); send(s, {"type": "NICK", "nick": nick})
    r = recv_all(s, 0.3)
    assert r[:1] == [{"type": "OK"}], f"{nick} nao entrou: {r}"
    return s


def sem_events(msgs):
    return [m for m in msgs if not (isinstance(m, dict) and m.get("type") == "EVENT")]


# O log vai para um ficheiro (nao para um pipe que ninguem le: ver probe_m2).
log = tempfile.TemporaryFile(mode="w+")
srv = subprocess.Popen(["./bin/server", PORT], stdout=log,
                       stderr=subprocess.STDOUT, text=True)
atexit.register(lambda: srv.poll() is None and srv.kill())
time.sleep(0.5)
if srv.poll() is not None:
    log.seek(0); print(log.read())
    sys.exit(f"o servidor nao arrancou na porta {PORT} (ja ha outro a correr?)")

fails = []
def check(name, got, want):
    ok = got == want
    print(f"  {'ok  ' if ok else 'FALHA'} {name}")
    if not ok:
        print(f"        obtido:   {got}\n        esperado: {want}")
        fails.append(name)

# --- 1. nicks unicos, com corrida -------------------------------------------
print("1. nicks unicos com corrida")
mas = 0
for ronda in range(50):
    socks = [conn() for _ in range(8)]
    time.sleep(0.05)                          # todas aceites antes do tiro
    barreira = threading.Barrier(len(socks))
    res = [None] * len(socks)
    def tenta(i):
        barreira.wait()
        send(socks[i], {"type": "NICK", "nick": "dup"})
        r = recv_all(socks[i], 0.4)
        res[i] = r[0] if r else None
    ths = [threading.Thread(target=tenta, args=(i,)) for i in range(len(socks))]
    for t in ths: t.start()
    for t in ths: t.join()
    oks = sum(1 for r in res if r == {"type": "OK"})
    e409 = sum(1 for r in res if isinstance(r, dict) and r.get("code") == 409)
    if oks != 1 or e409 != len(socks) - 1:
        mas += 1
    for s in socks: s.close()
    time.sleep(0.05)                          # dar tempo a que o "dup" saia
check("50 rondas de 8 NICK \"dup\" simultaneos: sempre 1 OK e 7 x 409", mas, 0)

# --- 2. maiusculas (D18) ----------------------------------------------------
print("2. maiusculas")
ana = entra("ana")
x = conn(); send(x, {"type": "NICK", "nick": "ANA"})
check("\"ANA\" com \"ana\" ligada -> 409", [m.get("code") for m in recv_all(x)], [409])
send(x, {"type": "NICK", "nick": "Rui"})
check("e na mesma ligacao \"Rui\" entra (D19)", recv_all(x), [{"type": "OK"}])
rui = x
zed = entra("zed")
recv_all(ana); recv_all(rui)

# --- 3. PRIV so chega a quem deve -------------------------------------------
print("3. PRIV")
send(ana, {"type": "PRIV", "to": "rui", "text": "segredo"})   # rui registou "Rui"
priv = {"type": "CHAT", "from": "ana", "to": "Rui", "text": "segredo", "private": True}
check("o destino recebe o CHAT privado (to como foi registado)", recv_all(rui), [priv])
check("o emissor recebe a copia (D14)", recv_all(ana), [priv])
check("o terceiro NAO recebe nada", recv_all(zed), [])
send(ana, {"type": "PRIV", "to": "ana", "text": "nota"})
check("PRIV a si proprio: uma so entrega",
      recv_all(ana), [{"type": "CHAT", "from": "ana", "to": "ana", "text": "nota", "private": True}])
send(ana, {"type": "PRIV", "to": "fantasma", "text": "?"})
check("PRIV para quem nao esta -> 404", [m.get("code") for m in recv_all(ana)], [404])

# --- 4. PRIV para quem entra e sai sem parar --------------------------------
print("4. PRIV contra entradas e saidas")
parar = threading.Event()
def pisca():
    while not parar.is_set():
        try:
            s = conn(); send(s, {"type": "NICK", "nick": "pisca"})
            s.settimeout(0.3)
            try: s.recv(4096)
            except socket.timeout: pass
            s.close()
        except OSError:
            pass
t = threading.Thread(target=pisca, daemon=True); t.start()
estranhas = 0; copias = 0; nao_ha = 0
ana.settimeout(2)
f = ana.makefile("rb")
for i in range(1500):
    send(ana, {"type": "PRIV", "to": "pisca", "text": str(i)})
    while True:                               # ler ate a resposta a ESTE PRIV
        m = json.loads(f.readline())
        if m.get("type") == "EVENT":
            continue
        break
    if m.get("type") == "CHAT" and m.get("private") and m.get("text") == str(i):
        copias += 1
    elif m.get("type") == "ERROR" and m.get("code") == 404:
        nao_ha += 1
    else:
        estranhas += 1
parar.set(); t.join()
time.sleep(0.4); recv_all(ana); recv_all(rui); recv_all(zed)
check(f"1500 PRIV: so copias ({copias}) ou 404 ({nao_ha}), nada mais", estranhas, 0)
check("e houve dos dois (a corrida aconteceu mesmo)", copias > 0 and nao_ha > 0, True)
check("o servidor continua vivo", srv.poll(), None)

# --- 5. LIST com a lista cheia ----------------------------------------------
print("5. LIST com 64")
# f.close() ANTES do ana.close(): em Python, o close() de um socket com um
# makefile() aberto NAO fecha a ligacao — a "ana" ficava no servidor e a
# seccao 5 encontrava a lista cheia (63 + ela).
f.close(); ana.close(); rui.close(); zed.close(); time.sleep(0.5)
cheios = []
for i in range(64):
    nick = ("%02d" % i) + "x" * 30              # 32 bytes cada
    cheios.append(entra(nick))
time.sleep(0.3)
for s in cheios: recv_all(s, 0.05)
cheios[0].settimeout(2)
send(cheios[0], {"type": "LIST"})
raw = b""
while not raw.endswith(b"\n"):
    raw += cheios[0].recv(65536)
users = [m for m in map(json.loads, raw.decode().splitlines()) if m.get("type") == "USERS"]
nomes = users[0]["nicks"] if users else []
check("USERS com os 64 nicks", len(nomes), 64)
check("por ordem alfabetica (D16)", nomes == sorted(nomes), True)
check(f"e cabe numa linha ({len(raw.splitlines()[-1])} bytes <= 4096)",
      len(raw.splitlines()[-1]) + 1 <= 4096, True)
for s in cheios: s.close()
time.sleep(0.5)

# --- 6. QUIT ----------------------------------------------------------------
print("6. QUIT")
ana = entra("ana"); rui = entra("rui"); recv_all(ana)
# QUIT e um MSG colados no mesmo send(): o MSG nao pode chegar a ninguem.
rui.sendall(b'{"type":"QUIT"}\n{"type":"MSG","text":"depois do quit"}\n')
check("QUIT -> OK e fecho", recv_all(rui, 1.0), [{"type": "OK"}, "CLOSED"])
check("os outros veem \"rui left\" e NAO o MSG colado depois",
      recv_all(ana), [{"type": "EVENT", "text": "rui left"}])
time.sleep(0.2)
r2 = conn(); send(r2, {"type": "NICK", "nick": "rui"})
check("o nick \"rui\" ficou livre", recv_all(r2), [{"type": "OK"}])
r2.close(); ana.close(); time.sleep(0.4)

# --- 7. o nosso cliente de ponta a ponta ------------------------------------
print("7. ./bin/client")
dono = entra("ana")
outro = entra("rui"); recv_all(dono)
# argv pede "ana" (em uso) -> 409 -> o cliente pede outro nick no stdin.
entrada = "ana2\n/list\n/msg rui ola rui\n/lsit\n/quit\n"
p = subprocess.run(["./bin/client", "127.0.0.1", PORT, "ana"], input=entrada,
                   capture_output=True, text=True, timeout=10)
out = p.stdout
check("409 -> o cliente pede outro nick e entra como ana2",
      "ja esta em uso" in out and "como ana2" in out, True)
check("/list mostra quem esta", "[online: 3] ana ana2 rui" in out, True)
check("/msg chega ao rui como privado",
      any(m.get("text") == "ola rui" and m.get("private") for m in recv_all(outro)), True)
check("e a copia aparece ao proprio", "<ana2 -> rui> (privado) ola rui" in out, True)
check("/lsit e recusado localmente, nao vai para o chat",
      "comando desconhecido" in out and not any(
          m.get("text") == "/lsit" for m in recv_all(dono) if isinstance(m, dict)), True)
check("/quit sai com 0", p.returncode, 0)
dono.close(); outro.close()

# ----------------------------------------------------------------------------
srv.terminate()
try: srv.wait(timeout=3)
except Exception: srv.kill(); srv.wait()
log.seek(0); linhas = log.read().splitlines()
print("\n--- LOG DO SERVIDOR (so o que nao e entrada/saida/erro esperado) ---")
print("\n".join(l for l in linhas
                if not any(k in l for k in ("ligado", "saiu", "fechou", "abortada",
                                            "ERROR 409", "ERROR 404", "QUIT",
                                            "entretanto saiu", "sem nick"))))
print(f"RESULTADO: {len(fails)} falha(s)")
sys.exit(1 if fails else 0)
