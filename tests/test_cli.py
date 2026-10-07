"""Проверки запуска и независимое восстановление гонки по её журналу."""
import pathlib
import re
import signal
import subprocess
import sys
import tempfile

BINARY = str(pathlib.Path(sys.argv[1]).resolve())


def run(args=(), expected=0):
    result = subprocess.run([BINARY, *args], capture_output=True, text=True, timeout=10)
    assert result.returncode == expected, (args, result.returncode, result.stderr)
    return result.stdout


def check_trace(text, teams, cars, lanes, length, laps):
    states = {}
    occupied = {}
    order = []
    turns = 0
    rounds = 0
    previous = 0
    current = None
    limit = 0
    finished = []
    table = False
    table_rows = 0

    def end_round():
        assert turns == len(order)
        for car in states.values():
            if car.get("zero"):
                car["status"] = "сход"
            if car["status"] in ("сход", "финиш"):
                cell = (car["lane"], car["position"])
                if occupied.get(cell) == car["id"]:
                    del occupied[cell]

    for line in text.splitlines():
        if line == "Машина Команда Дорожка Клетка Продвижение Круги Состояние":
            if rounds:
                end_round()
            table = True
            table_rows = 0
            continue
        if table:
            row = re.fullmatch(r"\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(-?\d+)\s+(\d+)\s+(.*)", line)
            if row:
                id_, team, lane, position, distance, completed = map(int, row.groups()[:6])
                status = row[7]
                lane -= 1
                position -= 1
                assert completed == max(0, distance) // length
                assert position == distance % length
                if not rounds:
                    assert id_ not in states and (lane, position) not in occupied
                    assert team == (id_ - 1) // cars + 1
                    assert lane == lanes - 1 and distance == -table_rows - 1
                    states[id_] = dict(id=id_, team=team, lane=lane, position=position,
                                       distance=distance, status=status)
                    occupied[lane, position] = id_
                else:
                    car = states[id_]
                    assert (team, lane, position, distance, status) == (
                        car["team"], car["lane"], car["position"], car["distance"], car["status"])
                table_rows += 1
                continue
            assert table_rows == teams * cars
            table = False

        match = re.fullmatch(r"Раунд (\d+)\. Порядок:(.*)", line)
        if match:
            rounds += 1
            assert int(match[1]) == rounds
            order = list(map(int, match[2].split()))
            active = [c for c in states.values() if c["status"] == "участвует"]
            expected = [c["id"] for c in sorted(active, key=lambda c: (-c["distance"], c["lane"], c["id"]))]
            assert order == expected
            turns = 0
            previous = 0
            continue

        match = re.fullmatch(r"Машина (\d+) \(команда (\d+)\): предел (\d+)\.", line)
        if match:
            id_, team, limit = map(int, match.groups())
            assert id_ == order[turns] and team == states[id_]["team"]
            assert limit == (4 if turns == 0 else previous + (2 if turns == 1 else 1))
            current = states[id_]
            assert current["status"] == "участвует"
            continue

        if line.startswith("  Нулевой ход:"):
            c = current
            candidates = [(c["lane"], (c["position"] + 1) % length, 1),
                          (c["lane"] - 1, c["position"], 1),
                          (c["lane"] + 1, c["position"], 3)]
            assert not any(0 <= lane < lanes and cost <= limit and (lane, p) not in occupied
                           for lane, p, cost in candidates)
            c["zero"] = True
            previous = 0
            turns += 1
            continue

        match = re.fullmatch(r"  Маршрут:(.*)\. Стоимость: (\d+)\. Дорожка: (\d+)\. Клетка: (\d+)\.", line)
        if match:
            c = current
            tokens = re.findall(r"вперёд|внутрь\(\d+->\d+\)|наружу\(\d+->\d+\)", match[1])
            assert tokens
            visited = {(c["lane"], c["position"])}
            cost = 0
            crossed_laps = []
            for index, token in enumerate(tokens):
                old_cell = c["lane"], c["position"]
                if token == "вперёд":
                    c["position"] = (c["position"] + 1) % length
                    c["distance"] += 1
                    cost += 1
                    if c["distance"] > 0 and c["distance"] % length == 0:
                        crossed_laps.append(c["distance"] // length)
                else:
                    old_lane = c["lane"]
                    if token.startswith("внутрь"):
                        c["lane"] -= 1
                        cost += 1
                    else:
                        c["lane"] += 1
                        cost += 3
                    assert list(map(int, re.findall(r"\d+", token))) == [old_lane + 1, c["lane"] + 1]
                cell = c["lane"], c["position"]
                assert 0 <= c["lane"] < lanes
                assert cell not in occupied and cell not in visited
                assert occupied[old_cell] == c["id"]
                del occupied[old_cell]
                occupied[cell] = c["id"]
                visited.add(cell)
                assert cost <= limit and c["distance"] <= laps * length
                if c["distance"] == laps * length:
                    assert index == len(tokens) - 1 and c["position"] == 0
                    c["status"] = "финиш"
                    finished.append(c["id"])
                    assert f"[финиш, место {len(finished)}]" in match[1]
            assert crossed_laps == list(map(int, re.findall(r"\[круг (\d+) завершён\]", match[1])))
            assert cost == int(match[2])
            assert (c["lane"] + 1, c["position"] + 1) == (int(match[3]), int(match[4]))
            previous = cost
            turns += 1

    assert len(states) == teams * cars
    first_teams = [c["team"] for c in list(states.values())[:teams]]
    assert len(set(first_teams)) == teams
    assert [c["team"] for c in states.values()] == first_teams * cars
    result_text = text.split("Результаты машин:\n", 1)[1].split("Результаты команд", 1)[0]
    results = re.findall(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+(-?\d+)\s+(финиш|сход|не завершила)$", result_text, re.M)
    assert len(results) == len(states)
    expected_ids = finished + [c["id"] for c in sorted(
        (c for c in states.values() if c["status"] != "финиш"),
        key=lambda c: (-c["distance"], c["lane"], c["id"]))]
    assert [int(row[1]) for row in results] == expected_ids
    assert [int(row[0]) for row in results] == list(range(1, teams * cars + 1))
    for _, id_, team, distance, status in results:
        c = states[int(id_)]
        assert int(team) == c["team"] and int(distance) == c["distance"]
        assert status == ("не завершила" if c["status"] == "участвует" else c["status"])
    team_text = text.split("Результаты команд (по лучшей машине):\n", 1)[1]
    team_rows = re.findall(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)$", team_text, re.M)
    expected_teams = []
    for rank, id_, team, _, _ in results:
        if int(team) not in [t[0] for t in expected_teams]:
            expected_teams.append((int(team), int(id_), int(rank)))
    assert [(int(t), int(c), int(r)) for _, t, c, r in team_rows] == expected_teams
    assert [int(row[0]) for row in team_rows] == list(range(1, teams + 1))
    assert f"Раундов: {rounds}." in text


normal = run()
assert normal == run(["--no-animation"]) == run(["--delay", "5000"])
assert "\x1b" not in normal, "При перенаправлении вывода анимация выключается"
assert normal == run(), "Одинаковый seed должен давать одинаковый журнал"
check_trace(normal, 3, 2, 4, 24, 2)
assert "не завершила" in run(["--rounds", "1", "--quiet"])
assert "Использование" in run(["--help"])

for strategy in ("greedy", "random"):
    for seed in range(10):
        teams, cars, lanes, length, laps = 1 + seed % 8, 1 + seed % 4, 1 + seed % 6, 33 + seed, 1 + seed % 3
        args = ["--teams", str(teams), "--cars", str(cars), "--lanes", str(lanes),
                "--length", str(length), "--laps", str(laps), "--strategy", strategy,
                "--seed", str(seed), "--rounds", "200"]
        check_trace(run(args), teams, cars, lanes, length, laps)
check_trace(run(["--teams", "8", "--cars", "4", "--length", "33", "--lanes", "6", "--laps", "1"]), 8, 4, 6, 33, 1)
check_trace(run(["--teams", "1", "--cars", "1", "--length", "3", "--lanes", "1", "--laps", "1"]), 1, 1, 1, 3, 1)
check_trace(run(["--lanes", "1", "--length", "7", "--laps", "1"]), 3, 2, 1, 7, 1)
check_trace(run(["--rounds", "1"]), 3, 2, 4, 24, 2)
crowded = run(["--teams", "8", "--cars", "4", "--length", "33", "--lanes", "2",
               "--laps", "2", "--strategy", "random", "--seed", "0"])
assert "Нулевой ход" in crowded and " сход\n" in crowded
check_trace(crowded, 8, 4, 2, 33, 2)
run(["--seed", "4294967295", "--quiet"])

invalid = [["--teams", "0"], ["--teams", "9"], ["--cars", "5"], ["--length", "121"],
           ["--lanes", "0"], ["--laps", "101"], ["--rounds", "10001"], ["--length", "6"],
           ["--seed", "-1"], ["--seed", "4294967296"], ["--seed", "99999999999999999999999"],
           ["--teams", "2abc"], ["--teams", ""], ["--teams", " 2"], ["--teams", "+2"],
           ["--rounds", "0"], ["--strategy", "unknown"], ["--teams"], ["--unknown"],
           ["--log", "--quiet"], ["--delay", "-1"], ["--delay", "5001"], ["--delay"]]
for args in invalid:
    run(args, expected=1)

with tempfile.TemporaryDirectory(prefix="akos-tests-") as folder:
    log = pathlib.Path(folder) / "race.log"
    run(["--quiet", "--log", str(log)])
    assert log.read_text() == normal, "Журнал должен сохранять все события даже при --quiet"
    run(["--log", str(pathlib.Path(folder) / "missing" / "race.log")], expected=1)

if pathlib.Path("/dev/full").exists():
    run(["--log", "/dev/full", "--quiet"], expected=1)

for stop_signal in (signal.SIGINT, signal.SIGTERM):
    process = subprocess.Popen([BINARY, "--teams", "8", "--cars", "4", "--length", "120",
                                "--laps", "100", "--rounds", "10000", "--strategy", "random"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    first_line = process.stdout.readline()  # Обработчик уже установлен; большой вывод удерживает процесс живым.
    assert first_line.decode().startswith("Автогонки:")
    process.send_signal(stop_signal)
    stdout, stderr = process.communicate(timeout=10)
    stdout = stdout.decode()
    stderr = stderr.decode()
    assert process.returncode == 128 + stop_signal, (process.returncode, stderr)
    assert "прерывание пользователем" in stdout
    assert "Результаты команд" in stdout
    check_trace(first_line.decode() + stdout, 8, 4, 4, 120, 100)

print("CLI: 28 журналов, параметры, повторяемость, лог и прерывания проверены.")
