#!/usr/bin/env python3
"""
probe_m2.py — verificacao funcional do servidor multiutilizador (M2).

Nao faz parte do projeto: e uma ferramenta de diagnostico, como o probe_m0.py.
Liga tres clientes a serio e verifica o que cada um recebe — broadcast, EVENT
de entrada e saida, e quem NAO deve receber o que.

O caso que mais rende: o terceiro cliente liga-se mas so faz NICK depois, e
nessa janela NAO pode receber nada. Foi assim que se descobriu que um cliente
sem NICK estava a receber EVENT antes do OK ao seu proprio NICK.

Uso:
    python3 scripts/probe_m2.py          # porta 5210 por omissao

Espera encontrar ./bin/server (correr a partir da raiz do repositorio).
"""
import json, socket, subprocess, sys, time

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

srv = subprocess.Popen(["./bin/server", PORT], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
time.sleep(0.5)
fails=[]
def check(name, got, want):
    ok = got == want
    print(f"  {'ok  ' if ok else 'FALHA'} {name}")
    if not ok:
        print(f"        obtido:   {got}\n        esperado: {want}")
        fails.append(name)

# --- 1. tres clientes entram ---
a, b, c = conn(), conn(), conn()
send(a, {"type":"NICK","nick":"ana"})
check("ana recebe OK", recv_all(a), [{"type":"OK"}])

send(b, {"type":"NICK","nick":"rui"})
check("rui recebe OK", recv_all(b), [{"type":"OK"}])
check("ana e avisada de que o rui entrou",
      recv_all(a), [{"type":"EVENT","text":"rui joined"}])

send(c, {"type":"NICK","nick":"zed"})
recv_all(c)
check("ana ve o zed entrar", recv_all(a), [{"type":"EVENT","text":"zed joined"}])
check("rui ve o zed entrar", recv_all(b), [{"type":"EVENT","text":"zed joined"}])

# --- 2. broadcast: todos recebem, INCLUINDO o emissor (D5) ---
send(a, {"type":"MSG","text":"ola a todos"})
chat = {"type":"CHAT","from":"ana","text":"ola a todos","private":False}
check("o EMISSOR recebe o proprio CHAT (D5)", recv_all(a), [chat])
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

a.close(); b.close(); time.sleep(0.5)
srv.terminate()
try: out,_ = srv.communicate(timeout=3)
except Exception: srv.kill(); out,_ = srv.communicate()
print("\n--- LOG DO SERVIDOR ---"); print(out)
print(f"RESULTADO: {len(fails)} falha(s)")
sys.exit(1 if fails else 0)
