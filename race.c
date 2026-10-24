#include "race.h"
#include "display.h"
#include <stdlib.h>
#include <string.h>

const char *status_name(int status) {
    switch (status) {
        case ACTIVE: return "участвует";
        case FINISHED: return "финиш";
        case RETIRED: return "сход";
        default: return "не завершила";
    }
}

void initialize_race(Race *race, Config config) {
    memset(race, 0, sizeof(*race));
    race->config = config;
    race->car_count = config.teams * config.cars_per_team;

    int teams[MAX_TEAMS];
    for (int i = 0; i < config.teams; i++) {
        teams[i] = i + 1;
    }
    for (int i = config.teams - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int temp = teams[i];
        teams[i] = teams[j];
        teams[j] = temp;
    }

    // Сначала по одной машине каждой команды, затем следующие в том же порядке.
    int index = 0;
    for (int vehicle = 0; vehicle < config.cars_per_team; vehicle++) {
        for (int i = 0; i < config.teams; i++) {
            Car *car = &race->cars[index];
            car->id = (teams[i] - 1) * config.cars_per_team + vehicle + 1;
            car->team = teams[i];
            car->lane = config.lanes - 1;
            car->position = config.length - 1 - index;
            car->distance = -1 - index;
            car->status = ACTIVE;
            race->track[car->lane][car->position] = car->id;
            race->order[index] = index;
            index++;
        }
    }
    race->order_count = race->car_count;
}

int active_count(const Race *race) {
    int count = 0;
    for (int i = 0; i < race->car_count; i++) {
        if (race->cars[i].status == ACTIVE) count++;
    }
    return count;
}

// Сравниваем общее продвижение, затем дорожку, затем номер машины.
int ahead(const Car *a, const Car *b) {
    if (a->distance != b->distance) return a->distance > b->distance;
    if (a->lane != b->lane) return a->lane < b->lane;
    return a->id < b->id;
}

int create_order(const Race *race, int order[MAX_CARS]) {
    int count = 0;
    for (int i = 0; i < race->car_count; i++) {
        if (race->cars[i].status == ACTIVE) order[count++] = i;
    }
    for (int i = 1; i < count; i++) {
        int car_index = order[i];
        int j = i;
        while (j > 0 && ahead(&race->cars[car_index], &race->cars[order[j - 1]])) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = car_index;
    }
    return count;
}

int step_cost(int direction) {
    return direction == OUTSIDE ? 3 : 1;
}

void destination(const Config *config, int direction,
                        int *lane, int *position) {
    if (direction == FORWARD) *position = (*position + 1) % config->length;
    else if (direction == INSIDE) (*lane)--;
    else (*lane)++;
}

// Строим один допустимый маршрут. Другие машины во время его выбора неподвижны.
Route choose_route(const Race *race, const Car *car, int limit,
                          int *blocked) {
    Route route = {0};
    int visited[MAX_LANES][MAX_LENGTH] = {{0}};
    int lane = car->lane;
    int position = car->position;
    int distance = car->distance;
    visited[lane][position] = 1;
    *blocked = 0;

    while (route.cost < limit && route.count < MAX_STEPS) {
        int choices[3];
        int count = 0;
        for (int direction = FORWARD; direction <= OUTSIDE; direction++) {
            int next_lane = lane;
            int next_position = position;
            destination(&race->config, direction, &next_lane, &next_position);
            if (next_lane < 0 || next_lane >= race->config.lanes) continue;
            if (step_cost(direction) > limit - route.cost) continue;
            if (visited[next_lane][next_position]) continue;
            if (race->track[next_lane][next_position] != 0) {
                *blocked = 1;
                continue;
            }
            choices[count++] = direction;
        }
        if (count == 0) break;

        // greedy: вперёд, иначе внутрь, иначе наружу. random: случайный допустимый шаг.
        int direction = choices[0];
        if (race->config.strategy == RANDOM) direction = choices[rand() % count];
        route.directions[route.count++] = direction;
        route.cost += step_cost(direction);
        destination(&race->config, direction, &lane, &position);
        visited[lane][position] = 1;
        if (direction == FORWARD) distance++;

        // Финиш обрывает маршрут точно на линии, даже если остался запас движения.
        if (distance == race->config.laps * race->config.length) break;
    }
    return route;
}

void execute_route(Race *race, Car *car, const Route *route, int limit) {
    static const char *names[] = {"вперёд", "внутрь", "наружу"};
    int used = 0;
    output(1, "  Маршрут:");
    for (int i = 0; i < route->count; i++) {
        int direction = route->directions[i];
        const char *event = names[direction];
        used += step_cost(direction);
        int old_lane = car->lane;
        race->track[car->lane][car->position] = 0;
        destination(&race->config, direction, &car->lane, &car->position);
        race->track[car->lane][car->position] = car->id;
        output(1, " %s", names[direction]);

        if (direction != FORWARD) {
            output(1, "(%d->%d)", old_lane + 1, car->lane + 1);
        } else {
            car->distance++;
            if (car->distance == 0) {
                output(1, " [стартовая линия]");
                event = "Пересечение стартовой линии";
            } else if (car->distance > 0 && car->distance % race->config.length == 0) {
                output(1, " [круг %d завершён]", car->distance / race->config.length);
                event = "Круг завершён";
            }
        }
        if (car->distance == race->config.laps * race->config.length) {
            car->status = FINISHED;
            car->finish_place = ++race->finish_count;
            output(1, " [финиш, место %d]", car->finish_place);
            event = "Финиш! Машина остаётся на трассе до конца раунда";
        }
        animate_frame(race, car, event, i + 1, route->count, used, limit, route);
        if (car->status == FINISHED) break;
    }
    output(1, ". Стоимость: %d. Дорожка: %d. Клетка: %d.\n",
           route->cost, car->lane + 1, car->position + 1);
}

