"""Проверки передачи ходов, отказов запуска и завершения процессов."""
import ctypes
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from test_cli import check_trace

BINARY = str(Path(sys.argv[1]).resolve())
HERE = Path(__file__).resolve().parent
MAXIMUM = ["--teams", "8", "--cars", "4", "--length", "120", "--lanes", "6",
           "--laps", "100", "--rounds", "10000", "--strategy", "random"]
# При гибели судьи собираем осиротевших детей сами, не полагаясь на init среды.
assert ctypes.CDLL(None).prctl(36, 1, 0, 0, 0) == 0  # PR_SET_CHILD_SUBREAPER


def children(pid):
    path = Path(f"/proc/{pid}/task/{pid}/children")
    return [int(value) for value in path.read_text().split()] if path.exists() else []


def wait_children(process, count):
    until = time.monotonic() + 3
    while time.monotonic() < until:
        found = children(process.pid)
        if len(found) == count:
            return found
        assert process.poll() is None, process.communicate()
        time.sleep(0.005)
    raise AssertionError("Судья не создал ожидаемое число процессов")


def gone(pids):
    assert all(not Path(f"/proc/{pid}").exists() for pid in pids), pids


with tempfile.TemporaryDirectory(prefix="akos-processes-") as directory:
    directory = Path(directory)
    library = directory / "faults.so"
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
                    str(HERE / "faults.c"), "-ldl", "-o", str(library)], check=True)
    environment = dict(os.environ, LD_PRELOAD=str(library))
    for name in list(environment):
        if name.startswith("AKOS_"):
            del environment[name]

    def launch(args=(), expected=0, **controls):
        process = subprocess.Popen([BINARY, *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, env=dict(environment, **controls))
        try:
            stdout, stderr = process.communicate(timeout=8)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
        assert process.returncode == expected, (controls, process.returncode, stderr)
        assert not children(os.getpid()), "После выхода судьи остались несобранные дети"
        assert not Path(f"/dev/shm/akos-race-{process.pid}").exists()
        if expected == 0:
            assert not stderr, stderr
        return stdout

    normal = launch()
    for strategy in ("greedy", "random"):
        args = ["--strategy", strategy, "--seed", "7"]
        first = launch(args)
        assert launch(args) == first
        assert launch(args, AKOS_EINTR="1") == first
        check_trace(first, 3, 2, 4, 24, 2)

    log = directory / "race.log"
    launch(["--quiet", "--log", str(log)])
    assert log.read_text() == normal
    # Повторный запуск после каждого отказа не требует ручного удаления памяти.
    failures = ["shm:1", "truncate:1", "map:1", "unlink:1"]
    failures += [f"sem:{n}" for n in range(1, 35)]
    failures += [f"fork:{n}" for n in range(1, 33)]
    for failure in failures:
        launch([*MAXIMUM, "--quiet"], expected=1, AKOS_FAIL=failure)
    for n in range(1, 35):
        launch([*MAXIMUM, "--quiet"], expected=1, AKOS_FAIL=f"destroy:{n}")
    launch([*MAXIMUM, "--quiet"], expected=1, AKOS_CHILD_EXIT="1")
    assert launch() == normal

    # Родитель и 32 настоящих однопоточных ребёнка, POSIX MAP_SHARED.
    # stdout удерживает большую гонку живой; задержка нужна только для наблюдения старта.
    for action in ("SIGINT", "SIGTERM", "group_INT", "group_TERM", "kill_child", "stop_child", "kill_judge"):
        process = subprocess.Popen([BINARY, *MAXIMUM], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   env=dict(environment, AKOS_SLOW_CHILD="1"), start_new_session=True)
        pids = []
        try:
            pids = wait_children(process, 32)
            assert len(list(Path(f"/proc/{process.pid}/task").iterdir())) == 1
            assert f"/dev/shm/akos-race-{process.pid} (deleted)" in Path(f"/proc/{process.pid}/maps").read_text()
            assert all(len(list(Path(f"/proc/{pid}/task").iterdir())) == 1 for pid in pids)
            if action == "kill_judge":
                process.kill()
            elif action.startswith("group_"):
                os.killpg(process.pid, signal.SIGINT if action == "group_INT" else signal.SIGTERM)
            elif action in ("SIGINT", "SIGTERM"):
                process.send_signal(getattr(signal, action))
            else:
                os.kill(pids[0], signal.SIGKILL if action == "kill_child" else signal.SIGSTOP)
            stdout, stderr = process.communicate(timeout=5)
            expected = -signal.SIGKILL if action == "kill_judge" else 1 if action.endswith("child") else 130 if "INT" in action else 143
            assert process.returncode == expected, (action, process.returncode, stderr)
            if expected > 1:
                assert not stderr, stderr
                assert "Результаты команд" in stdout.decode()
                check_trace(stdout.decode(), 8, 4, 6, 120, 100)
            if action == "kill_judge":
                for pid in pids:
                    os.waitpid(pid, 0)
            gone(pids)
            assert not Path(f"/dev/shm/akos-race-{process.pid}").exists()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            for pid in pids:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass

print(f"Процессы: {len(failures)} отказов запуска, 34 отказа destroy, EINTR, повторяемость и журнал проверены.")
print("Подтверждены 33 отдельных процесса; сигналы судье и группе, гибель и остановка ребёнка не оставляют участников и общей памяти.")
print("После SIGKILL судьи дети также завершаются, имя разделяемой памяти отсутствует.")
