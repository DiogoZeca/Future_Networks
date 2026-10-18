#!/usr/bin/env python3
"""
probe_m3.py — verificacao dos comandos do M3 com clientes reais.

Como o probe_m2.py: arranca o seu proprio ./bin/server, liga clientes a serio
e verifica o que CADA UM recebe — e o que NAO deve receber. E passa/falha.

Seccoes:
  1. nicks unicos com corrida: 8 clientes pedem o mesmo nick no mesmo instante,
     50 vezes — ganha sempre exatamente UM (os outros: 409)
  2. maiusculas (D18): "ANA" e recusado se "ana" estiver
  3. PRIV: o emissor recebe OK, o destino o CHAT, e mais NINGUEM nada (sem
     copia ao emissor desde a revisao do M3)
  4. PRIV para quem entra e sai sem parar: nem crash nem respostas estranhas
     (o destino pode sair entre o "existe?" e o envio — refcount do find)
  5. LIST com a lista cheia: 64 nicks de 32 bytes, ordenados, numa linha
  6. QUIT: OK, fecho, "x left" aos outros; o que vem colado DEPOIS do QUIT e
     ignorado; o nick fica livre a seguir
  7. o nosso cliente (./bin/client) de ponta a ponta: 409 -> pede outro nick,
     /list, /msg (com eco local), comando desconhecido nao vai para o chat,
     os OK nao aparecem, /quit
  8. revisao do M3: linha de 64 KiB aceite e uma maior -> 413 e fecho; text
     de 4097 -> 400 "text too long" e a ligacao continua; mensagem de varias
     linhas chega igual a outro cliente, e o nosso cliente mostra-a
     indentada e sem caracteres de controlo
  9. correcao do M3: o nosso cliente NUM TERMINAL (pseudo-terminal, modulo
     pty): o prompt "ana> ", uma mensagem que chega a meio da escrita nao se
     mistura com ela, o que se envia aparece uma vez so, Enter sozinho nao
     faz nada, Backspace apaga um caracter UTF-8 inteiro, setas ignoradas,
     as boas-vindas do servidor (D22), e o terminal e reposto no Ctrl+D e
     no Ctrl+C

Uso:
    python3 scripts/probe_m3.py          # porta 5310 por omissao

Espera encontrar ./bin/server e ./bin/client (correr a partir da raiz).
"""
import atexit, fcntl, json, os, pty, select, socket, struct, subprocess, sys, tempfile, termios, threading, time

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
    except ConnectionResetError:
        out.append("RESET")                   # fecho com RST (ver seccao 8)
    return out


def entra(nick):
    s = conn(); send(s, {"type": "NICK", "nick": nick})
    r = recv_all(s, 0.3)
    assert r[:1] == [{"type": "OK"}], f"{nick} nao entrou: {r}"
    return s


def welcome(nick):
    """O EVENT de boas-vindas que segue o OK a um NICK (correcao do M3, D22)."""
    return {"type": "EVENT",
            "text": f"welcome, {nick}! commands: /list, /msg <nick> <text>, /quit"}


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
check("e na mesma ligacao \"Rui\" entra (D19), com as boas-vindas (D22)",
      recv_all(x), [{"type": "OK"}, welcome("Rui")])
rui = x
zed = entra("zed")
recv_all(ana); recv_all(rui)

# --- 3. PRIV so chega a quem deve -------------------------------------------
print("3. PRIV")
send(ana, {"type": "PRIV", "to": "rui", "text": "segredo"})   # rui registou "Rui"
priv = {"type": "CHAT", "from": "ana", "text": "segredo", "private": True}
check("o destino (\"rui\" encontra \"Rui\") recebe o CHAT privado, sem \"to\"",
      recv_all(rui), [priv])