// До этого момента нулевой ход и финиш не освобождают занятую клетку.
void finish_round(Race *race) {
    for (int i = 0; i < race->car_count; i++) {
        Car *car = &race->cars[i];
        int retired_now = 0;
        if (car->status == ACTIVE && car->zero_move) {
            car->status = RETIRED;
            retired_now = 1;
            output(1, "Сход: машина %d, продвижение %d.\n", car->id, car->distance);
        }
        if (car->status == FINISHED || car->status == RETIRED) {
            // Ранее снятая машина не должна очистить клетку, занятую новым участником.
            if (race->track[car->lane][car->position] == car->id) {
                race->track[car->lane][car->position] = 0;
            }
        }
        if (retired_now) {
            animate_frame(race, car, "Сход: машина снята с дистанции", 0, 0, 0, 0, NULL);
        }
    }
}

void begin_round(Race *race) {
    int *order = race->order;
    int count = create_order(race, order);
    race->order_count = count;
    race->rounds++;
    output(1, "\nРаунд %d. Порядок:", race->rounds);
    for (int i = 0; i < count; i++) output(1, " %d", race->cars[order[i]].id);
    output(1, "\n");
    animate_frame(race, NULL, "Начало раунда. Порядок ходов зафиксирован", 0, 0, 0, 0, NULL);

}

int perform_turn(Race *race, int car_index, int limit) {
    Car *car = &race->cars[car_index];
    int blocked;
    Route route = choose_route(race, car, limit, &blocked);
    output(1, "Машина %d (команда %d): предел %d.\n", car->id, car->team, limit);
    if (blocked) output(1, "  Блокирование: часть переходов занята другими машинами.\n");
    if (route.count > 0 || blocked) {
        animate_frame(race, car, blocked ? "Блокирование: занятые клетки исключены" : "Выбран маршрут",
                      0, route.count, 0, limit, &route);
    }
    if (route.count == 0) {
        car->zero_move = 1;
        output(1, "  Нулевой ход: нет допустимого перехода. Сход в конце раунда.\n");
        animate_frame(race, car, "Нулевой ход. Сход в конце раунда", 0, 0, 0, limit, &route);
    } else {
        execute_route(race, car, &route, limit);
    }
    return route.cost;
}

void end_round(Race *race) {
    finish_round(race);
    race->order_count = create_order(race, race->order);
    animate_frame(race, NULL, "Раунд завершён. Показан порядок следующего раунда", 0, 0, 0, 0, NULL);
    print_positions(race);
    print_track(race);
}


// Финишировавшие идут по порядку финиша; остальные — по продвижению.
int result_before(const Car *a, const Car *b) {
    if (a->status == FINISHED && b->status == FINISHED) {
        return a->finish_place < b->finish_place;
    }
    if (a->status == FINISHED) return 1;
    if (b->status == FINISHED) return 0;
    return ahead(a, b);
}

void create_results(const Race *race, int order[MAX_CARS]) {
    for (int i = 0; i < race->car_count; i++) order[i] = i;
    for (int i = 1; i < race->car_count; i++) {
        int car_index = order[i];
        int j = i;
        while (j > 0 && result_before(&race->cars[car_index], &race->cars[order[j - 1]])) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = car_index;
    }
}

void print_results(const Race *race, const char *reason) {
    int order[MAX_CARS];
    create_results(race, order);
    output(0, "\nГонка завершена: %s. Раундов: %d.\n", reason, race->rounds);
    output(0, "Результаты машин:\nМесто Машина Команда Продвижение Состояние\n");
    for (int i = 0; i < race->car_count; i++) {
        const Car *car = &race->cars[order[i]];
        output(0, "%5d %6d %7d %11d %s\n", i + 1, car->id, car->team,
               car->distance, status_name(car->status));
    }

    // Команды сравниваются по месту своей лучшей машины в итоговой классификации.
    int seen[MAX_TEAMS] = {0};
    int place = 0;
    output(0, "Результаты команд (по лучшей машине):\nМесто Команда Лучшая_машина Место_машины\n");
    for (int i = 0; i < race->car_count; i++) {
        const Car *car = &race->cars[order[i]];
        if (!seen[car->team - 1]) {
            seen[car->team - 1] = 1;
            output(0, "%5d %7d %13d %12d\n", ++place, car->team, car->id, i + 1);
        }
    }
}

