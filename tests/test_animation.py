"""Запускаем настоящий терминальный вывод и проверяем отдельные кадры."""
import errno
import fcntl
import os
import pathlib
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

BINARY = str(pathlib.Path(sys.argv[1]).resolve())
BASE = ["--teams", "1", "--cars", "1", "--lanes", "1", "--length", "3", "--laps", "1"]
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


def terminal(args, stop_signal=None, term="xterm"):
    master, slave = os.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    environment = dict(os.environ, TERM=term)
    process = subprocess.Popen([BINARY, *args], stdin=subprocess.DEVNULL,
                               stdout=slave, stderr=subprocess.PIPE, env=environment)
    os.close(slave)
    data = bytearray()
    started = time.monotonic()
    stopped = None
    try:
        while time.monotonic() - started < 5:
            ready, _, _ = select.select([master], [], [], 0.05)
            if ready:
                try:
                    block = os.read(master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        break
                    raise
                if not block:
                    break
                data.extend(block)
                if stop_signal is not None and stopped is None and b"\x1b[J" in data:
                    process.send_signal(stop_signal)
                    stopped = time.monotonic()
            elif process.poll() is not None:
                break
        code = process.wait(timeout=1)
        stderr = process.stderr.read().decode()
        assert not stderr, stderr
        if stop_signal is not None:
            assert stopped is not None
            assert time.monotonic() - stopped < 2.5, "Сигнал должен отменять длинную задержку"
        return data.decode().replace("\r\n", "\n"), code, time.monotonic() - started
    finally:
        os.close(master)
        process.stderr.close()
        if process.poll() is None:
            process.kill()
            process.wait()


def frames(text):
    return [ANSI.sub("", chunk).split("\n\x1b", 1)[0]
            for chunk in text.split("\x1b[H")[1:]]


text, code, duration = terminal(BASE)
assert code == 0
assert duration >= 0.5, "В терминале по умолчанию должна быть видимая пауза"
assert text.count("\x1b[?1049h") == text.count("\x1b[?1049l") == 1
assert text.count("\x1b[?25l") == text.count("\x1b[?25h") == 1
assert "\x1b[7;31m" in text and "+--+--+--+" in text
positions = []
sample_frame = None
for frame in frames(text):
    match = re.search(r"Машина 1, команда 1 \| Дорожка 1, клетка (\d+)", frame)
    step = re.search(r"Шаг (\d+)/", frame)
    assert "Порядок:" in frame and "Маршрут (В=вперёд, И=внутрь, Н=наружу):" in frame
    if match and step and int(step[1]) > 0:
        position = int(match[1])
        positions.append(position)
        row = re.search(r"^Д1 (.*)$", frame, re.M)[1]
        cells = [cell.strip() for cell in row.split("|")[1:-1]]
        assert cells == ["1" if p == position else "." for p in range(1, 4)]
        sample_frame = frame
assert positions == [1, 2, 3, 1], positions
assert "Результаты команд" in text.split("\x1b[?1049l")[1]

with tempfile.TemporaryDirectory(prefix="akos-animation-") as folder:
    log = pathlib.Path(folder) / "animation.log"
    text, code, _ = terminal(BASE + ["--delay", "1", "--log", str(log)])
    plain = subprocess.run([BINARY, *BASE], capture_output=True, text=True, check=True).stdout
    assert code == 0 and log.read_text() == plain
    assert "\x1b" not in log.read_text()

for option in (["--no-animation"], ["--delay", "0"], ["--quiet"]):
    text, code, _ = terminal(BASE + option)
    assert code == 0 and "\x1b" not in text
text, code, _ = terminal(BASE, term="dumb")
assert code == 0 and "\x1b" not in text

for stop_signal in (signal.SIGINT, signal.SIGTERM):
    text, code, _ = terminal(BASE + ["--delay", "5000"], stop_signal=stop_signal)
    assert code == 128 + stop_signal
    assert "\x1b[?25h\x1b[?1049l" in text
    assert "прерывание пользователем" in text and "Результаты команд" in text

text, code, _ = terminal(["--teams", "1", "--cars", "1", "--length", "120", "--lanes", "6",
                          "--rounds", "1", "--delay", "1"])
assert code == 0
for frame in frames(text):
    screen = frame.split("Гонка завершена", 1)[0]
    assert all(len(line) <= 80 for line in screen.splitlines()), screen
assert "Клетки 102–120 из 120" in text and "Клетки 1–19 из 120" in text

text, code, _ = terminal(["--teams", "8", "--cars", "4", "--length", "33", "--lanes", "2",
                          "--laps", "2", "--strategy", "random", "--seed", "0", "--delay", "1"])
assert code == 0
assert "Блокирование:" in text and "Нулевой ход." in text and "Финиш!" in text
retirements = 0
for frame in frames(text):
    screen = frame.split("Гонка завершена", 1)[0]
    assert all(len(line) <= 80 for line in screen.splitlines()), screen
    assert len(screen.splitlines()) <= 24, screen
    if "Сход: машина снята с дистанции" in frame:
        retirements += 1
        car = re.search(r"Машина (\d+),", frame)[1]
        cells = [cell.strip() for row in re.findall(r"^Д\d (.*)$", frame, re.M)
                 for cell in row.split("|")[1:-1]]
        assert car not in cells, "Сошедшая машина должна быть снята в кадре схода"
assert retirements == 22, retirements

print("Анимация: переходы, порядок, маршруты, блокировки, сход, журнал, экран и сигналы проверены.")
print("Пример кадра:\n" + sample_frame.split("Число в клетке", 1)[0])
