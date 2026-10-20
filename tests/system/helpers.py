"""
helpers.py — o que os testes de sistema partilham: arrancar o servidor numa
porta livre, ligar clientes "a serio" e ler o que cada um recebe.

Tudo com a biblioteca padrao, como o scripts/interop.py: qualquer pessoa (ou
outro grupo) corre isto sem instalar mais nada alem do pytest.
"""
import json
import os
import pathlib
import select
import signal
import socket
import ssl
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
SERVER = str(ROOT / "bin" / "server")
CLIENT = str(ROOT / "bin" / "client")


def free_port():
    """Uma porta TCP livre agora (o kernel escolhe-a; ha uma corrida pequena
    ate o servidor a usar, aceitavel em testes)."""
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def free_udp_port():
    """Uma porta UDP livre agora (M5: a descoberta de cada servidor de teste)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 0))
    port = s.getsockname()[1]
    s.close()
    return port


_TLS_CERT = None


def tls_cert():
    """M6: (certificado, chave, fingerprint) de teste, gerados UMA vez por
    sessao do pytest pelo proprio scripts/gen-cert.sh (assim o script tambem
    e testado), num diretorio temporario fora do repositorio — a chave nunca
    chega perto do git."""
    global _TLS_CERT
    if _TLS_CERT is None:
        d = tempfile.mkdtemp(prefix="fn-tls-")
        p = subprocess.run(["sh", str(ROOT / "scripts" / "gen-cert.sh"), "certs"],
                           cwd=d, capture_output=True, text=True, timeout=30)
        if p.returncode != 0:
            raise RuntimeError(f"gen-cert.sh falhou:\n{p.stdout}{p.stderr}")
        lines = p.stdout.splitlines()
        i = next(i for i, l in enumerate(lines) if l.startswith("fingerprint SHA-256"))
        _TLS_CERT = (os.path.join(d, "certs", "server.pem"),
                     os.path.join(d, "certs", "server.key"), lines[i + 1].strip())
    return _TLS_CERT


def tls_socket(port=None, host="127.0.0.1", sock=None):
    """Uma ligacao TLS ao nosso servidor (ou sobre `sock`, ja ligado), SEM
    verificar o certificado: e o teste, nao o cliente — quem verifica o
    fingerprint e o ./bin/client."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx.wrap_socket(sock or socket.create_connection((host, port), timeout=3))


class Server:
    """O nosso ./bin/server, num processo a parte, com o log num ficheiro.

    O log vai para um ficheiro e NAO para um pipe: um pipe que ninguem le
    enche aos 64 KB, o servidor fica bloqueado a escrever o log e o chat
    inteiro para (M2.md, nota 6).

    M5: cada servidor tem a SUA porta UDP de descoberta (udp_port), livre,
    em vez da 5001 de todos: assim os testes nao se ouvem uns aos outros (nem
    a um servidor a serio que esteja a correr na maquina). Quem passar "-u"
    escolhe a sua."""

    def __init__(self, *args, port=None, tls=False):
        self.port = port or free_port()
        if "-u" in args:
            u = args[args.index("-u") + 1]
            self.udp_port = int(u) if u.isdigit() else None
        else:
            self.udp_port = free_udp_port()
            args = ("-u", str(self.udp_port), *args)
        # M6: tls=True acrescenta a porta TLS (tls_port), com o certificado de
        # teste (tls_cert()). A porta em claro continua la, como a serio.
        self.tls_port = None
        if tls:
            cert, key, _ = tls_cert()
            self.tls_port = free_port()
            args = ("-T", str(self.tls_port), "-C", cert, "-K", key, *args)
        self._log = tempfile.TemporaryFile(mode="w+")
        self.proc = subprocess.Popen([SERVER, *args, str(self.port)],
                                     stdout=self._log, stderr=subprocess.STDOUT)
        # Esperar ATE aceitar ligacoes, e nao um tempo fixo (M3.md, nota 10).
        fim = time.time() + 5
        while time.time() < fim:
            if self.proc.poll() is not None:
                raise RuntimeError(f"o servidor nao arrancou:\n{self.log()}")
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.02)
        # Essa ligacao de teste aparece no log como "saiu sem nick: FIN".
        # A porta TLS espera-se pelo log e nao com uma ligacao: um connect
        # em claro na porta TLS seria um handshake falhado a mais no log.
        if tls and not self.wait_log("TLS na porta"):
            raise RuntimeError(f"o servidor nao abriu a porta TLS:\n{self.log()}")

    def log(self):
        self._log.seek(0)
        return self._log.read()

    def wait_log(self, text, timeout=5.0):
        """Espera ate `text` aparecer no log. Devolve True/False."""
        fim = time.time() + timeout
        while time.time() < fim:
            if text in self.log():
                return True
            time.sleep(0.05)
        return False

    def alive(self):
        return self.proc.poll() is None

    def stop(self, sig=signal.SIGINT, timeout=10):
        """Manda o sinal e espera que saia. Devolve o codigo de saida."""
        if self.alive():
            self.proc.send_signal(sig)
        return self.proc.wait(timeout=timeout)

    def kill(self):
        if self.alive():
            self.proc.kill()
            self.proc.wait()


