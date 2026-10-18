"""
conftest.py — a configuracao dos testes de sistema (M4: "automated tests
with pytest", slide 123).

    make pytest                                  # tudo (precisa de `make`)
    python3 -m pytest tests/system -m "not slow" # sem os testes demorados
    python3 -m pytest tests/system/test_interop.py --host <ip> --port 5000
                                                 # os cenarios contra OUTRO servidor
"""
import os

import pytest

from helpers import CLIENT, SERVER, Server


def pytest_addoption(parser):
    parser.addoption("--host", default=None,
                     help="correr test_interop.py contra este servidor (de outro grupo)")
    parser.addoption("--port", type=int, default=5000,
                     help="a porta desse servidor (5000)")


def pytest_configure(config):
    config.addinivalue_line("markers", "slow: demora mais do que alguns segundos")
    for b in (SERVER, CLIENT):
        if not os.access(b, os.X_OK):
            pytest.exit(f"{b} nao existe: correr `make` primeiro", returncode=2)


SANITIZERS = ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:",
              "WARNING: ThreadSanitizer")


def finish(s):
    """No fim de CADA teste, o servidor e encerrado com Ctrl+C (D24), como
    se faria a mao: tem de sair com 0, e sem relatorios dos sanitizers no log
    (com `make SAN=1` isto inclui as fugas de memoria, que o LeakSanitizer so
    ve numa saida normal). Assim cada teste tambem testa o encerramento."""
    rc = None
    if s.alive():
        try:
            rc = s.stop(timeout=15)
        except Exception:
            s.kill()
            raise
    log = s.log()
    bad = [l for l in log.splitlines() if any(k in l for k in SANITIZERS)]
    assert not bad, "o servidor teve relatorios dos sanitizers:\n" + log
    assert rc in (None, 0), f"o servidor saiu com {rc} no Ctrl+C:\n{log}"


@pytest.fixture
def server():
    """Um servidor nosso, novo para cada teste, com os prazos por omissao."""
    s = Server()
    yield s
    finish(s)


@pytest.fixture
def make_server():
    """Fabrica: make_server("-i", "2") para um servidor com outras opcoes.
    Todos os que forem criados sao encerrados (e verificados) no fim."""
    made = []

    def make(*args, port=None):
        s = Server(*args, port=port)
        made.append(s)
        return s

    yield make
    for s in made:
        finish(s)