check("o emissor recebe so o OK, sem copia (D13, D14)", recv_all(ana), [{"type": "OK"}])
check("o terceiro NAO recebe nada", recv_all(zed), [])
send(ana, {"type": "PRIV", "to": "ana", "text": "nota"})
check("PRIV a si proprio: OK e o CHAT, uma vez cada",
      recv_all(ana), [{"type": "OK"}, {"type": "CHAT", "from": "ana", "text": "nota", "private": True}])
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
estranhas = 0; aceites = 0; nao_ha = 0
ana.settimeout(2)
f = ana.makefile("rb")
for i in range(1500):
    send(ana, {"type": "PRIV", "to": "pisca", "text": str(i)})
    while True:                               # ler ate a resposta a ESTE PRIV
        m = json.loads(f.readline())
        if m.get("type") == "EVENT":
            continue
        break
    if m.get("type") == "OK":
        aceites += 1
    elif m.get("type") == "ERROR" and m.get("code") == 404:
        nao_ha += 1
    else:
        estranhas += 1
parar.set(); t.join()
time.sleep(0.4); recv_all(ana); recv_all(rui); recv_all(zed)
check(f"1500 PRIV: so OK ({aceites}) ou 404 ({nao_ha}), nada mais", estranhas, 0)
check("e houve dos dois (a corrida aconteceu mesmo)", aceites > 0 and nao_ha > 0, True)
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
check("o nick \"rui\" ficou livre", recv_all(r2), [{"type": "OK"}, welcome("rui")])
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
check("e o eco LOCAL aparece ao proprio", "<ana2 -> rui> (privado) ola rui" in out, True)
check("os OK nao aparecem no ecra", "[ok]" not in out, True)
check("/lsit e recusado localmente, nao vai para o chat",
      "comando desconhecido" in out and not any(
          m.get("text") == "/lsit" for m in recv_all(dono) if isinstance(m, dict)), True)
check("/quit sai com 0", p.returncode, 0)
dono.close(); outro.close()


# --- 8. revisao do M3: limites e varias linhas ------------------------------
print("8. limites e varias linhas")
ana = entra("ana"); rui = entra("rui"); recv_all(ana)

def manda_bruto(s, data):
    s.sendall(data)

# Uma linha de EXATAMENTE 64 KiB (com o '\n') e aceite pelo framing: como o
# text tem mais de 4096, a resposta e o 400 "text too long" — e nao o 413.
pre = b'{"type":"MSG","text":"'; pos = b'"}\n'
linha = pre + b"x" * (65536 - len(pre) - len(pos)) + pos
manda_bruto(ana, linha)
r = recv_all(ana, 1.0)
check("linha de 65536 bytes: chega a session (400 text too long, nao 413)",
      [(m.get("code"), m.get("reason")) for m in r if isinstance(m, dict)],
      [(400, "text too long")])
send(ana, {"type": "MSG", "text": "ainda aqui"})
check("e a ligacao continua", [m.get("type") for m in recv_all(ana)], ["OK", "CHAT"])
recv_all(rui)

# 65537 bytes: um a mais -> 413 e o servidor fecha.
x = conn(); send(x, {"type": "NICK", "nick": "grande"}); recv_all(x)
manda_bruto(x, pre + b"y" * (65537 - len(pre) - len(pos)) + pos)
r = [m.get("code") if isinstance(m, dict) else m for m in recv_all(x, 1.5)]
# O servidor fecha com bytes do cliente ainda por ler (o resto da linha), e
# o kernel manda RST em vez de FIN — que pode engolir o proprio ERROR 413. E
# o problema conhecido do "line too long" (M0 nota 4, M1 caso hostil B), que
# fica para o M4. Aqui aceita-se o 413 + fecho, ou so o RST.
check("linha de 65537 bytes: ERROR 413 e o servidor fecha (ou RST, M4)",
      r in ([413, "CLOSED"], [413, "RESET"], ["RESET"]), True)
if r == ["RESET"]:
    print("        (desta vez o RST engoliu o 413 — o problema do M4)")
x.close(); recv_all(ana); recv_all(rui)