class Conn:
    """Uma ligacao TCP com o protocolo do slide 124: JSON por linha."""

    def __init__(self, port, host="127.0.0.1", sock=None):
        self.sock = sock or socket.create_connection((host, port), timeout=3)
        self.buf = b""
        self.closed = False             # o servidor fechou (FIN)
        self.reset = False              # ... ou abortou (RST)

    def send(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode())

    def send_raw(self, data):
        self.sock.sendall(data)

    def _fill(self, timeout):
        """Le o que houver ate `timeout`. False se nao chegou nada."""
        if self.closed:
            return False
        # M6: com TLS pode haver bytes JA decifrados dentro do SSL sem que o
        # socket tenha nada (o mesmo caso do SSL_pending no servidor): o
        # select() diria "nada" e o teste perdia mensagens que ja chegaram.
        pending = getattr(self.sock, "pending", None)
        if not (pending and pending() > 0):
            r, _, _ = select.select([self.sock], [], [], timeout)
            if not r:
                return False
        try:
            d = self.sock.recv(65536)
        except ConnectionResetError:
            self.closed = self.reset = True
            return False
        except ssl.SSLError:            # M6: o TLS acabou mal (alerta, record estragado)
            self.closed = True
            return False
        if not d:
            self.closed = True
            return False
        self.buf += d
        return True

    def _pop(self):
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line)

    def recv_all(self, timeout=0.4):
        """Tudo o que chegar ate o servidor ficar calado `timeout` segundos."""
        out = []
        while True:
            while b"\n" in self.buf:
                line = self.buf.split(b"\n", 1)[0]
                if line.strip():
                    out.append(self._pop())
                else:
                    self._pop_raw()
            if not self._fill(timeout):
                return out

    def _pop_raw(self):
        _, _, self.buf = self.buf.partition(b"\n")

    def next(self, timeout=3.0, skip_events=False):
        """A proxima mensagem (None se nada chegar em `timeout`)."""
        fim = time.time() + timeout
        while True:
            while b"\n" in self.buf:
                m = self._pop()
                if skip_events and m.get("type") == "EVENT":
                    continue
                return m
            left = fim - time.time()
            if left <= 0 or not self._fill(left):
                return None

    def wait_closed(self, timeout=3.0):
        """Espera que o servidor feche, deitando fora o que chegar.
        Devolve "FIN", "RST" ou None (nao fechou)."""
        fim = time.time() + timeout
        while not self.closed and time.time() < fim:
            self._fill(fim - time.time())
        if not self.closed:
            return None
        return "RST" if self.reset else "FIN"

    def close(self):
        """Fecho ordeiro, e esperar que o servidor feche tambem (senao o nick
        pode ainda estar ocupado no teste seguinte: M3.md, P33)."""
        try:
            self.sock.shutdown(socket.SHUT_WR)
            self.wait_closed(1.0)
        except OSError:
            pass
        self.sock.close()


def welcome(nick):
    """O EVENT de boas-vindas que segue o OK a um NICK (D22)."""
    return {"type": "EVENT",
            "text": f"welcome, {nick}! commands: /list, /msg <nick> <text>, /quit"}


OK = {"type": "OK"}


def join(port, nick, tls=False):
    """Liga, faz NICK e consome o OK e as boas-vindas. tls=True: por TLS
    (M6), com `port` a porta TLS do servidor."""
    c = Conn(port, sock=tls_socket(port) if tls else None)
    c.send({"type": "NICK", "nick": nick})
    got = [c.next(), c.next()]
    assert got == [OK, welcome(nick)], f"{nick} nao entrou: {got}"
    return c


def no_events(msgs):
    return [m for m in msgs if m.get("type") != "EVENT"]


def chat(frm, text, private=False):
    return {"type": "CHAT", "from": frm, "text": text, "private": private}
