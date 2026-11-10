"""Сравнение реализаций и жизненный цикл независимо запускаемых машин."""
import errno
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
from animation_trace import check_frames
from test_cli import check_trace

ROOT = Path(__file__).resolve().parent.parent
MQ = "--queues" in sys.argv
JUDGE, CAR = (str(ROOT / "race-mq-judge"), str(ROOT / "race-mq-car")) if MQ else (str(ROOT / "race-judge"), str(ROOT / "race-car"))
serial = 0
if MQ:
    mq = ctypes.CDLL("librt.so.1", use_errno=True)
    mq.mq_open.argtypes = [ctypes.c_char_p, ctypes.c_int]
    mq.mq_open.restype = ctypes.c_int
    mq.mq_close.argtypes = [ctypes.c_int]


def clean(prefix):
    assert not list(Path("/dev/shm").glob(prefix + "*")), prefix
    assert not list(Path("/dev/shm").glob("sem." + prefix + "*")), prefix
    if MQ:
        queue = prefix.replace("akos-session-", "akos-mq-")
        assert not list(Path("/dev/shm").glob("sem." + queue + "*")), prefix
        name = "/" + queue + ("" if queue.startswith("akos-client-") else "-events")
        fd = mq.mq_open(name.encode(), os.O_RDONLY | os.O_NONBLOCK)
        if fd >= 0: mq.mq_close(fd)
        assert fd == -1 and ctypes.get_errno() == errno.ENOENT, name