# Varias linhas, com um '\033' pelo meio: chega igual (o servidor nao mexe).
send(ana, {"type": "MSG", "text": "l1\nl2\x1b[2J"})
r = recv_all(rui)
check("mensagem de varias linhas chega tal e qual a outro cliente",
      [m.get("text") for m in r], ["l1\nl2\x1b[2J"])
recv_all(ana)

# E o nosso cliente mostra-a indentada e limpa.
p = subprocess.Popen(["./bin/client", "127.0.0.1", PORT, "zed"], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
time.sleep(0.5); recv_all(ana); recv_all(rui)
send(ana, {"type": "MSG", "text": "linha 1\nlinha 2\x1b[2J"})
time.sleep(0.4)
out, _ = p.communicate("/quit\n", timeout=10)
check("o nosso cliente mostra as linhas seguintes indentadas e o ESC como '?'",
      "<ana> linha 1\n    linha 2?[2J" in out, True)
ana.close(); rui.close(); time.sleep(0.3)

# --- 9. correcao do M3: o nosso cliente num terminal --------------------------
print("9. ./bin/client num terminal (pty)")


class Ecra:
    """Um terminal minimo: o suficiente para o que o nosso cliente escreve
    ('\\r', '\\n', ESC [ K, texto). Guarda as linhas como ficam no ecra — e e
    isso que se verifica, nao os bytes: "aparece uma vez so" e uma pergunta
    sobre o ecra."""

    def __init__(self):
        self.linhas = [""]; self.col = 0; self.pend = b""

    def come(self, data):
        data = self.pend + data; self.pend = b""
        i = 0
        while i < len(data):
            c = data[i:i + 1]
            if c == b"\x1b":
                if data[i + 1:i + 3] == b"[K":      # apagar ate ao fim da linha
                    l = self.linhas[-1]; self.linhas[-1] = l[:self.col]; i += 3; continue
                if i + 3 > len(data):               # sequencia partida: esperar
                    self.pend = data[i:]; return
                i += 3; continue
            if c == b"\r":
                self.col = 0
            elif c == b"\n":
                self.linhas.append(""); self.col = 0
            elif c == b"\a":
                pass
            else:                                   # texto (UTF-8 inteiro)
                j = i + 1
                while j < len(data) and data[j:j + 1] not in (b"\x1b", b"\r", b"\n", b"\a"):
                    j += 1
                try:
                    t = data[i:j].decode()
                except UnicodeDecodeError as e:
                    if j == len(data) and j - (i + e.start) < 4:
                        self.pend = data[i:]; return    # partido entre leituras
                    t = data[i:j].decode(errors="replace")  # lixo: mostrar como tal
                l = self.linhas[-1]
                self.linhas[-1] = l[:self.col] + t + l[self.col + len(t):]
                self.col += len(t); i = j; continue
            i += 1

    def ultima(self):
        return self.linhas[-1]

    def com(self, texto):
        return [l for l in self.linhas if texto in l]


def cliente_pty(nick):
    pid, fd = pty.fork()
    if pid == 0:                                    # filho: o terminal e o pty
        os.execv("./bin/client", ["./bin/client", "127.0.0.1", PORT, nick])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    return pid, fd


def ler_pty(fd, ecra, quieto=0.4):
    """Le ate o cliente ficar calado `quieto` segundos."""
    while True:
        r, _, _ = select.select([fd], [], [], quieto)
        if not r:
            return
        try:
            d = os.read(fd, 65536)
        except OSError:                             # o cliente terminou
            return
        if not d:
            return
        ecra.come(d)


def eco_e_canon(fd):
    lf = termios.tcgetattr(fd)[3]
    return bool(lf & termios.ECHO) and bool(lf & termios.ICANON)


rui = entra("rui"); recv_all(rui)
pid, fd = cliente_pty("ana")
ecra = Ecra()
ler_pty(fd, ecra, 0.8); recv_all(rui)
check("o prompt e o nick a frente: \"ana> \"", ecra.ultima(), "ana> ")
check("as boas-vindas do servidor aparecem (D22)",
      ecra.com("* welcome, ana! commands: /list, /msg <nick> <text>, /quit") != [], True)
check("o terminal esta em modo raw (sem eco do terminal)", eco_e_canon(fd), False)

# A meio da escrita chega uma mensagem: nao se misturam.
os.write(fd, b"meio"); ler_pty(fd, ecra)
send(rui, {"type": "MSG", "text": "boas"}); recv_all(rui); ler_pty(fd, ecra)
check("a mensagem que chega fica numa linha so dela",
      ecra.com("<rui> boas"), ["<rui> boas"])
check("e o que se estava a escrever continua no prompt", ecra.ultima(), "ana> meio")

# Enter: o rui recebe; no ecra da ana a mensagem aparece UMA vez.
os.write(fd, b"\r"); ler_pty(fd, ecra)
check("o rui recebe o que a ana escreveu",
      [m.get("text") for m in recv_all(rui) if m.get("type") == "CHAT"], ["meio"])
check("no ecra da ana aparece uma vez so (era o 'duplica no ecra')",
      ecra.com("meio"), ["<ana> meio"])
check("e o prompt fica vazio", ecra.ultima(), "ana> ")

# Enter sozinho: nada sai e o ecra nao sobe.
antes = list(ecra.linhas)
os.write(fd, b"\r\r\r\r\r"); ler_pty(fd, ecra)
check("5 Enters sozinhos: o ecra fica igual (era o 'spam de Enters')", ecra.linhas, antes)
check("e nada chega ao servidor", recv_all(rui), [])

# Backspace num caracter de 2 bytes, setas pelo meio, Ctrl+U.
os.write(fd, "olé".encode() + b"\x7f" + b"a" + b"\x1b[A\x1b[D" + b"!\r"); ler_pty(fd, ecra)
check("Backspace apaga o 'e' inteiro (2 bytes) e as setas nao entram no texto",
      [m.get("text") for m in recv_all(rui) if m.get("type") == "CHAT"], ["ola!"])
os.write(fd, b"lixo\x15fica\r"); ler_pty(fd, ecra)
check("Ctrl+U apaga a linha",
      [m.get("text") for m in recv_all(rui) if m.get("type") == "CHAT"], ["fica"])

# /msg: aparece o eco local, nao o que se escreveu.
os.write(fd, b"/msg rui segredo\r"); ler_pty(fd, ecra)
check("/msg: o rui recebe o privado",
      [(m.get("text"), m.get("private")) for m in recv_all(rui)], [("segredo", True)])
check("/msg: no ecra fica so o eco local, uma vez",
      ecra.com("segredo"), ["<ana -> rui> (privado) segredo"])

# Ctrl+D: QUIT, sai com 0 e o terminal volta ao normal.
os.write(fd, b"\x04"); ler_pty(fd, ecra, 0.8)
_, st = os.waitpid(pid, 0)
check("Ctrl+D: o cliente sai com 0", os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0, True)
check("e o terminal volta a ter eco e modo canonico", eco_e_canon(fd), True)
check("o rui ve a ana sair", recv_all(rui), [{"type": "EVENT", "text": "ana left"}])
os.close(fd)

# Ctrl+C: o cliente morre (como antes: o QUIT no Ctrl+C e do M4), mas repoe
# o terminal primeiro.
pid, fd = cliente_pty("zed"); ler_pty(fd, Ecra(), 0.8); recv_all(rui)
os.write(fd, b"meio escrito\x03"); time.sleep(0.5)
_, st = os.waitpid(pid, 0)
check("Ctrl+C: o cliente termina pelo SIGINT", os.WIFSIGNALED(st) and os.WTERMSIG(st) == 2, True)
check("e o terminal foi reposto antes", eco_e_canon(fd), True)
os.close(fd)
rui.close(); time.sleep(0.3)

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
