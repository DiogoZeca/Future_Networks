"""
test_terminal.py — o nosso cliente NUM TERMINAL (um pseudo-terminal, modulo
pty), com um mini-emulador de ecra: "aparece uma vez so" e uma pergunta sobre
o ecra, nao sobre os bytes. Era o scripts/probe_m3.py §9 (correcao do M3:
P34-P36), mais o Ctrl+C do M4.

O Ctrl+Z nao se testa aqui (o processo nao tem uma shell com controlo de
jobs por cima): testa-se a mao (M3.md, "Como reproduzir").
"""
import fcntl
import os
import pty
import select
import struct
import termios
import time

import pytest

from helpers import CLIENT, join


class Ecra:
    """Um terminal minimo: o suficiente para o que o nosso cliente escreve
    ('\\r', '\\n', ESC [ K, texto). Guarda as linhas como ficam no ecra."""

    def __init__(self):
        self.linhas = [""]
        self.col = 0
        self.pend = b""

    def come(self, data):
        data = self.pend + data
        self.pend = b""
        i = 0
        while i < len(data):
            c = data[i:i + 1]
            if c == b"\x1b":
                if data[i + 1:i + 3] == b"[K":           # apagar ate ao fim da linha
                    self.linhas[-1] = self.linhas[-1][:self.col]
                    i += 3
                    continue
                if i + 3 > len(data):                     # sequencia partida: esperar
                    self.pend = data[i:]
                    return
                i += 3
                continue
            if c == b"\r":
                self.col = 0
            elif c == b"\n":
                self.linhas.append("")
                self.col = 0
            elif c != b"\a":                              # texto (UTF-8 inteiro)
                j = i + 1
                while j < len(data) and data[j:j + 1] not in (b"\x1b", b"\r", b"\n", b"\a"):
                    j += 1
                try:
                    t = data[i:j].decode()
                except UnicodeDecodeError as e:
                    if j == len(data) and j - (i + e.start) < 4:
                        self.pend = data[i:]              # partido entre leituras
                        return
                    t = data[i:j].decode(errors="replace")
                l = self.linhas[-1]
                self.linhas[-1] = l[:self.col] + t + l[self.col + len(t):]
                self.col += len(t)
                i = j
                continue
            i += 1

    def ultima(self):
        return self.linhas[-1]

    def com(self, texto):
        return [l for l in self.linhas if texto in l]


class Tty:
    """./bin/client num pseudo-terminal de 80x24."""

    def __init__(self, port, nick):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:                                 # filho: o terminal e o pty
            os.execv(CLIENT, [CLIENT, "127.0.0.1", str(port), nick])
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        self.ecra = Ecra()
        self.ler(0.8)

    def ler(self, quieto=0.4):
        """Le ate o cliente ficar calado `quieto` segundos."""
        while True:
            r, _, _ = select.select([self.fd], [], [], quieto)
            if not r:
                return
            try:
                d = os.read(self.fd, 65536)
            except OSError:                               # o cliente terminou
                return
            if not d:
                return
            self.ecra.come(d)

    def escreve(self, data):
        os.write(self.fd, data)
        self.ler()

    def canonico(self):
        """O terminal tem eco e modo canonico (como a shell o quer)?"""
        lf = termios.tcgetattr(self.fd)[3]
        return bool(lf & termios.ECHO) and bool(lf & termios.ICANON)

    def espera_fim(self, timeout=5):
        fim = time.time() + timeout
        while time.time() < fim:
            pid, st = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                return st
            self.ler(0.1)
        raise TimeoutError("o cliente nao terminou")

    def fecha(self):
        try:
            os.close(self.fd)
        except OSError:
            pass


@pytest.fixture
def ana_tty(server):
    rui = join(server.port, "rui")
    t = Tty(server.port, "ana")
    rui.recv_all()
    yield t, rui
    t.fecha()


def chats(conn):
    return [m.get("text") for m in conn.recv_all() if m.get("type") == "CHAT"]


def test_prompt_boas_vindas_e_modo_raw(ana_tty):
    t, _ = ana_tty
    assert t.ecra.ultima() == "ana> "
    assert t.ecra.com("* welcome, ana! commands: /list, /msg <nick> <text>, /quit")
    assert not t.canonico()


def test_mensagem_a_meio_da_escrita_nao_se_mistura(ana_tty):           # P34
    t, rui = ana_tty
    t.escreve(b"meio")
    rui.send({"type": "MSG", "text": "boas"})
    rui.recv_all()
    t.ler()
    assert t.ecra.com("<rui> boas") == ["<rui> boas"]
    assert t.ecra.ultima() == "ana> meio"


def test_o_que_se_envia_aparece_uma_vez(ana_tty):                       # P35
    t, rui = ana_tty
    t.escreve(b"meio\r")
    assert chats(rui) == ["meio"]
    assert t.ecra.com("meio") == ["<ana> meio"]
    assert t.ecra.ultima() == "ana> "


def test_enter_sozinho_nao_faz_nada(ana_tty):                           # P36
    t, rui = ana_tty
    antes = list(t.ecra.linhas)
    t.escreve(b"\r\r\r\r\r")
    assert t.ecra.linhas == antes
    assert rui.recv_all() == []


def test_backspace_utf8_setas_e_ctrl_u(ana_tty):
    t, rui = ana_tty
    t.escreve("olé".encode() + b"\x7f" + b"a" + b"\x1b[A\x1b[D" + b"!\r")
    assert chats(rui) == ["ola!"]
    t.escreve(b"lixo\x15fica\r")
    assert chats(rui) == ["fica"]


def test_msg_fica_so_o_eco_local(ana_tty):
    t, rui = ana_tty
    t.escreve(b"/msg rui segredo\r")
    assert [(m.get("text"), m.get("private")) for m in rui.recv_all()] == [("segredo", True)]
    assert t.ecra.com("segredo") == ["<ana -> rui> (privado) segredo"]


def test_ctrl_d_sai_e_repoe_o_terminal(ana_tty):
    t, rui = ana_tty
    os.write(t.fd, b"\x04")
    st = t.espera_fim()
    assert os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0
    assert t.canonico()
    assert rui.recv_all() == [{"type": "EVENT", "text": "ana left"}]


def test_ctrl_c_manda_quit_e_repoe_o_terminal(ana_tty, server):          # M4
    t, rui = ana_tty
    os.write(t.fd, b"meio escrito\x03")
    st = t.espera_fim()
    assert os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0   # ja nao morre pelo SIGINT
    assert t.canonico()
    assert server.wait_log("(ana) saiu: QUIT")
    assert rui.recv_all() == [{"type": "EVENT", "text": "ana left"}]
