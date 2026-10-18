#!/usr/bin/env python3
"""
probe_m2.py — verificacao funcional do servidor multiutilizador (M2).

Nao faz parte do projeto: e uma ferramenta de diagnostico, como o probe_m0.py.
Liga tres clientes a serio e verifica o que cada um recebe — broadcast, EVENT
de entrada e saida, e quem NAO deve receber o que.

O caso que mais rende: o terceiro cliente liga-se mas so faz NICK depois, e
nessa janela NAO pode receber nada. Foi assim que se descobriu que um cliente
sem NICK estava a receber EVENT antes do OK ao seu proprio NICK.

Secoes 6-8 (acrescentadas na revisao do M2, cada uma e um bug que existiu):
  6. o OK e SEMPRE a primeira linha, mesmo com trafego constante
     (antes: 26 em 2000 ligacoes viam um CHAT antes do OK)
  7. um cliente que nao le e desligado e o chat continua
     (antes: o chat inteiro congelava para sempre) — demora ~6 s
  8. lista cheia: quem nao cabe recebe ERROR 503 em vez de um fecho mudo

Uso:
    python3 scripts/probe_m2.py          # porta 5210 por omissao

Espera encontrar ./bin/server (correr a partir da raiz do repositorio).
"""
import atexit, json, select, socket, subprocess, sys, tempfile, threading, time

PORT = sys.argv[1] if len(sys.argv) > 1 else "5210"
def conn():
    s = socket.create_connection(("127.0.0.1", int(PORT)), timeout=3)
    s.settimeout(1.5); return s

def send(s, obj): s.sendall((json.dumps(obj)+"\n").encode())

def recv_all(s, timeout=0.7):
    """Le tudo o que chegar ate ao timeout. Devolve lista de dicts."""
    s.settimeout(timeout); out=[]; buf=b""
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n",1)
                if line.strip(): out.append(json.loads(line))
    except socket.timeout: pass
    return out

# O log vai para um ficheiro temporario e NAO para um pipe: um pipe que
# ninguem le enche aos 64 KB, o printf() do servidor bloqueia e o servidor
# inteiro para — as secoes 6-8 geram milhares de linhas de log.
log = tempfile.TemporaryFile(mode="w+")
srv = subprocess.Popen(["./bin/server", PORT], stdout=log,
                       stderr=subprocess.STDOUT, text=True)
# Matar o servidor SEMPRE, mesmo que o script rebente a meio: senao fica um
# servidor orfao na porta, e a execucao seguinte testa ESSE (velho) sem dar
# por isso — o novo falha com "Address already in use" em silencio.
atexit.register(lambda: srv.poll() is None and srv.kill())
time.sleep(0.5)
if srv.poll() is not None:
    log.seek(0); print(log.read())
    sys.exit(f"o servidor nao arrancou na porta {PORT} (ja ha outro a correr?)")
fails=[]
def check(name, got, want):
    ok = got == want
    print(f"  {'ok  ' if ok else 'FALHA'} {name}")
    if not ok:
        print(f"        obtido:   {got}\n        esperado: {want}")
        fails.append(name)

# Correcao do M3 (D22): a quem entra, o OK e seguido de um EVENT de
# boas-vindas so para ele, com os comandos.
def welcome(nick):
    return {"type":"EVENT",
            "text":f"welcome, {nick}! commands: /list, /msg <nick> <text>, /quit"}

# --- 1. tres clientes entram ---
a, b, c = conn(), conn(), conn()
send(a, {"type":"NICK","nick":"ana"})
check("ana recebe OK (e as boas-vindas, D22)", recv_all(a), [{"type":"OK"}, welcome("ana")])

send(b, {"type":"NICK","nick":"rui"})
check("rui recebe OK (e as boas-vindas, D22)", recv_all(b), [{"type":"OK"}, welcome("rui")])
check("ana e avisada de que o rui entrou",
      recv_all(a), [{"type":"EVENT","text":"rui joined"}])

send(c, {"type":"NICK","nick":"zed"})
recv_all(c)
check("ana ve o zed entrar", recv_all(a), [{"type":"EVENT","text":"zed joined"}])
check("rui ve o zed entrar", recv_all(b), [{"type":"EVENT","text":"zed joined"}])

# --- 2. broadcast: todos recebem, INCLUINDO o emissor (D5) ---
send(a, {"type":"MSG","text":"ola a todos"})
chat = {"type":"CHAT","from":"ana","text":"ola a todos","private":False}
# Desde a revisao do M3 o emissor recebe primeiro o OK (D13) e depois o CHAT.
check("o EMISSOR recebe OK e o proprio CHAT (D13, D5)", recv_all(a), [{"type":"OK"}, chat])
check("rui recebe o CHAT", recv_all(b), [chat])
check("zed recebe o CHAT", recv_all(c), [chat])

# --- 3. erro so para quem se enganou ---
send(b, {"type":"MSG"})                       # sem text
check("rui recebe ERROR 400",
      [m["type"] for m in recv_all(b)], ["ERROR"])
check("ana NAO recebe o erro do rui", recv_all(a), [])

# --- 4. saida ---
c.close()
time.sleep(0.4)
check("ana ve o zed sair", recv_all(a), [{"type":"EVENT","text":"zed left"}])
check("rui ve o zed sair", recv_all(b), [{"type":"EVENT","text":"zed left"}])

# --- 5. quem sai antes do NICK nao gera EVENT ---
d = conn(); d.close(); time.sleep(0.4)
check("ligar e desligar sem nick nao avisa ninguem", recv_all(a), [])

