"""Проверки пункта 5: завершение, отказы ресурсов и порядок освобождения."""
import collections
import ctypes
import errno
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
HERE = Path(__file__).resolve().parent
MAXIMUM = ["--teams", "8", "--cars", "4", "--length", "33", "--lanes", "2",
           "--strategy", "random", "--seed", "0"]
BASE_ENV = {k: v for k, v in os.environ.items() if not k.startswith("AKOS_TEST_")}


def read_events(path, complete=True):
    events = path.read_text().splitlines() if path.exists() else []
    counts = collections.Counter(events)
    assert counts["create"] == counts["join"], counts
    if complete:
        assert counts["sem_init"] == counts["sem_destroy"], counts
        assert counts["mutex_init"] == counts["mutex_destroy"], counts
    joined = [i for i, e in enumerate(events) if e == "join"]
    destroyed = [i for i, e in enumerate(events) if e.endswith("destroy_attempt")]
    if joined and destroyed:
        assert max(joined) < min(destroyed), events
    assert counts["fclose"] == 1, counts
    assert events[-1] == "fclose", events
    return counts


with tempfile.TemporaryDirectory(prefix="akos-shutdown-") as directory:
    directory = Path(directory)
    shim = directory / "faults.so"
    subprocess.run([os.environ.get("CC", "gcc"), "-std=c11", "-O2", "-fPIC", "-shared",
                    str(HERE / "faults.c"), "-ldl", "-pthread", "-o", str(shim)], check=True)
    sequence = 0

    def launch(args=MAXIMUM, expected=0, complete=True, **controls):
        global sequence
        sequence += 1
        events = directory / f"events-{sequence}.log"
        log = directory / f"race-{sequence}.log"
        environment = dict(BASE_ENV, LD_PRELOAD=str(shim), AKOS_TEST_EVENTS=str(events), **controls)
        result = subprocess.run([str(BINARY), *args, "--quiet", "--log", str(log)],
                                capture_output=True, text=True, env=environment, timeout=10)
        assert result.returncode == expected, (controls, result.returncode, result.stderr)
        return result, log.read_text(), read_events(events, complete)

    for args, reason in [([], "все машины финишировали или сошли"),
                         (["--rounds", "1"], "достигнут предел раундов"),
                         (MAXIMUM, "все машины финишировали или сошли"),
                         (["--teams", "1", "--cars", "1", "--length", "3", "--lanes", "1", "--laps", "1"],
                          "все машины финишировали или сошли")]:
        result, log, counts = launch(args)
        assert reason in result.stdout and "Результаты команд" in log
        assert not result.stderr
        if args == MAXIMUM:
            assert "сход" in log and "финиш" in log

    # Каждая возможная точка отказа: 32 create, 33 sem_init и mutex_init.
    failures = [("AKOS_TEST_FAIL_CREATE", n) for n in range(1, 33)]
    failures += [("AKOS_TEST_FAIL_SEM", n) for n in range(1, 34)]
    failures += [("AKOS_TEST_FAIL_MUTEX", 1)]
    for variable, n in failures:
        result, log, counts = launch(expected=1, **{variable: str(n)})
        assert "Не удалось создать потоки машин" in result.stderr
        assert "Гонка завершена" not in log
        if variable == "AKOS_TEST_FAIL_CREATE":
            assert counts["create"] == n - 1, counts
        elif variable == "AKOS_TEST_FAIL_SEM":
            assert counts["sem_init"] == n - 1, counts
        else:
            assert counts["create"] == counts["sem_init"] == counts["mutex_init"] == 0

    # Ошибка destroy не прерывает очистку остальных объектов и закрытие журнала.
    cleanup_failures = [("AKOS_TEST_FAIL_DESTROY_SEM", n) for n in range(1, 34)]
    cleanup_failures += [("AKOS_TEST_FAIL_DESTROY_MUTEX", 1)]
    for variable, n in cleanup_failures:
        result, log, counts = launch(expected=1, complete=False, **{variable: str(n)})
        assert "Не удалось освободить ресурсы потоков" in result.stderr
        assert "Результаты команд" in log
        assert counts["join"] == 32
        assert counts["sem_destroy_attempt"] == 33 and counts["mutex_destroy_attempt"] == 1
        assert counts["sem_init"] - counts["sem_destroy"] == (variable == "AKOS_TEST_FAIL_DESTROY_SEM")
        assert counts["mutex_init"] - counts["mutex_destroy"] == (variable == "AKOS_TEST_FAIL_DESTROY_MUTEX")

    for cleanup_failure in ("AKOS_TEST_FAIL_DESTROY_SEM", "AKOS_TEST_FAIL_DESTROY_MUTEX"):
        result, log, counts = launch(expected=1, complete=False,
                                     AKOS_TEST_FAIL_CREATE="5", **{cleanup_failure: "1"})
        assert "Ошибка освобождения ресурсов после неудачного запуска" in result.stderr
        assert "Не удалось создать потоки машин" in result.stderr
        assert counts["create"] == counts["join"] == 4
        assert counts["sem_destroy_attempt"] == 6 and counts["mutex_destroy_attempt"] == 1
        assert "Гонка завершена" not in log

    # Сигнал в начале, середине и конце создания рабочих потоков.
    for stop_signal in (signal.SIGINT, signal.SIGTERM):
        for nth in (1, 16, 32):
            result, log, counts = launch(expected=128 + stop_signal,
                                         AKOS_TEST_SIGNAL_CREATE=str(nth),
                                         AKOS_TEST_SIGNAL_NUMBER=str(stop_signal.value))
            assert counts["signal_create"] == 1 and counts["join"] == 32
            assert "прерывание пользователем" in log and "Раундов: 0." in log
            assert "Результаты команд" in result.stdout

    libc = ctypes.CDLL(None, use_errno=True)
    libc.tgkill.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
    libc.tgkill.restype = ctypes.c_int

    # Пауза 5 секунд: два сигнала на стартовом кадре, затем каждый из них
    # адресно судье и рабочему потоку во время хода. В тесте рабочих кадров
    # пропускаются только паузы судьи; пауза машины остаётся настоящей.
    for stop_signal in (signal.SIGINT, signal.SIGTERM):
        for phase in ("start", "worker-to-judge", "worker-to-worker"):
            sequence += 1
            events = directory / f"events-{sequence}.log"
            log = directory / f"race-{sequence}.log"
            master, slave = os.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
            environment = dict(BASE_ENV, TERM="xterm", LD_PRELOAD=str(shim),
                               AKOS_TEST_EVENTS=str(events))
            if phase != "start":
                environment["AKOS_TEST_SKIP_MAIN_DELAY"] = "1"
            process = subprocess.Popen([str(BINARY), *MAXIMUM, "--laps", "100", "--delay", "5000",
                                        "--log", str(log)], stdout=slave, stderr=subprocess.PIPE, env=environment)
            os.close(slave)
            data = bytearray()
            marker = b"\x1b[J" if phase == "start" else "Шаг 0/".encode()
            try:
                deadline = time.monotonic() + 5
                while marker not in data and time.monotonic() < deadline:
                    if select.select([master], [], [], .05)[0]:
                        data.extend(os.read(master, 65536))
                assert marker in data, (phase, data.decode())
                tasks = sorted(int(p.name) for p in Path(f"/proc/{process.pid}/task").iterdir())
                assert len(tasks) == 33, tasks
                target = next(t for t in tasks if t != process.pid) if phase == "worker-to-worker" else process.pid
                started = time.monotonic()
                assert libc.tgkill(process.pid, target, stop_signal) == 0, ctypes.get_errno()
                while time.monotonic() - started < 2.5:
                    if select.select([master], [], [], .02)[0]:
                        try:
                            part = os.read(master, 65536)
                        except OSError as error:
                            if error.errno == errno.EIO:
                                break
                            raise
                        if not part:
                            break
                        data.extend(part)
                    elif process.poll() is not None:
                        break
                code = process.wait(timeout=.5)
                assert code == 128 + stop_signal, (phase, code, process.stderr.read())
                assert time.monotonic() - started < 2.5
                text = data.decode()
                assert "\x1b[?25h\x1b[?1049l" in text and "Результаты команд" in text
                assert not process.stderr.read()
                counts = read_events(events)
                assert counts["create"] == counts["join"] == 32
                assert "прерывание пользователем" in log.read_text()
                if phase != "start":
                    assert "Раундов: 1." in log.read_text(), "Текущий раунд должен завершиться целиком"
            finally:
                os.close(master)
                process.stderr.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

print("Завершение: 4 обычных сценария, 66 отказов запуска, 34 отказа освобождения, "
      "2 совмещённых отказа и 12 сценариев SIGINT/SIGTERM проверены; "
      "join предшествует destroy, журнал закрывается последним.")
