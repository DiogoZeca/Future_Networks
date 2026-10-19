#!/usr/bin/env python3
"""
discover.py — quem responde a um DISCOVER nesta rede, e COMO responde (M5).

Para o dia da interoperabilidade: manda o pedido por broadcast UDP e mostra
cada resposta EM BRUTO, com o IP de origem, e o que o nosso cliente faria
com ela. Serve para ver o formato de cada grupo (tcp_port? port? '\\n'?) e
para perceber se o problema e a rede (ninguem responde) ou o formato
(responde, mas o nosso cliente nao a aproveita).

    python3 scripts/discover.py                  # 255.255.255.255 e o broadcast de cada interface
    python3 scripts/discover.py --plain          # o pedido como texto: DISCOVER
    python3 scripts/discover.py --to 192.168.1.20 --port 5001 --wait 3

So a biblioteca padrao. Os broadcasts de cada interface vem do `ip -4 -o
addr` (Linux); sem ele fica so o 255.255.255.255.
"""
import argparse
import json
import socket
import subprocess
import time


def interface_broadcasts():
    """Os broadcasts dirigidos (ex.: 192.168.1.255) das interfaces IPv4."""
    try:
        out = subprocess.run(["ip", "-4", "-o", "addr", "show"], capture_output=True,
                             text=True, timeout=2).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    found = []
    for line in out.splitlines():
        f = line.split()
        if "brd" in f and f[1] != "lo":
            found.append(f[f.index("brd") + 1])
    return found


def verdict(data):
    """O que o NOSSO cliente faria com esta resposta (as regras do disco.c)."""
    if len(data) > 512:
        return "IGNORADA: mais de 512 bytes"
    try:
        m = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        return "IGNORADA: nao e JSON (UTF-8)"
    if not isinstance(m, dict):
        return "IGNORADA: nao e um objeto JSON"
    if "type" in m and m["type"] != "SERVER":
        return f"IGNORADA: type {m['type']!r} (esperava SERVER ou nenhum)"
    key = "tcp_port" if "tcp_port" in m else "port"
    p = m.get(key)
    if not isinstance(p, int) or isinstance(p, bool) or not 1 <= p <= 65535:
        return f"IGNORADA: sem porta valida em {key!r}"
    extra = [k for k in ("ip", "host", "addr") if k in m]
    note = f" (o campo {extra} e ignorado: usa-se o IP de origem)" if extra else ""
    return f"ok: porta TCP {p} via {key!r}{note}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=5001, help="porta UDP (5001)")
    ap.add_argument("--wait", type=float, default=2.0, help="segundos a esperar (2)")
    ap.add_argument("--to", action="append", help="destino (repetivel); por omissao, broadcast")
    ap.add_argument("--plain", action="store_true", help='pedido em texto: DISCOVER')
    a = ap.parse_args()

    request = b"DISCOVER" if a.plain else b'{"type":"DISCOVER"}'
    targets = a.to or ["255.255.255.255", *interface_broadcasts()]

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    for t in dict.fromkeys(targets):             # sem repetidos, pela ordem
        try:
            s.sendto(request, (t, a.port))
            print(f"> {request.decode()} para {t}:{a.port}")
        except OSError as e:
            print(f"! {t}:{a.port}: {e}")

    seen = 0
    end = time.time() + a.wait
    while True:
        left = end - time.time()
        if left <= 0:
            break
        s.settimeout(left)
        try:
            data, (ip, port) = s.recvfrom(65535)
        except socket.timeout:
            break
        seen += 1
        print(f"< {ip}:{port}  {data!r}")
        print(f"    {verdict(data)}")
    if seen == 0:
        print(f"ninguem respondeu em {a.wait:g} s (firewall? sudo ufw allow {a.port}/udp no servidor)")


if __name__ == "__main__":
    main()
