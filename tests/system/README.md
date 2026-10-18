# Testes de sistema (M4)

O servidor e o cliente **a sério**, com ligações TCP reais, em pytest (slide
123: *"automated tests with pytest"*). Só usam a biblioteca padrão do Python.

```sh
make pytest                                   # tudo (~1,5 min)
make pytest PYTEST_ARGS='-m "not slow"'       # sem os 4 demorados
make clean && make pytest SAN=1               # com ASan/UBSan/LeakSanitizer
python3 -m pytest tests/system/test_interop.py --host <ip> --port 5000
                                              # os cenários contra outro grupo
```

| Ficheiro | O que testa |
|---|---|
| `test_chat.py` | entrar, broadcast, erros, saídas, 503; 409, `PRIV`, `LIST`, `QUIT`, limites (M2, M3) |
| `test_concurrency.py` | corridas e clientes que não leem — `slow` (M2, M3) |
| `test_client.py` | o nosso cliente por um pipe, contra o nosso servidor e contra servidores falsos; keepalive, servidor mudo, Ctrl+C (M3, M4) |
| `test_terminal.py` | o nosso cliente num pseudo-terminal: a linha de escrita, Ctrl+D, Ctrl+C (correção do M3, M4) |
| `test_interop.py` | os cenários de `tests/interop/` × 3 modos de envio |
| `test_robustness.py` | idle timeout, fecho sem RST, RST/half-close/`kill -9`, fuzz de JSON, Ctrl+C, log (M4) |

O `conftest.py` arranca um servidor novo por teste, numa porta livre, com o log
num ficheiro (`helpers.py`); no fim de cada teste encerra-o com **Ctrl+C** e
verifica que saiu com 0 e sem relatórios dos sanitizers. Detalhe e resultados
em [`docs/milestones/M4.md`](../../docs/milestones/M4.md).
