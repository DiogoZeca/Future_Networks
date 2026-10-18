"""
test_interop.py — os cenarios de tests/interop/*.txt (a especificacao em
forma executavel), cada um nos 3 modos de envio, pelo mesmo motor do
scripts/interop.py.

    python3 -m pytest tests/system/test_interop.py                 # o nosso servidor
    python3 -m pytest tests/system/test_interop.py --host <ip> --port 5000
                                                                   # o de outro grupo
"""
import pathlib
import sys

import pytest

from conftest import finish
from helpers import ROOT, Server

sys.path.insert(0, str(ROOT / "scripts"))
import interop  # noqa: E402  (o mesmo codigo que se corre a mao contra outros grupos)

SCENARIOS = sorted((ROOT / "tests" / "interop").glob("*.txt"))


@pytest.fixture
def target(request):
    host = request.config.getoption("--host")
    if host:
        yield host, request.config.getoption("--port")
        return
    s = Server()
    yield "127.0.0.1", s.port
    finish(s)


@pytest.mark.parametrize("mode", interop.MODES)
@pytest.mark.parametrize("path", SCENARIOS, ids=[p.stem for p in SCENARIOS])
def test_cenario(path, mode, target):
    err = interop.run(*target, interop.load(path), mode)
    assert err is None, err
