#!/usr/bin/env python3
"""
probe_client.py - sonda do NOSSO cliente contra servidores "estranhos".

Arranca, para cada cenario, um servidor falso que se porta como o servidor de
outro grupo PODIA portar-se (sem OK ao NICK, respostas partidas, tipos que
ainda nao conhecemos, fecho a meio...), corre ./bin/client contra ele com a
linha "ola" e mostra o que o cliente imprimiu.

Como o probe_m0.py, nao e passa/falha: o objetivo e ver o que acontece e
saber explicar porque.

Uso (a partir da raiz do repositorio, depois de `make`):
    python3 scripts/probe_client.py          # porta base 5190
    python3 scripts/probe_client.py 6000
"""
import socket
import subprocess
import sys
import threading
import time

OK = b'{"type":"OK"}\n'


def chat(text):
    return b'{"type":"CHAT","from":"ana","text":"' + text + b'","private":false}\n'


# cenario -> (descricao, funcao(numero_da_linha) -> bytes a enviar; None = fechar)
def byte_a_byte(sock, data):
    for b in data:
        sock.sendall(bytes([b]))
        time.sleep(0.002)


SCENARIOS = {
    "sem_ok": (
        "nao responde ao NICK (D6 diferente)",
        lambda i: b"" if i == 0 else chat(b"eco")),
    "byte_a_byte": (
        "cada resposta chega um byte de cada vez",
        lambda i: ("BYTES", OK if i == 0 else chat(b"partido"))),
    "colado_com_event": (
        "OK e um EVENT (tipo do M2) colados no mesmo segmento",
        lambda i: OK + b'{"type":"EVENT","text":"rui joined"}\n' if i == 0
        else chat(b"depois do event")),
    "nick_recusado": (
        "recusa o NICK com 409",
        lambda i: b'{"type":"ERROR","code":409,"reason":"nick taken"}\n'),
    "fecha_a_meio": (
        "fecha a ligacao a meio de uma resposta",
        lambda i: OK if i == 0 else (b'{"type":"CHA', None)),
    "linha_gigante": (
        "manda 5000 bytes sem '\\n' e fecha",
        lambda i: OK if i == 0 else b"Z" * 5000),
    # Revisao do M3: o cliente e tolerante a receber. Uma linha maior do que
    # 64 KiB e descartada (com um aviso) e a seguinte e mostrada normalmente.
    "linha_acima_64k": (
        "manda 70000 bytes com '\\n' e depois um CHAT normal",
        lambda i: OK if i == 0 else b"Z" * 70000 + b"\n" + chat(b"depois da gigante")),
    # Como o servidor do primeiro grupo com quem falamos: OK a cada pedido
    # aceite, antes do CHAT. Os OK nao devem aparecer no ecra.
    "ok_a_cada_pedido": (
        "responde OK + CHAT a cada mensagem; o CHAT tem varias linhas e um ESC",
        lambda i: OK if i == 0 else OK + chat(b"linha 1\\nlinha 2\\u001b[31m")),
    "crlf_e_ordem": (
        "termina em \\r\\n, chaves por outra ordem, campo extra",
        lambda i: b'{"type":"OK"}\r\n' if i == 0 else
        b'{"private":false,"text":"crlf","from":"ana","type":"CHAT","extra":1}\r\n'),
    "nao_json": (
        "responde com uma linha que nao e JSON",
        lambda i: OK if i == 0 else b"isto nao e json\n"),
}


def fake_server(listener, behave):
    conn, _ = listener.accept()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    buf, i = b"", 0
    try:
        while True:
            while b"\n" not in buf:
                data = conn.recv(4096)
                if not data:
                    return
                buf += data
            _, _, buf = buf.partition(b"\n")
            out = behave(i)
            close_after = False
            if isinstance(out, tuple) and out[0] == "BYTES":
                byte_a_byte(conn, out[1])
            elif isinstance(out, tuple):
                conn.sendall(out[0])
                close_after = True
            else:
                conn.sendall(out)
            if close_after:
                return
            i += 1
    except OSError:
        pass                                  # o cliente fechou primeiro (RST)
    finally:
        time.sleep(0.2)
        conn.close()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5190
    for n, (name, (desc, behave)) in enumerate(SCENARIOS.items()):
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", port + n))
        listener.listen(1)
        t = threading.Thread(target=fake_server, args=(listener, behave), daemon=True)
        t.start()

        start = time.time()
        r = subprocess.run(["./bin/client", "127.0.0.1", str(port + n), "ana"],
                           input=b"ola\n", capture_output=True, timeout=30)
        took = time.time() - start
        t.join(timeout=2)
        listener.close()

        print(f"=== {name}: {desc}")
        for line in (r.stdout + r.stderr).decode("utf-8", "replace").splitlines():
            print(f"    {line}")
        print(f"    (exit {r.returncode}, {took:.1f} s)\n")


if __name__ == "__main__":
    main()