def command(binary, args=(), expected=0, env=None):
    p = subprocess.Popen([str(ROOT / binary), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    try:
        out, err = p.communicate(timeout=45 if MQ else 15)
    finally:
        if p.poll() is None:
            p.kill(); p.wait()
    assert p.returncode == expected, (binary, args, p.returncode, err)
    if expected == 0:
        assert not err, err
    clean(f"akos-race-{p.pid}")
    return out.decode()


class Session:
    def __init__(self, args=(), stdout=subprocess.PIPE, start=True, name=None):
        global serial
        serial += 1
        self.name = name or f"test-{os.getpid()}-{serial}"
        self.args = list(args)
        self.cars = []
        self.judge = None
        self.stdout = stdout
        if start:
            self.start()

    def start(self):
        self.judge = subprocess.Popen([JUDGE, "--session", self.name, *self.args], stdout=self.stdout,
                                      stderr=subprocess.PIPE, env=dict(os.environ, TERM="xterm"))
        return self

    def car(self, id_, seconds=3):
        car = subprocess.Popen([CAR, self.name, str(id_), str(seconds)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.cars.append(car)
        return car

    def finish(self, expected=0, car_codes=None):
        out, err = self.judge.communicate(timeout=45 if MQ else 15)
        assert self.judge.returncode == expected, (expected, self.judge.returncode, err)
        if expected == 0:
            assert not err, err
        codes = []
        for car in self.cars:
            _, error = car.communicate(timeout=5)
            codes.append(car.returncode)
            if car.returncode == 0:
                assert not error, error
        if car_codes is not None:
            assert codes == car_codes, codes
        clean("akos-session-" + self.name)
        if MQ:
            for car in self.cars: clean(f"akos-client-{car.pid}")
        return out.decode() if out is not None else ""

    def close(self):
        for p in [self.judge, *self.cars]:
            if p is not None and p.poll() is None:
                p.kill()
        for p in [self.judge, *self.cars]:
            if p is not None:
                p.wait()

    def __enter__(self): return self
    def __exit__(self, *_): self.close()


def parked(car):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        assert car.poll() is None, car.communicate()
        if "futex" in Path(f"/proc/{car.pid}/wchan").read_text():
            return
        time.sleep(0.005)
    raise AssertionError("Машина не дождалась регистрации")


def scenario(t, c, lanes, length, laps, rounds, strategy, seed):
    return ["--teams", str(t), "--cars", str(c), "--lanes", str(lanes), "--length", str(length),
            "--laps", str(laps), "--rounds", str(rounds), "--strategy", strategy, "--seed", str(seed)]


with tempfile.TemporaryDirectory(prefix="akos-variants-") as folder:
    folder = Path(folder)
    cases = [(3, 2, 4, 24, 2, 100, s, seed) for s in ("greedy", "random") for seed in (0, 1, 7)]
    cases += [(1, 1, 1, 3, 1, 10, "random", 3), (8, 4, 2, 33, 2, 100, "random", 0),
              (8, 4, 6, 120, 100, 10000, "greedy", 4294967295),
              (8, 4, 6, 120, 100, 10000, "random", 4294967295)]
    for n, (t, c, lanes, length, laps, rounds, strategy, seed) in enumerate(cases):
        args = scenario(t, c, lanes, length, laps, rounds, strategy, seed)
        # Для максимальных гонок сравниваем полные журналы на небольшой дистанции,
        # а конечность на 100 кругах проверяем по кратким итогам.
        quiet = laps == 100
        extra = ["--quiet"] if quiet else []
        expected = command("race", args + extra)
        assert command("race-named", args + extra) == expected
        log = folder / f"{n}.log"
        with Session(args + extra + ([] if quiet else ["--log", str(log)])) as s:
            for id_ in reversed(range(1, t * c + 1)): s.car(id_)
            output = s.finish(car_codes=[0] * (t * c))
            assert output == expected, n
            if not quiet:
                assert log.read_text() == expected
                check_trace(output, t, c, lanes, length, laps)

    # Машина может ожидать ещё не запущенного судью.
    with Session(["--teams", "1", "--cars", "1", "--length", "3", "--laps", "1"], start=False) as s:
        s.car(1)
        time.sleep(0.05)
        s.start()
        s.finish(car_codes=[0])

    # Повторный ID, отсутствующий ID и второй судья не меняют первую сессию.
    args = ["--teams", "1", "--cars", "2", "--length", "3", "--laps", "1"]
    with Session(args) as s:
        parked(s.car(1))
        duplicate = s.car(1); assert duplicate.communicate(timeout=3)[1] and duplicate.returncode == 1
        wrong = s.car(3); assert wrong.communicate(timeout=3)[1] and wrong.returncode == 1
        command("race-mq-judge" if MQ else "race-judge", ["--session", s.name], expected=1)
        assert not Path(f"/proc/{s.judge.pid}/task/{s.judge.pid}/children").read_text().strip()
        s.car(2)
        s.finish(car_codes=[0, 1, 1, 0])

    with Session([*args, "--wait", "1"]) as s:
        s.car(1)
        s.finish(expected=1)
    for sig in (signal.SIGINT, signal.SIGTERM):
        with Session(args) as s:
            parked(s.car(1))
            s.judge.send_signal(sig)
            s.finish(expected=128 + sig, car_codes=[0])

    quiet_log = folder / "quiet.log"
    with Session(["--quiet", "--log", str(quiet_log)]) as s:
        for id_ in range(1, 7): s.car(id_)
        assert s.finish(car_codes=[0] * 6) == command("race", ["--quiet"])
        assert quiet_log.read_text() == command("race")
    if Path("/dev/full").exists():
        with Session(["--quiet", "--log", "/dev/full"]) as s:
            for id_ in range(1, 7): s.car(id_)
            s.finish(expected=1, car_codes=[0] * 6)

    # Две независимые гонки выполняются одновременно с разными данными.
    a = scenario(3, 2, 4, 24, 2, 100, "random", 7)
    b = scenario(8, 4, 2, 33, 2, 100, "greedy", 3)
    with Session(a) as first, Session(b) as second:
        for id_ in range(1, 33):
            second.car(id_)
            if id_ <= 6: first.car(id_)
        assert first.finish(car_codes=[0] * 6) == command("race", a)
        assert second.finish(car_codes=[0] * 32) == command("race", b)

    others = [subprocess.Popen([str(ROOT / binary), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
              for binary, args in (("race", a), ("race-named", b))]
    try:
        with Session(a) as s:
            for id_ in range(1, 7): s.car(id_)
            assert s.finish(car_codes=[0] * 6) == command("race", a)
        for p, args in zip(others, (a, b)):
            out, err = p.communicate(timeout=45 if MQ else 15)
            assert p.returncode == 0 and not err
            assert out.decode() == command("race", args)
            clean(f"akos-race-{p.pid}")
    finally:
        for p in others:
            if p.poll() is None: p.kill()
            p.wait()

    # Гибель участника или судьи не оставляет открытых имён сессии.
    for target in ("car", "judge"):
        with Session(args) as s:
            car = s.car(1); parked(car)
            (car if target == "car" else s.judge).kill()
            if target == "car": s.finish(expected=1)
            else:
                s.judge.communicate(timeout=3)
                car.communicate(timeout=3)
                assert car.returncode == 1
                clean("akos-session-" + s.name)

    # Отказ после регистрации всех участников: pidfd замечает смерть,
    # а остановленная машина обнаруживается по отсутствию прогресса протокола.
    long_race = scenario(8, 4, 6, 120, 100, 10000, "random", 1)
    for sig in (signal.SIGKILL, signal.SIGSTOP):
        with Session(long_race) as s:
            for id_ in range(1, 33): s.car(id_)
            deadline = time.monotonic() + 4
            if MQ:
                for car in s.cars: parked(car)
            while not MQ and list(Path("/dev/shm").glob("sem.akos-session-" + s.name + "*")):
                assert time.monotonic() < deadline
                time.sleep(0.005)
            os.kill(s.cars[0].pid, sig)
            s.finish(expected=1)

    # Реальные кадры независимой версии и сигнал во время пятисекундной паузы.
    for sig in (None, signal.SIGINT, signal.SIGTERM):
        master, slave = os.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        log = folder / f"animated-{sig}.log"
        args = ["--teams", "1", "--cars", "1", "--lanes", "1", "--length", "3", "--laps", "1",
                "--delay", "1" if sig is None else "5000", "--log", str(log)]
        try:
            with Session(args, stdout=slave) as s:
                os.close(slave); slave = -1
                car = s.car(1)
                data = bytearray(); sent = None; deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    if select.select([master], [], [], 0.05)[0]:
                        try: block = os.read(master, 65536)
                        except OSError as error:
                            if error.errno == errno.EIO: break
                            raise
                        if not block: break
                        data.extend(block)
                    if sig is not None and sent is None and b"\x1b[J" in data:
                        car.send_signal(sig); sent = time.monotonic()
                s.finish(expected=0 if sig is None else 128 + sig,
                         car_codes=[0 if sig is None else 128 + sig])
                text = data.decode().replace("\r\n", "\n")
                assert "\x1b[?25h\x1b[?1049l" in text
                if sig is None: check_frames(text, log.read_text(), 1, 1, 1, 3, 1)
                else: assert sent is not None and time.monotonic() - sent < 2.5
        finally:
            os.close(master)
            if slave >= 0: os.close(slave)

    master, slave = os.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    log = folder / "multi-frame.log"
    try:
        with Session([*a, "--laps", "1", "--delay", "1", "--log", str(log)], stdout=slave) as s:
            os.close(slave); slave = -1
            for id_ in range(1, 7): s.car(id_)
            data = bytearray(); deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if not select.select([master], [], [], 0.05)[0]: continue
                try: block = os.read(master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO: break
                    raise
                if not block: break
                data.extend(block)
            s.finish(car_codes=[0] * 6)
            check_frames(data.decode().replace("\r\n", "\n"), log.read_text(), 3, 2, 4, 24, 1)
    finally:
        os.close(master)
        if slave >= 0: os.close(slave)

    library = folder / "faults.so"
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
                    str(ROOT / "tests/faults.c"), "-ldl", "-o", str(library)], check=True)
    small = ["--teams", "8", "--cars", "4", "--length", "33", "--rounds", "1", "--quiet"]
    failures = []
    if not MQ:
        failures = ["shm:1", "truncate:1", "map:1", "unlink:1"]
        failures += [f"{op}:{n}" for op in ("open", "close", "sem_unlink") for n in range(1, 35)]
        failures += [f"fork:{n}" for n in range(1, 33)]
        for failure in failures:
            command("race-named", small, expected=1, env=dict(os.environ, LD_PRELOAD=str(library), AKOS_FAIL=failure))
    for n in range(1, 4 if MQ else 35):
        command("race-mq-judge" if MQ else "race-judge", ["--session", f"failure-{os.getpid()}", *small], expected=1,
                env=dict(os.environ, LD_PRELOAD=str(library), AKOS_FAIL=f"open:{n}"))
        clean(f"akos-session-failure-{os.getpid()}")
    if MQ:
        command("race-mq-judge", ["--session", f"failure-{os.getpid()}", *small], expected=1,
                env=dict(os.environ, LD_PRELOAD=str(library), AKOS_FAIL="mq:1"))
        clean(f"akos-session-failure-{os.getpid()}")
        for failure in ("mq:1", "mq:2", "open:1"):
            car = subprocess.Popen([CAR, "missing", "1", "1"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   env=dict(os.environ, LD_PRELOAD=str(library), AKOS_FAIL=failure))
            car.communicate(timeout=3)
            assert car.returncode == 1
            clean(f"akos-client-{car.pid}")
        for sig in (None, signal.SIGINT, signal.SIGTERM):
            car = subprocess.Popen([CAR, "missing", "1", "1"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if sig is not None:
                time.sleep(0.05); car.send_signal(sig)
            car.communicate(timeout=3)
            assert car.returncode == (1 if sig is None else 128 + sig)
            clean(f"akos-client-{car.pid}")
        # Обмен модели очередями не должен зависеть от shm_open.
        with Session(["--teams", "1", "--cars", "1", "--length", "3", "--laps", "1"], start=False) as s:
            s.judge = subprocess.Popen([JUDGE, "--session", s.name, *s.args], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      env=dict(os.environ, LD_PRELOAD=str(library), AKOS_FAIL="shm:1"))
            s.car(1); s.finish(car_codes=[0])

print(f"{'Очереди' if MQ else 'Варианты'}: {len(cases)} конфигураций совпали с базовой реализацией, включая максимальные параметры.")
if MQ:
    print("Проверены 7 отказов открытия очередей/семафоров судьи и машины; обмен работает при отказе shm_open.")
else:
    print(f"Проверены {len(failures)} отказов именованного варианта и 34 отказа создания семафоров судьи.")
print("Независимый запуск, регистрация, параллельные сессии, сигналы, кадры и очистка ресурсов проверены.")
