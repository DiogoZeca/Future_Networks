#!/usr/bin/env python3

import os
import socket
import struct
import subprocess
import sys
import time

PORT = sys.argv[1] if len(sys.argv) > 1 else "5099"
HOST = "127.0.0.1"
SERVER = "./bin/server"


def log(msg):
    print(f"[probe] {msg}")


def connect():
    return socket.create_connection((HOST, int(PORT)), timeout=3)


def main():
    if not os.path.exists(SERVER):
        sys.exit(f"{SERVER} não existe — correr `make` primeiro, a partir da raiz do repo.")

    srv = subprocess.Popen([SERVER, PORT], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
    time.sleep(0.4)

    # 1. Eco básico: o caminho feliz.
    c1 = connect()
    c1.sendall(b"hello\n")
    log(f"1. eco basico      -> {c1.recv(100)!r}")

    # 2. Servidor iterativo: o 2.º cliente completa o handshake TCP mas fica
    #    na fila do listen() sem ser servido. Ligado != aceite pela aplicação.
    c2 = connect()
    c2.sendall(b"second\n")
    c2.settimeout(1.0)
    try:
        log(f"2. 2.o cliente     -> {c2.recv(100)!r}  (NAO ficou em fila!)")
    except socket.timeout:
        log("2. 2.o cliente     -> liga mas nao e servido = fila do listen(), como esperado")

    # 3. A lição central: uma mensagem grande chega partida em vários recv().
    big = b"A" * 10000 + b"\n"
    c1.sendall(big)
    got = b""
    c1.settimeout(2.0)
    try:
        while len(got) < len(big):
            chunk = c1.recv(65536)
            if not chunk:
                break
            got += chunk
    except socket.timeout:
        pass
    ok = "OK" if got == big else "TRUNCADO/DIFERENTE"
    log(f"3. {len(big)} bytes    -> ecoados {len(got)} ({ok}) — ver o log do servidor")

    # 4. Fecho abrupto com RST (equivalente a um kill -9 ou a perder energia
    #    *com* a pilha TCP ainda viva). O erro aparece no send() do servidor.
    c3 = connect()
    c3.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    c3.sendall(b"abrupt")
    c3.close()
    time.sleep(0.3)
    log("4. RST (SO_LINGER) -> ver 'Connection reset by peer' no log do servidor")

    # 5. Half-close: fechar só o sentido de envio. O servidor vê recv()==0.
    c1.shutdown(socket.SHUT_WR)
    c1.settimeout(1.0)
    try:
        rest = c1.recv(100)
        log(f"5. half-close      -> servidor devolveu {rest!r} (b'' = fechou tambem)")
    except socket.timeout:
        log("5. half-close      -> servidor NAO reparou no FIN")

    # c2 fecha com dados por ler no buffer -> o kernel manda RST, não FIN.
    c1.close()
    c2.close()
    time.sleep(0.5)

    srv.terminate()
    try:
        out, _ = srv.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        srv.kill()
        out, _ = srv.communicate()
    print("\n--- LOG DO SERVIDOR ---")
    print(out)


if __name__ == "__main__":
    main()
