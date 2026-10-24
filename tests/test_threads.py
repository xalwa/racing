"""Сравнение с ИДЗ №1, ошибки POSIX-ресурсов и проверка потоков в Linux."""
import collections
import ctypes
import fcntl
import os
from pathlib import Path
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

BINARY = Path(sys.argv[1]).resolve()
REFERENCE = sys.argv[2]
HERE = Path(__file__).resolve().parent


def run(binary, args=(), env=None, expected=0):
    result = subprocess.run([str(binary), *args], capture_output=True, text=True,
                            timeout=10, env=env)
    assert result.returncode == expected, (args, result.returncode, result.stderr)
    return result


with tempfile.TemporaryDirectory(prefix="akos-thread-tests-") as folder:
    folder = Path(folder)
    reference = folder / "race-v1"
    # Читаем исходник ИДЗ №1 из сохранённого коммита. Рабочие файлы и ветка
    # при этом не меняются, а сравнение не зависит от размещения папок.
    source = subprocess.run(["git", "-C", str(HERE.parent), "show", f"{REFERENCE}:main.c"],
                            capture_output=True, text=True, check=True).stdout
    reference_source = folder / "reference.c"
    reference_source.write_text(source)
    subprocess.run([os.environ.get("CC", "gcc"), "-std=c11", "-O2", str(reference_source),
                    "-o", str(reference)], check=True)
    shim = folder / "faults.so"
    subprocess.run([os.environ.get("CC", "gcc"), "-std=c11", "-O2", "-fPIC", "-shared",
                    str(HERE / "faults.c"), "-ldl", "-pthread", "-o", str(shim)], check=True)
    cases = []
    for strategy in ("greedy", "random"):
        for seed in range(30):
            cases.append(["--teams", str(1 + seed % 8), "--cars", str(1 + seed % 4),
                          "--lanes", str(1 + seed % 6), "--length", str(33 + seed),
                          "--laps", str(1 + seed % 3), "--rounds", "200", "--strategy", strategy,
                          "--seed", str(seed)])
    cases += [[], ["--rounds", "1"], ["--teams", "8", "--cars", "4", "--length", "33",
               "--lanes", "2", "--strategy", "random", "--seed", "0"],
              ["--teams", "1", "--cars", "1", "--lanes", "1", "--length", "3", "--laps", "1"]]
    for args in cases:
        assert run(BINARY, args).stdout == run(reference, args).stdout, args

    environment = dict(os.environ, LD_PRELOAD=str(shim))
    maximum = ["--teams", "8", "--cars", "4", "--length", "33", "--lanes", "2",
               "--strategy", "random", "--seed", "0"]
    expected_trace = run(reference, maximum).stdout
    for _ in range(10):
        assert run(BINARY, maximum, dict(environment, AKOS_TEST_JITTER="1")).stdout == expected_trace

    events = folder / "eintr.log"
    result = run(BINARY, maximum, dict(environment, AKOS_TEST_EINTR="1",
                                      AKOS_TEST_EVENTS=str(events)))
    assert result.stdout == expected_trace
    assert events.read_text().splitlines().count("eintr") == 33

    fault_cases = [("AKOS_TEST_FAIL_CREATE", str(n)) for n in (1, 2, 6, 17, 32)]
    fault_cases += [("AKOS_TEST_FAIL_SEM", str(n)) for n in (1, 2, 7, 18, 33)]
    fault_cases += [("AKOS_TEST_FAIL_MUTEX", "1")]
    for index, (variable, value) in enumerate(fault_cases):
        events = folder / f"events-{index}.log"
        result = run(BINARY, maximum, dict(environment, AKOS_TEST_EVENTS=str(events),
                                          **{variable: value}), expected=1)
        assert "Не удалось создать потоки машин" in result.stderr
        assert "Гонка завершена" not in result.stdout
        counts = collections.Counter(events.read_text().splitlines() if events.exists() else [])
        assert counts["create"] == counts["join"], counts
        assert counts["sem_init"] == counts["sem_destroy"], counts
        assert counts["mutex_init"] == counts["mutex_destroy"], counts
        if variable == "AKOS_TEST_FAIL_CREATE":
            assert counts["create"] == int(value) - 1, counts
        elif variable == "AKOS_TEST_FAIL_SEM":
            assert counts["sem_init"] == int(value) - 1, counts
        else:
            assert not counts, counts

    events = folder / "success.log"
    run(BINARY, maximum, dict(environment, AKOS_TEST_EVENTS=str(events)))
    counts = collections.Counter(events.read_text().splitlines())
    assert counts == {"create": 32, "join": 32, "sem_init": 33, "sem_destroy": 33,
                      "mutex_init": 1, "mutex_destroy": 1,
                      "sem_destroy_attempt": 33, "mutex_destroy_attempt": 1}, counts

# Реальный процесс: все 32 потока одновременно существуют, пока первый ход
# отображается в терминале. Сигнал направляется непосредственно судье/машине.
libc = ctypes.CDLL(None, use_errno=True)
libc.tgkill.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
libc.tgkill.restype = ctypes.c_int
for target_worker in (False, True):
    master, slave = os.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    process = subprocess.Popen([str(BINARY), *maximum, "--laps", "100", "--delay", "500"],
                               stdout=slave, stderr=subprocess.PIPE,
                               env=dict(os.environ, TERM="xterm"))
    os.close(slave)
    data = bytearray()
    try:
        deadline = time.monotonic() + 5
        while b"\xd0\xa8\xd0\xb0\xd0\xb3 1/" not in data and time.monotonic() < deadline:
            if select.select([master], [], [], .1)[0]:
                data.extend(os.read(master, 65536))
        assert "Шаг 1/" in data.decode(), "Не дождались первого перехода машины"
        tasks = sorted(int(p.name) for p in Path(f"/proc/{process.pid}/task").iterdir())
        assert len(tasks) == 33, tasks
        target = next(t for t in tasks if t != process.pid) if target_worker else process.pid
        started = time.monotonic()
        assert libc.tgkill(process.pid, target, signal.SIGTERM) == 0, ctypes.get_errno()
        while process.poll() is None and time.monotonic() - started < 2:
            if select.select([master], [], [], .02)[0]:
                try:
                    data.extend(os.read(master, 65536))
                except OSError:
                    break
        code = process.wait(timeout=1)
        assert code == 143, (code, process.stderr.read())
        while select.select([master], [], [], 0)[0]:
            try:
                part = os.read(master, 65536)
            except OSError:
                break
            if not part:
                break
            data.extend(part)
        assert "Результаты команд" in data.decode()
        assert b"\x1b[?25h\x1b[?1049l" in data
    finally:
        os.close(master)
        process.stderr.close()
        if process.poll() is None:
            process.kill()
            process.wait()

print("Потоки Python: 64 сравнения с ИДЗ №1, 10 запусков с задержками планирования, "
      "11 отказов инициализации, 33 возврата EINTR, освобождение ресурсов, "
      "33 живых потока и адресные сигналы проверены.")
