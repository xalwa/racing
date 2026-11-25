"""Сетевые проверки запускают штатных клиентов и управляемых участников."""
from pathlib import Path
import errno
import os
import pty
import re
import select
import signal
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
HEADER = struct.Struct("!2sBBI")
HELLO, WELCOME, TURN, MOVE, RESULT, ERROR, WATCH, VIEW, STANDINGS, STOP = range(1, 11)


def until(condition, seconds=5):
    stop = time.monotonic() + seconds
    while time.monotonic() < stop:
        if condition():
            return
        time.sleep(0.005)
    raise AssertionError("Условие не выполнено за отведённое время")


def words(*values):
    return struct.pack(f"!{len(values)}I", *values)


def frame(kind, body=b""):
    return HEADER.pack(b"RC", 1, kind, len(body)) + body


def exact(peer, count):
    result = bytearray()
    while len(result) < count:
        part = peer.recv(count - len(result))
        assert part, "Получен EOF вместо сообщения"
        result.extend(part)
    return bytes(result)


def read(peer):
    magic, version, kind, size = HEADER.unpack(exact(peer, HEADER.size))
    assert magic == b"RC" and version == 1 and size <= 2048
    return kind, exact(peer, size)


class Process:
    def __init__(self, folder, name, args, stdout=None, env=None):
        self.path = folder / f"{len(list(folder.iterdir()))}.out"
        self.error = self.path.with_suffix(".err")
        with self.path.open("w") as out, self.error.open("w") as err:
            self.proc = subprocess.Popen([str(ROOT / name), *args], stdout=out if stdout is None else stdout,
                                         stderr=err, env=env)

    def out(self):
        return self.path.read_text(errors="replace")

    def err(self):
        return self.error.read_text(errors="replace")

    def wait(self, expected=0, seconds=12):
        assert self.proc.wait(timeout=seconds) == expected, (self.out()[-2000:], self.err())


class Match:
    def __init__(self, *options, address="127.0.0.1"):
        self.address = address
        family = socket.AF_INET6 if ":" in address else socket.AF_INET
        with socket.socket(family) as probe:
            probe.bind((address, 0))
            self.port = str(probe.getsockname()[1])
        self.options = list(options)
        self.children, self.sockets = [], []

    def __enter__(self):
        self.temp = tempfile.TemporaryDirectory()
        try:
            self.judge = self.run("race-server", "--delay", "0", *self.options)
            until(lambda: "Ожидается машин:" in self.judge.out() or self.judge.proc.poll() is not None)
            assert self.judge.proc.poll() is None, self.judge.err()
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *unused):
        for sock in self.sockets:
            sock.close()
        for child in self.children:
            if child.proc.poll() is None:
                child.proc.kill()
            child.proc.wait(timeout=5)
        self.temp.cleanup()

    def run(self, name, *options, **output):
        child = Process(Path(self.temp.name), name, [self.address, self.port, *options], **output)
        self.children.append(child)
        return child

    def connect(self, buffer=None):
        family = socket.AF_INET6 if ":" in self.address else socket.AF_INET
        peer = socket.socket(family)
        self.sockets.append(peer)
        if buffer is not None:
            peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, buffer)
        peer.settimeout(5)
        peer.connect((self.address, int(self.port)))
        return peer

    def register(self, car=1, split=False):
        peer = self.connect()
        hello = frame(HELLO, words(car))
        if split:
            for part in (hello[:3], hello[3:9], hello[9:]):
                peer.sendall(part)
                time.sleep(0.01)
        else:
            peer.sendall(hello)
        assert read(peer)[0] == WELCOME
        return peer

    def view(self):
        peer = self.connect()
        peer.sendall(frame(WATCH))
        kind, data = read(peer)
        assert kind == VIEW
        return peer, data


