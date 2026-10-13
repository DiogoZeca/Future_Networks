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

Devolve 0 se tudo passou, 1 caso contrario.
"""
import glob
import json
import os
import socket
import sys
import time

TIMEOUT = 3.0
MODES = ("linha", "colado", "byte-a-byte")


def load(path):
    """Le um cenario: lista de ('>', texto) e ('<', objeto_json)."""
    steps = []
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            kind, _, rest = line.partition(" ")
            if kind == ">":
                steps.append((">", rest))
            elif kind == "<":
                steps.append(("<", json.loads(rest)))
            else:
                raise ValueError(f"{path}:{n}: linha deve comecar por '>', '<' ou '#'")
    return steps


class LineReader:
    """O mesmo problema do M1, do lado do Python: acumular e partir em '\\n'."""

    def __init__(self, sock):
        self.sock, self.buf = sock, b""

    def readline(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("o servidor fechou a ligacao")
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


def run(host, port, steps, mode):
    """Devolve None se passou, ou uma string com o que falhou."""
    with socket.create_connection((host, port), timeout=TIMEOUT) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        reader = LineReader(sock)
        i = 0
        while i < len(steps):
            if steps[i][0] == ">":
                batch = []
                while i < len(steps) and steps[i][0] == ">":
                    batch.append(steps[i][1])
                    i += 1
                send(sock, batch, mode)
            else:
                want = steps[i][1]
                i += 1
                try:
                    got_raw = reader.readline()
                except (socket.timeout, ConnectionError) as e:
                    return f"esperava {json.dumps(want)}\n        recebeu: ({e})"
                try:
                    got = json.loads(got_raw)
                except json.JSONDecodeError:
                    return f"resposta nao e JSON: {got_raw!r}"
                missing = {k: v for k, v in want.items()
                           if not isinstance(got, dict) or got.get(k, object()) != v}
                if missing:
                    return (f"esperava {json.dumps(want, ensure_ascii=False)}\n"
                            f"        recebeu {got_raw}")
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
