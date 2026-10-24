"""Сверка каждого терминального кадра с состоянием, восстановленным по журналу."""
import re

ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


def check_frames(text, log, teams, cars, lanes, length, laps, columns=80, rows=24):
    actual = iter(text.split("\x1b[H")[1:])
    state = {}
    occupied = {}
    order = []
    round_ = 0
    finished = 0
    checked = 0
    steps = 0
    initial_table = False
    current = None
    blocked = False
    limit = 0

    def frame(event, car=None, step=0, route=(), cost=0, budget=0):
        nonlocal checked, steps
        raw = next(actual, None)
        assert raw is not None, ("Пропущен кадр", round_, event, car)
        screen = ANSI.sub("", raw).split("Гонка завершена:", 1)[0]
        checked += 1
        assert len(screen.splitlines()) <= rows, screen
        assert all(len(line) <= columns for line in screen.splitlines()), screen
        headline = re.search(r"Раунд (\d+)/\d+ \| Участвуют: (\d+) \| Финиш: (\d+)", screen)
        assert headline, screen
        assert tuple(map(int, headline.groups())) == (
            round_, sum(c["status"] == "active" for c in state.values()), finished), screen
        expected_events = {
            "start": "Стартовая расстановка",
            "begin": "Начало раунда. Порядок ходов зафиксирован",
            "route": "Выбран маршрут",
            "blocked": "Блокирование: занятые клетки исключены",
            "zero": "Нулевой ход. Сход в конце раунда",
            "retire": "Сход: машина снята с дистанции",
            "end": "Раунд завершён. Показан порядок следующего раунда",
        }
        assert f"Событие: {expected_events.get(event, event)}\n" in screen, screen
        shown_order = screen.split("Порядок:", 1)[1].split("Маршрут", 1)[0]
        assert list(map(int, shown_order.split())) == order, screen
        shown_route = screen.split("Маршрут (В=вперёд, И=внутрь, Н=наружу): ", 1)[1].splitlines()[0]
        assert shown_route == ("".join(route) if route else "—"), screen
        if car is not None:
            c = state[car]
            position = re.search(r"Машина (\d+), команда (\d+) \| Дорожка (\d+), клетка (\d+) \| Круги (\d+)/(\d+)", screen)
            assert position, screen
            assert tuple(map(int, position.groups())) == (
                car, c["team"], c["lane"], c["cell"], max(0, c["distance"]) // length, laps), screen
            move = re.search(r"Шаг (\d+)/(\d+) \| Потрачено (\d+) из (\d+) единиц хода", screen)
            assert move and tuple(map(int, move.groups())) == (step, len(route), cost, budget), screen
        viewport = re.search(r"Клетки (\d+)–(\d+) из (\d+)", screen)
        assert viewport and int(viewport[3]) == length, screen
        first, last = int(viewport[1]), int(viewport[2])
        assert 1 <= first <= last <= length
        grid = re.findall(r"^Д(\d+) (.*)$", screen, re.M)
        assert [int(lane) for lane, _ in grid] == list(range(1, lanes + 1)), screen
        for lane, row in grid:
            cells = [token.strip() for token in row.split("|")[1:-1]]
            expected = [str(occupied.get((int(lane), cell), ".")) for cell in range(first, last + 1)]
            assert cells == expected, (event, lane, cells, expected)
        # Проверяем не только текст после снятия ANSI: все видимые машины
        # окрашены по командам, и выделена именно машина из заголовка.
        colors = (31, 32, 33, 34, 35, 36, 91, 92)
        for team in range(1, teams + 1):
            assert f"\x1b[1;{colors[team - 1]}m{team}\x1b[0m" in raw
        colored = 0
        for lane, row in re.findall(r"^Д(\d+) (.*)$", raw, re.M):
            for attribute, color, id_ in re.findall(r"\x1b\[([17]);(\d+)m\s*(\d+)\x1b\[0m", row):
                id_ = int(id_)
                assert int(color) == colors[state[id_]["team"] - 1]
                assert int(attribute) == (7 if id_ == car else 1)
                colored += 1
        assert colored == sum(first <= cell <= last for lane, cell in occupied)
        if step:
            steps += 1

    def active_order():
        return sorted((id_ for id_, c in state.items() if c["status"] == "active"),
                      key=lambda id_: (-state[id_]["distance"], state[id_]["lane"], id_))

    for line in log.splitlines():
        if line == "Машина Команда Дорожка Клетка Продвижение Круги Состояние":
            if round_ == 0:
                initial_table = True
            else:
                for id_, c in state.items():
                    cell = c["lane"], c["cell"]
                    if c["status"] != "active" and occupied.get(cell) == id_:
                        del occupied[cell]
                order = active_order()
                frame("end")
            continue
        if initial_table:
            match = re.fullmatch(r"\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(-?\d+)\s+(\d+)\s+участвует", line)
            if match:
                id_, team, lane, cell, distance, _ = map(int, match.groups())
                state[id_] = dict(team=team, lane=lane, cell=cell, distance=distance, status="active")
                occupied[lane, cell] = id_
                order.append(id_)
                continue
            initial_table = False
            assert len(state) == teams * cars
            frame("start")

        match = re.fullmatch(r"Раунд (\d+)\. Порядок:(.*)", line)
        if match:
            round_ = int(match[1])
            order = list(map(int, match[2].split()))
            frame("begin")
            continue
        match = re.fullmatch(r"Машина (\d+) \(команда \d+\): предел (\d+)\.", line)
        if match:
            current, limit = map(int, match.groups())
            blocked = False
            continue
        if line.startswith("  Блокирование:"):
            blocked = True
            continue
        if line.startswith("  Нулевой ход:"):
            if blocked:
                frame("blocked", current, budget=limit)
            frame("zero", current, budget=limit)
            continue
        match = re.fullmatch(r"  Маршрут:(.*)\. Стоимость: (\d+)\. Дорожка: (\d+)\. Клетка: (\d+)\.", line)
        if match:
            tokens = re.findall(r"вперёд|внутрь\(\d+->\d+\)|наружу\(\d+->\d+\)", match[1])
            route = ["В" if t == "вперёд" else "И" if t.startswith("внутрь") else "Н" for t in tokens]
            frame("blocked" if blocked else "route", current, route=route, budget=limit)
            c = state[current]
            cost = 0
            for index, direction in enumerate(route):
                del occupied[c["lane"], c["cell"]]
                if direction == "В":
                    c["cell"] = c["cell"] % length + 1
                    c["distance"] += 1
                    event = "вперёд"
                    if c["distance"] == 0:
                        event = "Пересечение стартовой линии"
                    elif c["distance"] > 0 and c["distance"] % length == 0:
                        event = "Круг завершён"
                else:
                    c["lane"] += -1 if direction == "И" else 1
                    event = "внутрь" if direction == "И" else "наружу"
                cost += 3 if direction == "Н" else 1
                occupied[c["lane"], c["cell"]] = current
                if c["distance"] == laps * length:
                    finished += 1
                    c["status"] = "finish"
                    event = "Финиш! Машина остаётся на трассе до конца раунда"
                frame(event, current, index + 1, route, cost, limit)
            assert cost == int(match[2])
            continue
        match = re.fullmatch(r"Сход: машина (\d+), продвижение -?\d+\.", line)
        if match:
            id_ = int(match[1])
            c = state[id_]
            c["status"] = "retired"
            if occupied.get((c["lane"], c["cell"])) == id_:
                del occupied[c["lane"], c["cell"]]
            # finish_round проходит по стартовому массиву; к этому моменту
            # часть финишировавших уже удалена. Их порядок известен из старта.
            for earlier, old in state.items():
                if earlier == id_:
                    break
                if old["status"] != "active" and occupied.get((old["lane"], old["cell"])) == earlier:
                    del occupied[old["lane"], old["cell"]]
            frame("retire", id_)
    assert next(actual, None) is None, "В терминале есть лишний или дублированный кадр"
    return checked, steps