def same_race(options, cars, address="127.0.0.1"):
    reference = subprocess.check_output([ROOT / "race", "--no-animation", *options], text=True)
    turns = re.findall(r"Машина (\d+) \(команда \d+\): предел (\d+)\.", reference)
    table, teams = reference.split("Результаты машин:")[1].split("Результаты команд")
    team_places = {team: place for place, team, _, _ in re.findall(r"^ *(\d+) +(\d+) +(\d+) +(\d+)$", teams, re.M)}
    ranks = re.findall(r"^ *(\d+) +(\d+) +(\d+) +(-?\d+) +(финиш|сход|не завершила)$", table, re.M)
    rounds = re.search(r"Раундов: (\d+)\.", reference)[1]
    with Match(*options, address=address) as game:
        watcher = game.run("race-watch", "--no-animation")
        until(lambda: "Наблюдатель подключился." in game.judge.out())
        drivers = {str(i): game.run("race-driver", str(i)) for i in reversed(range(1, cars + 1))}
        game.judge.wait()
        watcher.wait()
        for driver in drivers.values():
            driver.wait()
        for place, id_, team, distance, status in ranks:
            expected = f"Итог машины {id_}: {status}, место {place}, место команды {team_places[team]}, продвижение {distance}. Раундов: {rounds}."
            assert expected in drivers[id_].out(), expected
        got_turns = re.findall(r"Ход \d+ назначен машине (\d+)\. Раунд: \d+, бюджет: (\d+)\.", game.judge.out())
        assert got_turns == turns
        assert re.search(r"Результаты машин:.*", watcher.out(), re.S)[0] == re.search(r"Результаты машин:.*", reference, re.S)[0]
        assert "Результаты машин:" not in game.judge.out()
        print(f"TCP {address}: {cars} машин, {rounds} раундов, {len(turns)} ходов совпали с v3.0.")


same_race([], 6)
same_race(["--strategy", "random", "--seed", "7"], 6)
try:
    same_race(["--teams", "1", "--cars", "2", "--length", "3", "--laps", "1"], 2, "::1")
except OSError as err:
    if err.errno not in (errno.EAFNOSUPPORT, errno.EADDRNOTAVAIL):
        raise
    print("TCP ::1: IPv6 недоступен в этом окружении.")
same_race(["--teams", "8", "--cars", "4", "--lanes", "6", "--length", "120", "--laps", "100", "--rounds", "1"], 32)

with Match("--teams", "1", "--cars", "2") as game:
    game.register(split=True)
    for id_, code in ((1, 3), (3, 2)):
        peer = game.connect()
        peer.sendall(frame(HELLO, words(id_)))
        assert read(peer) == (ERROR, words(code))
    game.judge.proc.send_signal(signal.SIGTERM)
    game.judge.wait(143)

with Match("--teams", "1", "--cars", "1", "--rounds", "1", "--delay", "10") as game:
    first, _ = game.view()
    car = game.register()
    assert read(car)[0] == TURN
    while True:
        kind, data = read(first)
        assert kind == VIEW
        if struct.unpack_from("!I", data)[0] == 1:
            break
    first.close()
    second, snapshot = game.view()
    assert struct.unpack_from("!I", snapshot)[0] == 1
    tty, slave = pty.openpty()
    try:
        watcher = game.run("race-watch", stdout=slave, env={**os.environ, "TERM": "xterm"})
        os.close(slave)
        slave = -1
        until(lambda: game.judge.out().count("Наблюдатель подключился.") == 3)
        car.sendall(frame(MOVE, words(1, 4) + bytes(4)))
        frames = []
        while True:
            kind, data = read(second)
            if kind == STANDINGS:
                break
            assert kind == VIEW
            fields = struct.unpack_from("!17I", data)
            if fields[8] == 1 and fields[9] > 0:
                cell = struct.unpack_from("!I", data, 17 * 4 + 2 * 4)[0]
                frames.append((fields[9], cell))
        assert frames == [(1, 0), (2, 1), (3, 2), (4, 3)], frames
        assert read(car)[0] == RESULT
        rendered = bytearray()
        end = time.monotonic() + 5
        while time.monotonic() < end:
            if not select.select([tty], [], [], 0.1)[0]:
                continue
            try:
                rendered.extend(os.read(tty, 16384))
            except OSError as err:
                if err.errno != errno.EIO:
                    raise
                break
        watcher.wait()
        output = rendered.decode()
        assert "\x1b[?1049h" in output and "\x1b[?1049l" in output
        assert all(f"Шаг {n}/4" in output for n in range(1, 5))
    finally:
        os.close(tty)
        if slave >= 0:
            os.close(slave)
    game.judge.wait()

