// Проверяем внутренние правила без изменения интерфейса основной программы.
#define main race_cli_main
#include "../main.c"
#undef main
#include <assert.h>

static Config settings(int lanes, int length) {
    Config config = {1, 1, lanes, length, 2, 100, GREEDY, 1, 1, NULL, 0};
    return config;
}

static void add_car(Race *race, int lane, int distance) {
    int index = race->car_count++;
    Car *car = &race->cars[index];
    car->id = index + 1;
    car->team = 1;
    car->lane = lane;
    car->distance = distance;
    car->position = (distance % race->config.length + race->config.length) % race->config.length;
    car->status = ACTIVE;
    assert(race->track[lane][car->position] == 0);
    race->track[lane][car->position] = car->id;
}

// Независимо пересчитываем путь: стоимость, занятость, отсутствие повторов и финиш.
static void verify_route(const Race *race, const Car *car, Route route, int limit) {
    int visited[MAX_LANES][MAX_LENGTH] = {{0}};
    int lane = car->lane;
    int position = car->position;
    int distance = car->distance;
    int cost = 0;
    visited[lane][position] = 1;
    for (int i = 0; i < route.count; i++) {
        int direction = route.directions[i];
        assert(direction >= FORWARD && direction <= OUTSIDE);
        if (direction == FORWARD) {
            position++;
            if (position == race->config.length) position = 0;
            distance++;
            cost++;
        } else if (direction == INSIDE) {
            lane--;
            cost++;
        } else {
            lane++;
            cost += 3;
        }
        assert(lane >= 0 && lane < race->config.lanes);
        assert(!visited[lane][position]);
        assert(race->track[lane][position] == 0);
        visited[lane][position] = 1;
        assert(cost <= limit);
        assert(distance <= race->config.laps * race->config.length);
        if (distance == race->config.laps * race->config.length) assert(i == route.count - 1);
    }
    assert(cost == route.cost);
    if (route.count == 0) {
        int available = 0;
        if (race->track[lane][(position + 1) % race->config.length] == 0 && limit >= 1) available++;
        if (lane > 0 && race->track[lane - 1][position] == 0 && limit >= 1) available++;
        if (lane + 1 < race->config.lanes && race->track[lane + 1][position] == 0 && limit >= 3) available++;
        assert(available == 0);
    }
}

static void verify_state(const Race *race, int after_round) {
    int found[MAX_CARS] = {0};
    for (int lane = 0; lane < race->config.lanes; lane++) {
        for (int p = 0; p < race->config.length; p++) {
            int id = race->track[lane][p];
            if (id == 0) continue;
            int index = -1;
            for (int i = 0; i < race->car_count; i++) {
                if (race->cars[i].id == id) index = i;
            }
            assert(index >= 0);
            found[index]++;
            assert(race->cars[index].lane == lane && race->cars[index].position == p);
        }
    }
    for (int i = 0; i < race->car_count; i++) {
        const Car *car = &race->cars[i];
        assert(found[i] <= 1);
        if (car->status == ACTIVE) assert(found[i] == 1);
        else if (after_round) assert(found[i] == 0);
        assert(car->position == (car->distance % race->config.length + race->config.length) % race->config.length);
        if (car->status == FINISHED) {
            assert(car->position == 0 && car->distance == race->config.laps * race->config.length);
        }
    }
}

static void test_start(void) {
    Race race;
    Config config = {3, 2, 4, 24, 2, 100, GREEDY, 1, 1, NULL, 0};
    srand(1);
    initialize_race(&race, config);
    verify_state(&race, 1);
    for (int i = 0; i < 3; i++) {
        assert(race.cars[i].team == race.cars[i + 3].team);
        assert(race.cars[i].id + 1 == race.cars[i + 3].id);
        assert(race.cars[i].distance == -i - 1);
        assert(race.cars[i].lane == 3);
        for (int j = 0; j < i; j++) assert(race.cars[i].team != race.cars[j].team);
    }
}

