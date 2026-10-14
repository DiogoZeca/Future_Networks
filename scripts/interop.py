#!/usr/bin/env python3
"""
interop.py - corre os cenarios de tests/interop/ contra QUALQUER servidor
(o nosso ou o de outro grupo, em qualquer linguagem).

Uso:
    python3 scripts/interop.py <host> <porta> [cenario.txt ...]

Sem cenarios na linha de comando, corre todos os tests/interop/*.txt.

Cada cenario e corrido em TRES modos de envio, porque e isso que o M1 tem de
aguentar (slide 125: "Two send() calls can arrive in one recv(), or one send()
can require many recv() calls"):

    linha        um send() por linha            - o caso facil
    colado       linhas '>' seguidas num send() - mensagens coladas
    byte-a-byte  um send() por byte             - mensagens partidas
                 (com TCP_NODELAY, para o kernel nao voltar a junta-los)

As respostas ('<') sao comparadas como JSON e como SUBCONJUNTO: os campos
esperados tem de existir com aquele valor; campos a mais sao ignorados. Assim
um cenario com {"type":"ERROR","code":400} passa com qualquer "reason".

Desde o M3 um cenario pode usar VARIAS ligacoes (o 409 e o PRIV precisam de
dois clientes). A letra antes do '>'/'<' diz qual: "a>", "b<", ... Sem letra
e a ligacao por omissao, como ate ao M2. Cada ligacao abre na primeira vez que
aparece, pela ordem do cenario.

Duas regras extra (M3):
  - "< CLOSED" espera que o servidor feche essa ligacao (QUIT).
  - A espera de um tipo que NAO seja EVENT salta os EVENT que aparecerem
    pelo caminho. Quem recebe os "x joined"/"x left" e semantica de cada
    grupo (D10): um cenario de PRIV nao deve falhar por causa disso.

Devolve 0 se tudo passou, 1 caso contrario.
"""
import glob
import json
import os
import re
import socket
import sys
import time

TIMEOUT = 3.0
MODES = ("linha", "colado", "byte-a-byte")


STEP = re.compile(r"^([a-z]?)([<>]) (.*)$")
CLOSED = "CLOSED"


def load(path):
    """Le um cenario: lista de (ligacao, '>', texto) e (ligacao, '<', json)."""
    steps = []
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            m = STEP.match(line)
            if not m:
                raise ValueError(f"{path}:{n}: linha deve comecar por "
                                 f"'>', '<', 'a>', 'b<', ... ou '#'")
            conn, kind, rest = m.groups()
            if kind == ">":
                steps.append((conn, ">", rest))
            elif rest == CLOSED:
                steps.append((conn, "<", CLOSED))
            else:
                steps.append((conn, "<", json.loads(rest)))
    return steps


class LineReader:
    """O mesmo problema do M1, do lado do Python: acumular e partir em '\\n'."""

    def __init__(self, sock):
        self.sock, self.buf = sock, b""

    def readline(self):
        """A proxima linha, ou None se o servidor fechou."""
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                return None
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return line.rstrip(b"\r").decode("utf-8")


def send(sock, lines, mode):
    data = [(l + "\n").encode("utf-8") for l in lines]
    if mode == "linha":
        for d in data:
            sock.sendall(d)
    elif mode == "colado":
        sock.sendall(b"".join(data))
    else:
        for d in data:
            for i in range(len(d)):
                sock.sendall(d[i:i + 1])
                time.sleep(0.0005)


def expect(reader, want, who):
    """Le ate a resposta esperada. Devolve None se bateu certo, ou o erro."""
    while True:
        try:
            got_raw = reader.readline()
        except (socket.timeout, OSError) as e:
            return f"{who}esperava {json.dumps(want, ensure_ascii=False)}\n        recebeu: ({e})"
        if want == CLOSED:
            if got_raw is None:
                return None
            return f"{who}esperava o fecho da ligacao\n        recebeu {got_raw}"
        if got_raw is None:
            return (f"{who}esperava {json.dumps(want, ensure_ascii=False)}\n"
                    f"        recebeu: (o servidor fechou a ligacao)")
        try:
            got = json.loads(got_raw)
        except json.JSONDecodeError:
            return f"{who}resposta nao e JSON: {got_raw!r}"
        # EVENT a mais pelo caminho: semantica de cada grupo (D10), saltar.
        if (isinstance(got, dict) and got.get("type") == "EVENT"
                and want.get("type") != "EVENT"):
            continue
        missing = {k: v for k, v in want.items()
                   if not isinstance(got, dict) or got.get(k, object()) != v}
        if missing:
            return (f"{who}esperava {json.dumps(want, ensure_ascii=False)}\n"
                    f"        recebeu {got_raw}")
        return None


def run(host, port, steps, mode):
    """Devolve None se passou, ou uma string com o que falhou."""
    conns = {}                                  # letra -> (socket, LineReader)

    def get(name):
        if name not in conns:
            sock = socket.create_connection((host, port), timeout=TIMEOUT)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            conns[name] = (sock, LineReader(sock))
        return conns[name]

    multi = len({c for c, _, _ in steps}) > 1
    try:
        i = 0
        while i < len(steps):
            name, kind, payload = steps[i]
            who = f"[{name or '-'}] " if multi else ""
            sock, reader = get(name)
            if kind == ">":
                # Linhas '>' seguidas DA MESMA ligacao vao juntas (modo colado).
                batch = []
                while i < len(steps) and steps[i][:2] == (name, ">"):
                    batch.append(steps[i][2])
                    i += 1
                send(sock, batch, mode)
            else:
                i += 1
                err = expect(reader, payload, who)
                if err:
                    return err
    finally:
        for sock, _ in conns.values():
            sock.close()
    return None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    host, port = sys.argv[1], int(sys.argv[2])
    here = os.path.dirname(os.path.abspath(__file__))
    files = sys.argv[3:] or sorted(glob.glob(os.path.join(here, "..", "tests", "interop", "*.txt")))

    fails = 0
    for path in files:
        steps = load(path)
        for mode in MODES:
            err = run(host, port, steps, mode)
            status = "ok   " if err is None else "FALHA"
            print(f"{status} {os.path.basename(path):<26} {mode}")
            if err:
                print(f"        {err}")
                fails += 1
    total = len(files) * len(MODES)
    print(f"\ninterop: {total - fails} ok, {fails} falhas ({host}:{port})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