with Match("--teams", "1", "--cars", "1", "--lanes", "1", "--rounds", "2",
           "--strategy", "random", "--reconnect", "4") as game:
    car = game.register()
    assert read(car)[0] == TURN
    move = frame(MOVE, words(1, 4) + bytes(4))
    car.sendall(move + move)
    kind, pending = read(car)
    assert kind == TURN and struct.unpack_from("!I", pending)[0] == 2
    car.close()
    until(lambda: "Ход 2 приостановлен" in game.judge.out())
    replacement = game.connect()
    replacement.sendall(frame(HELLO, words(1)))
    assert read(replacement) == (WELCOME, words(1, 1, 1, 4))
    assert read(replacement) == (TURN, pending)
    replacement.close()
    until(lambda: game.judge.out().count("Ход 2 приостановлен") == 2)
    driver = game.run("race-driver", "1")
    driver.wait()
    assert "продвижение 7. Раундов: 2." in driver.out()
    assert game.judge.out().count("Ход 1 получен") == 1
    assert game.judge.out().count("Ход 2 получен") == 1
    game.judge.wait()

with Match("--teams", "1", "--cars", "1", "--reconnect", "1") as game:
    car = game.register()
    assert read(car)[0] == TURN
    car.close()
    game.judge.wait(1)
    assert "ожидания переподключения" in game.judge.err()

with Match("--teams", "1", "--cars", "1", "--lanes", "1") as game:
    car = game.register()
    assert read(car)[0] == TURN
    car.sendall(frame(MOVE, words(1, 5) + bytes(5)))
    assert read(car) == (ERROR, words(6))
    game.judge.wait(1)

with Match("--teams", "1", "--cars", "2", "--turn-timeout", "1") as game:
    first = game.register()
    second = game.register(2)
    assert read(first)[0] == TURN
    assert read(second) == (STOP, words(4))
    game.judge.wait(1)
    assert "ожидания ответа на ход" in game.judge.err()

for sig in (signal.SIGINT, signal.SIGTERM):
    with Match("--teams", "1", "--cars", "2", "--wait", "30") as game:
        watcher = game.run("race-watch", "--no-animation")
        driver = game.run("race-driver", "2")
        until(lambda: "Наблюдатель подключился." in game.judge.out() and "Машина 2 подключена." in driver.out())
        current = None
        if sig == signal.SIGINT:
            current = game.register()
            assert read(current)[0] == TURN
        game.judge.proc.send_signal(sig)
        game.judge.wait(128 + sig)
        if current:
            assert read(current) == (STOP, words(3))
        for child in (watcher, driver):
            child.wait()
            assert "Судья остановил гонку." in child.out()

with Match("--teams", "8", "--cars", "4", "--lanes", "6", "--length", "120", "--laps", "100",
           "--rounds", "40") as game:
    stalled = game.connect(buffer=1024)
    stalled.sendall(frame(WATCH))
    until(lambda: "Наблюдатель подключился." in game.judge.out())
    drivers = [game.run("race-driver", str(i)) for i in range(1, 33)]
    game.judge.wait()
    for driver in drivers:
        driver.wait()
    assert "Кадры наблюдателя отстали" in game.judge.err()

print("TCP: поклеточные кадры, несколько наблюдателей, возврат машины, повтор кадра MOVE и остановка проверены.")