# --- 6. o OK e a PRIMEIRA linha, mesmo com trafego (D9) ---
# A ana fala sem parar; 1000 clientes entram um a um e cada um ve a 1.a linha.
# A janela era estreita (~1% das entradas), daí serem tantas.
parar = threading.Event()
def tagarela():
    while not parar.is_set():
        send(a, {"type":"MSG","text":"z"})
# Leitores em threads: esperam com select() e NAO com s.settimeout(), porque o
# timeout e do socket inteiro e tambem se aplicaria aos sendall() da outra
# thread — que aqui podem ter de esperar (o servidor pode estar ate 5 s preso
# num cliente que nao le, e entretanto nao le o nosso socket).
def le_ate(s, parar, guarda=None):
    s.settimeout(None); buf = b""
    while not parar.is_set():
        if not select.select([s], [], [], 0.2)[0]: continue
        try: d = s.recv(65536)
        except OSError: return
        if not d: return
        if guarda is None: continue
        buf += d
        while b"\n" in buf:
            l, buf = buf.split(b"\n", 1); guarda.append(json.loads(l))
def escoa(s):                                 # quem esta no chat tem de ler
    le_ate(s, parar)
a.settimeout(None); b.settimeout(None)       # ver le_ate()
ths = [threading.Thread(target=f, args=x, daemon=True)
       for f, x in ((tagarela, ()), (escoa, (a,)), (escoa, (b,)))]
for t in ths: t.start()
maus = 0
for i in range(1000):
    s = conn(); f = s.makefile("rb")
    send(s, {"type":"NICK","nick":"n%d" % i})
    try:
        if json.loads(f.readline()).get("type") != "OK": maus += 1
    except (socket.timeout, ValueError):
        maus += 1                             # nada (ou lixo) em 1,5 s
    s.close()
parar.set()
for t in ths: t.join()
check("1000 entradas com trafego: a 1.a linha e sempre o OK", maus, 0)
recv_all(a); recv_all(b)

# --- 7. cliente que nao le e desligado; o resto continua (regra 4) ---
lento = socket.socket()
lento.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
lento.connect(("127.0.0.1", int(PORT)))
send(lento, {"type":"NICK","nick":"lento"})   # e nunca mais le nada
time.sleep(0.3); recv_all(a); recv_all(b)

recebido = {"ana": [], "rui": []}
parar = threading.Event()
def junta(nome, s):
    le_ate(s, parar, recebido[nome])
a.settimeout(None); b.settimeout(None)
ths = [threading.Thread(target=junta, args=(n, s), daemon=True)
       for n, s in (("ana", a), ("rui", b))]
for t in ths: t.start()
for _ in range(3000):                          # ~3 MB: enche os buffers do lento
    send(a, {"type":"MSG","text":"x" * 1000})
time.sleep(6.5)                                # SEND_TIMEOUT_MS = 5000
# Duas mensagens: com o bug, a thread do rui ficava presa a meio do broadcast
# da 1.a (na fila do wlock do lento) e a 2.a nunca chegava a ser lida.
send(b, {"type":"MSG","text":"ainda ha alguem?"})
send(b, {"type":"MSG","text":"segunda"})
# Esperar ATE a "segunda" chegar aos dois (no maximo 10 s), e nao um tempo
# fixo: os ~3 MB da ana ainda podem estar a ser entregues a frente dela. Com
# 0,5 s fixos esta seccao falhava numa maquina mais lenta (uma VM) sem haver
# bug nenhum no servidor — M3.md, nota 11.
fim = time.time() + 10
while time.time() < fim and not all(
        any(m.get("text") == "segunda" for m in recebido[n]) for n in ("ana", "rui")):
    time.sleep(0.1)
parar.set()
for t in ths: t.join()
check("o lento foi desligado (os outros veem 'lento left')",
      any(m.get("text") == "lento left" for m in recebido["ana"]), True)
check("depois disso o rui ainda fala e ouve-se a si proprio",
      any(m.get("text") == "segunda" for m in recebido["rui"]), True)
check("e a ana ouve o rui",
      any(m.get("text") == "segunda" for m in recebido["ana"]), True)
lento.close()

a.close(); b.close(); time.sleep(0.5)

# --- 8. lista cheia: ERROR 503, nao um fecho mudo ---
# 70 ligacoes para um limite de 64 (ROSTER_MAX). Nao se conta qual e a 65.a:
# clientes das secoes anteriores podem ainda estar a sair. Verifica-se que as
# que sobram recebem TODAS um ERROR 503, e nada mais.
socks = [conn() for _ in range(70)]
time.sleep(0.5)
recusas = [recv_all(s, 0.3) for s in socks]
recusas = [r for r in recusas if r]           # quem foi aceite nao recebe nada
check("as ligacoes a mais recebem ERROR 503 (e so isso)",
      bool(recusas) and all([(m["type"], m.get("code")) for m in r] == [("ERROR", 503)]
                            for r in recusas), True)
for s in socks: s.close()
time.sleep(0.5)

srv.terminate()
try: srv.wait(timeout=3)
except Exception: srv.kill(); srv.wait()
log.seek(0); linhas = log.read().splitlines()
print("\n--- LOG DO SERVIDOR (linhas que nao sao entradas/saidas) ---")
print("\n".join(l for l in linhas
                if not any(k in l for k in ("ligado", "saiu", "fechou", "abortada"))))
print(f"RESULTADO: {len(fails)} falha(s)")
sys.exit(1 if fails else 0)