static void test_side_cost_and_blocking(void) {
    Race race = {0};
    race.config = settings(2, 6);
    add_car(&race, 0, 0);
    add_car(&race, 0, 1);
    int blocked;
    Route route = choose_route(&race, &race.cars[0], 2, &blocked);
    assert(route.count == 0 && blocked);
    route = choose_route(&race, &race.cars[0], 3, &blocked);
    assert(route.count == 1 && route.directions[0] == OUTSIDE && route.cost == 3);
    verify_route(&race, &race.cars[0], route, 3);

    memset(&race, 0, sizeof(race));
    race.config = settings(2, 6);
    add_car(&race, 1, 0);
    add_car(&race, 1, 1);
    route = choose_route(&race, &race.cars[0], 1, &blocked);
    assert(route.count == 1 && route.directions[0] == INSIDE && route.cost == 1);
    verify_route(&race, &race.cars[0], route, 1);
}

static void test_no_repeat(void) {
    Race race = {0};
    race.config = settings(1, 3);
    add_car(&race, 0, -1);
    int blocked;
    Route route = choose_route(&race, &race.cars[0], 20, &blocked);
    assert(route.count == 2 && route.cost == 2);
    assert(!blocked); // Собственная исходная клетка — повтор, а не блокировка другой машиной.
    verify_route(&race, &race.cars[0], route, 20);
}

static void test_delayed_removal(void) {
    Race race = {0};
    race.config = settings(1, 4);
    race.config.laps = 1;
    add_car(&race, 0, 3);
    add_car(&race, 0, 2);
    int blocked;
    Route route = choose_route(&race, &race.cars[0], 4, &blocked);
    assert(route.cost == 1);
    execute_route(&race, &race.cars[0], &route, 4);
    assert(race.cars[0].status == FINISHED && race.track[0][0] == 1);
    route = choose_route(&race, &race.cars[1], 3, &blocked);
    assert(route.cost == 1 && blocked);
    execute_route(&race, &race.cars[1], &route, 3);
    assert(race.cars[1].distance == 3);
    finish_round(&race);
    assert(race.track[0][0] == 0);
    run_round(&race);
    assert(race.cars[1].status == FINISHED && race.cars[1].finish_place == 2);
    verify_state(&race, 1);

    memset(&race, 0, sizeof(race));
    race.config = settings(1, 4);
    add_car(&race, 0, 0);
    add_car(&race, 0, 1);
    add_car(&race, 0, -1);
    route = choose_route(&race, &race.cars[0], 4, &blocked);
    assert(route.count == 0);
    race.cars[0].zero_move = 1;
    assert(race.track[0][0] == 1 && race.cars[0].status == ACTIVE);
    route = choose_route(&race, &race.cars[2], 2, &blocked);
    assert(route.count == 0); // Будущий сход пока остаётся препятствием.
    finish_round(&race);
    assert(race.cars[0].status == RETIRED && race.track[0][0] == 0);

    // Повторная уборка не удаляет машину, занявшую бывшую клетку финишировавшего.
    race.cars[0].status = FINISHED;
    race.track[0][1] = 0;
    race.cars[1].position = 0;
    race.cars[1].distance = 0;
    race.track[0][0] = 2;
    finish_round(&race);
    assert(race.track[0][0] == 2);
}

static void test_order_and_results(void) {
    Race race = {0};
    race.car_count = 4;
    race.cars[0] = (Car){.id = 1, .team = 1, .lane = 2, .distance = 10};
    race.cars[1] = (Car){.id = 2, .team = 1, .lane = 0, .distance = 10};
    race.cars[2] = (Car){.id = 3, .team = 2, .lane = 0, .distance = 9};
    race.cars[3] = (Car){.id = 4, .team = 2, .lane = 0, .distance = 10};
    int order[MAX_CARS];
    assert(create_order(&race, order) == 4);
    assert(order[0] == 1 && order[1] == 3 && order[2] == 0 && order[3] == 2);
    race.cars[0].status = FINISHED;
    race.cars[0].finish_place = 2;
    race.cars[2].status = FINISHED;
    race.cars[2].finish_place = 1;
    race.cars[1].status = RETIRED;
    race.cars[3].status = UNFINISHED;
    create_results(&race, order);
    assert(order[0] == 2 && order[1] == 0 && order[2] == 1 && order[3] == 3);
}

static void test_small_boards(void) {
    int cases = 0;
    // Все расстановки на 2x4 с одной движущейся машиной и до двух препятствий.
    for (int source = 0; source < 8; source++) {
        for (unsigned mask = 0; mask < 256; mask++) {
            if (mask & (1u << source)) continue;
            int obstacles = 0;
            for (int cell = 0; cell < 8; cell++) obstacles += (mask >> cell) & 1u;
            if (obstacles > 2) continue;
            for (int strategy = GREEDY; strategy <= RANDOM; strategy++) {
                for (int limit = 1; limit <= 7; limit++) {
                    Race race = {0};
                    race.config = settings(2, 4);
                    race.config.strategy = strategy;
                    add_car(&race, source / 4, source % 4);
                    for (int cell = 0; cell < 8; cell++) {
                        if (mask & (1u << cell)) add_car(&race, cell / 4, cell % 4);
                    }
                    srand((unsigned)cases);
                    int blocked;
                    Route route = choose_route(&race, &race.cars[0], limit, &blocked);
                    verify_route(&race, &race.cars[0], route, limit);
                    execute_route(&race, &race.cars[0], &route, limit);
                    verify_state(&race, 0);
                    cases++;
                }
            }
        }
    }
    assert(cases == 3248);
}

static void test_round_invariants(void) {
    Config config = {8, 4, 2, 33, 2, 100, RANDOM, 0, 1, NULL, 0};
    Race race;
    srand(config.seed);
    initialize_race(&race, config);
    for (int round = 0; round < config.max_rounds && active_count(&race); round++) {
        Car before[MAX_CARS];
        memcpy(before, race.cars, sizeof(before));
        run_round(&race);
        verify_state(&race, 1);
        int expected[MAX_CARS];
        int count = create_order(&race, expected);
        assert(race.order_count == count);
        for (int i = 0; i < count; i++) assert(race.order[i] == expected[i]);
        for (int i = 0; i < race.car_count; i++) {
            if (before[i].status == ACTIVE) continue;
            assert(race.cars[i].status == before[i].status);
            assert(race.cars[i].lane == before[i].lane);
            assert(race.cars[i].position == before[i].position);
            assert(race.cars[i].distance == before[i].distance);
            assert(race.cars[i].finish_place == before[i].finish_place);
        }
    }
    assert(active_count(&race) == 0 && race.finish_count > 0 && race.finish_count < race.car_count);
}

static void test_many_races(void) {
    // Проверяем каждый выбранный маршрут и состояние после каждого хода.
    for (int strategy = GREEDY; strategy <= RANDOM; strategy++) {
        for (int seed = 0; seed < 100; seed++) {
            Config config = {1 + seed % 8, 1 + seed % 4, 1 + seed % 6,
                             33 + seed % 20, 1 + seed % 3, 200, strategy, (unsigned)seed, 1, NULL, 0};
            Race race;
            srand(config.seed);
            initialize_race(&race, config);
            verify_state(&race, 1);
            for (int round = 0; round < config.max_rounds && active_count(&race); round++) {
                int order[MAX_CARS];
                int count = create_order(&race, order);
                int previous = 0;
                for (int i = 0; i < count; i++) {
                    Car *car = &race.cars[order[i]];
                    int limit = i == 0 ? 4 : previous + (i == 1 ? 2 : 1);
                    assert(limit <= MAX_STEPS);
                    int blocked;
                    Route route = choose_route(&race, car, limit, &blocked);
                    verify_route(&race, car, route, limit);
                    if (route.count == 0) car->zero_move = 1;
                    else execute_route(&race, car, &route, limit);
                    previous = route.cost;
                    verify_state(&race, 0);
                }
                finish_round(&race);
                verify_state(&race, 1);
            }
        }
    }
}

int main(void) {
    quiet = 1;
    test_start();
    test_side_cost_and_blocking();
    test_no_repeat();
    test_delayed_removal();
    test_order_and_results();
    test_small_boards();
    test_round_invariants();
    test_many_races();
    puts("C: 3248 маршрутов малых трасс, инварианты раунда и 200 гонок проверены.");
    return 0;
}
