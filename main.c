#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_TEAMS 8
#define MAX_CARS_PER_TEAM 4
#define MAX_CARS (MAX_TEAMS * MAX_CARS_PER_TEAM)
#define MAX_LANES 6
#define MAX_LENGTH 120
#define MAX_STEPS (MAX_CARS + 4)

enum { ACTIVE, FINISHED, RETIRED, UNFINISHED };
enum { FORWARD, INSIDE, OUTSIDE };
enum { GREEDY, RANDOM };

typedef struct {
    int teams;
    int cars_per_team;
    int lanes;
    int length;
    int laps;
    int max_rounds;
    int strategy;
    unsigned int seed;
    int quiet;
    const char *log_path;
    int delay_ms;
} Config;

typedef struct {
    int id;
    int team;
    int lane;
    int position;
    int distance;       // Продвижение относительно стартовой линии; на старте отрицательное.
    int status;
    int zero_move;
    int finish_place;
} Car;

typedef struct {
    int directions[MAX_STEPS];
    int count;
    int cost;
} Route;

typedef struct {
    Config config;
    Car cars[MAX_CARS];
    int track[MAX_LANES][MAX_LENGTH]; // 0 — свободно, другое число — номер машины.
    int car_count;
    int finish_count;
    int rounds;
    int order[MAX_CARS];
    int order_count;
} Race;

static FILE *log_file;
static int quiet;
static int animation;
static int screen_active;
static volatile sig_atomic_t interrupted;

// Подробные события можно скрыть на экране, но в журнал они попадают всегда.
static void output(int detail, const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (log_file != NULL) {
        va_list copy;
        va_copy(copy, args);
        vfprintf(log_file, format, copy);
        va_end(copy);
    }
    if (!detail || (!quiet && !animation)) {
        vprintf(format, args);
    }
    va_end(args);
}

static void handle_interrupt(int signal_number) {
    interrupted = signal_number;
}

static void stop_animation(void) {
    if (screen_active) {
        printf("\033[0m\033[?25h\033[?1049l");
        fflush(stdout);
        screen_active = 0;
    }
}

static void start_animation(void) {
    if (animation) {
        printf("\033[?1049h\033[?25l\033[2J");
        screen_active = 1;
    }
}

static const char *status_name(int status) {
    switch (status) {
        case ACTIVE: return "участвует";
        case FINISHED: return "финиш";
        case RETIRED: return "сход";
        default: return "не завершила";
    }
}

static void initialize_race(Race *race, Config config) {
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

static int active_count(const Race *race) {
    int count = 0;
    for (int i = 0; i < race->car_count; i++) {
        if (race->cars[i].status == ACTIVE) count++;
    }
    return count;
}

static void draw_border(int visible, int cell_width) {
    printf("   +");
    for (int i = 0; i < visible; i++) printf("%.*s+", cell_width - 1, "---");
    printf("\033[K\n");
}

// Отдельный кадр после каждого перехода. Управляющие коды не попадают в журнал.
static void animate_frame(const Race *race, const Car *car, const char *event,
                          int step, int count, int used, int limit, const Route *route) {
    if (!screen_active || interrupted) return;
    static const int colors[] = {31, 32, 33, 34, 35, 36, 91, 92};
    struct winsize size = {0};
    int columns = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        columns = size.ws_col;
    }
    int cell_width = race->config.length > 99 ? 4 : 3;
    int visible = (columns - 4) / cell_width;
    if (visible < 1) visible = 1;
    if (visible > race->config.length) visible = race->config.length;
    int focus = car != NULL ? car->position : race->cars[0].position;
    int first = focus - visible / 2;
    if (first < 0) first = 0;
    if (first + visible > race->config.length) first = race->config.length - visible;

    printf("\033[HАвтогонки | Раунд %d/%d | Участвуют: %d | Финиш: %d\033[K\n",
           race->rounds, race->config.max_rounds, active_count(race), race->finish_count);
    if (car != NULL) {
        int laps = car->distance > 0 ? car->distance / race->config.length : 0;
        printf("Машина %d, команда %d | Дорожка %d, клетка %d | Круги %d/%d\033[K\n",
               car->id, car->team, car->lane + 1, car->position + 1, laps, race->config.laps);
        printf("Шаг %d/%d | Потрачено %d из %d единиц хода\033[K\n", step, count, used, limit);
    } else {
        printf("Кругов: %d | Машин: %d | Задержка: %d мс\033[K\n",
               race->config.laps, race->car_count, race->config.delay_ms);
        printf("Машины движутся по одному переходу. Ctrl+C — завершить.\033[K\n");
    }
    printf("Событие: %s\033[K\n", event);
    printf("Порядок:");
    int line_width = 8;
    for (int i = 0; i < race->order_count; i++) {
        char token[8];
        int width = snprintf(token, sizeof(token), " %d", race->cars[race->order[i]].id);
        if (line_width + width > columns) {
            printf("\033[K\n        ");
            line_width = 8;
        }
        printf("%s", token);
        line_width += width;
    }
    printf("\033[K\nМаршрут (В=вперёд, И=внутрь, Н=наружу): ");
    if (route != NULL && route->count > 0) {
        static const char *symbols[] = {"В", "И", "Н"};
        for (int i = 0; i < route->count; i++) printf("%s", symbols[route->directions[i]]);
    } else {
        printf("—");
    }
    printf("\033[K\n");
    printf("Вправо -> | Клетки %d–%d из %d | Старт/финиш: клетка 1\033[K\n",
           first + 1, first + visible, race->config.length);
    printf("Команды:");
    for (int team = 1; team <= race->config.teams; team++) {
        printf(" \033[1;%dm%d\033[0m", colors[team - 1], team);
    }
    printf(" | Выделена текущая машина\033[K\n");
    printf(" № ");
    for (int p = first; p < first + visible; p++) printf("|%*d", cell_width - 1, p + 1);
    printf("|\033[K\n");
    draw_border(visible, cell_width);
    for (int lane = 0; lane < race->config.lanes; lane++) {
        printf("Д%d ", lane + 1);
        for (int p = first; p < first + visible; p++) {
            int id = race->track[lane][p];
            if (id == 0) {
                printf("|%*s", cell_width - 1, ".");
            } else {
                int team = (id - 1) / race->config.cars_per_team;
                int highlight = car != NULL && id == car->id ? 7 : 1;
                printf("|\033[%d;%dm%*d\033[0m", highlight, colors[team], cell_width - 1, id);
            }
        }
        printf("|\033[K\n");
        draw_border(visible, cell_width);
    }
    printf("\033[J");
    fflush(stdout);

    struct timespec delay = {race->config.delay_ms / 1000,
                             (race->config.delay_ms % 1000) * 1000000L};
    while (!interrupted && nanosleep(&delay, &delay) == -1 && errno == EINTR) {
        // Продолжаем паузу после постороннего сигнала; Ctrl+C отменяет задержки.
    }
}

// Сравниваем общее продвижение, затем дорожку, затем номер машины.
static int ahead(const Car *a, const Car *b) {
    if (a->distance != b->distance) return a->distance > b->distance;
    if (a->lane != b->lane) return a->lane < b->lane;
    return a->id < b->id;
}

static int create_order(const Race *race, int order[MAX_CARS]) {
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

static int step_cost(int direction) {
    return direction == OUTSIDE ? 3 : 1;
}

static void destination(const Config *config, int direction,
                        int *lane, int *position) {
    if (direction == FORWARD) *position = (*position + 1) % config->length;
    else if (direction == INSIDE) (*lane)--;
    else (*lane)++;
}

// Строим один допустимый маршрут. Другие машины во время его выбора неподвижны.
static Route choose_route(const Race *race, const Car *car, int limit,
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

static void execute_route(Race *race, Car *car, const Route *route, int limit) {
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
static void finish_round(Race *race) {
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

static void print_positions(const Race *race) {
    output(1, "Машина Команда Дорожка Клетка Продвижение Круги Состояние\n");
    for (int i = 0; i < race->car_count; i++) {
        const Car *car = &race->cars[i];
        int laps = car->distance > 0 ? car->distance / race->config.length : 0;
        output(1, "%6d %7d %7d %6d %11d %5d %s\n", car->id, car->team,
               car->lane + 1, car->position + 1, car->distance, laps,
               status_name(car->status));
    }
}

static void print_track(const Race *race) {
    output(1, "Трасса: вправо ->; после последней клетки идёт клетка 1.\n");
    output(1, "Клетка:    ");
    for (int p = 0; p < race->config.length; p++) output(1, "%3d", p + 1);
    output(1, "\n");
    for (int lane = 0; lane < race->config.lanes; lane++) {
        output(1, "Дорожка %d: ", lane + 1);
        for (int p = 0; p < race->config.length; p++) {
            if (race->track[lane][p] == 0) output(1, "  .");
            else output(1, "%3d", race->track[lane][p]);
        }
        output(1, "\n");
    }
}

static void run_round(Race *race) {
    int *order = race->order;
    int count = create_order(race, order);
    race->order_count = count;
    race->rounds++;
    output(1, "\nРаунд %d. Порядок:", race->rounds);
    for (int i = 0; i < count; i++) output(1, " %d", race->cars[order[i]].id);
    output(1, "\n");
    animate_frame(race, NULL, "Начало раунда. Порядок ходов зафиксирован", 0, 0, 0, 0, NULL);

    int previous_cost = 0;
    for (int i = 0; i < count; i++) {
        Car *car = &race->cars[order[i]];
        int limit = i == 0 ? 4 : previous_cost + (i == 1 ? 2 : 1);
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
        previous_cost = route.cost;
    }
    finish_round(race);
    race->order_count = create_order(race, race->order);
    animate_frame(race, NULL, "Раунд завершён. Показан порядок следующего раунда", 0, 0, 0, 0, NULL);
    print_positions(race);
    print_track(race);
}

// Финишировавшие идут по порядку финиша; остальные — по продвижению.
static int result_before(const Car *a, const Car *b) {
    if (a->status == FINISHED && b->status == FINISHED) {
        return a->finish_place < b->finish_place;
    }
    if (a->status == FINISHED) return 1;
    if (b->status == FINISHED) return 0;
    return ahead(a, b);
}

static void create_results(const Race *race, int order[MAX_CARS]) {
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

static void print_results(const Race *race, const char *reason) {
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

static void help(void) {
    puts("Автогонки. Использование: ./race [параметры]\n"
         "  --teams N       команды: 1..8 (по умолчанию 3)\n"
         "  --cars N        машин в команде: 1..4 (по умолчанию 2)\n"
         "  --lanes N       дорожки: 1..6 (по умолчанию 4)\n"
         "  --length N      клеток на дорожке: 3..120 (по умолчанию 24)\n"
         "  --laps N        круги: 1..100 (по умолчанию 2)\n"
         "  --rounds N      предел раундов: 1..10000 (по умолчанию 100)\n"
         "  --seed N        начальное значение генератора (по умолчанию 1)\n"
         "  --strategy S    greedy или random (по умолчанию greedy)\n"
         "  --log FILE      записать полный журнал в файл\n"
         "  --quiet         показывать только параметры и итоговые результаты\n"
         "  --delay MS      пауза между переходами: 0..5000 мс (по умолчанию 120)\n"
         "  --no-animation  обычный текстовый вывод без анимации\n"
         "  --help          эта справка\n"
         "На дорожке должно быть больше клеток, чем машин на старте.");
}

static int read_number(const char *text, unsigned int maximum, unsigned int *value) {
    if (*text < '0' || *text > '9') return 0;
    char *end;
    errno = 0;
    unsigned long number = strtoul(text, &end, 10);
    if (*end != '\0' || errno == ERANGE || number > maximum) return 0;
    *value = (unsigned int)number;
    return 1;
}

// Возвращает 1 при успехе, 0 при ошибке и 2 при запросе справки.
static int parse_arguments(int argc, char *argv[], Config *config) {
    for (int i = 1; i < argc; i++) {
        const char *option = argv[i];
        if (strcmp(option, "--help") == 0) return 2;
        if (strcmp(option, "--quiet") == 0) {
            config->quiet = 1;
            continue;
        }
        if (strcmp(option, "--no-animation") == 0) {
            config->delay_ms = 0;
            continue;
        }
        int *target = NULL;
        if (strcmp(option, "--teams") == 0) target = &config->teams;
        else if (strcmp(option, "--cars") == 0) target = &config->cars_per_team;
        else if (strcmp(option, "--lanes") == 0) target = &config->lanes;
        else if (strcmp(option, "--length") == 0) target = &config->length;
        else if (strcmp(option, "--laps") == 0) target = &config->laps;
        else if (strcmp(option, "--rounds") == 0) target = &config->max_rounds;
        else if (strcmp(option, "--delay") == 0) target = &config->delay_ms;
        else if (strcmp(option, "--seed") != 0 && strcmp(option, "--strategy") != 0 &&
                 strcmp(option, "--log") != 0) {
            fprintf(stderr, "Неизвестный параметр: %s\n", option);
            return 0;
        }
        if (++i == argc || strncmp(argv[i], "--", 2) == 0) {
            fprintf(stderr, "Не задано значение: %s\n", option);
            return 0;
        }
        const char *value = argv[i];
        if (strcmp(option, "--strategy") == 0) {
            if (strcmp(value, "greedy") == 0) config->strategy = GREEDY;
            else if (strcmp(value, "random") == 0) config->strategy = RANDOM;
            else {
                fprintf(stderr, "Стратегия должна быть greedy или random.\n");
                return 0;
            }
        } else if (strcmp(option, "--log") == 0) {
            config->log_path = value;
        } else {
            unsigned int number;
            unsigned int maximum = target == NULL ? UINT_MAX : INT_MAX;
            if (!read_number(value, maximum, &number)) {
                fprintf(stderr, "Некорректное число для %s: %s\n", option, value);
                return 0;
            }
            if (target != NULL) *target = (int)number;
            else config->seed = number;
        }
    }
    if (config->teams < 1 || config->teams > MAX_TEAMS ||
        config->cars_per_team < 1 || config->cars_per_team > MAX_CARS_PER_TEAM ||
        config->lanes < 1 || config->lanes > MAX_LANES ||
        config->length < 3 || config->length > MAX_LENGTH ||
        config->laps < 1 || config->laps > 100 ||
        config->max_rounds < 1 || config->max_rounds > 10000 ||
        config->delay_ms < 0 || config->delay_ms > 5000) {
        fprintf(stderr, "Параметры вне допустимых границ. См. --help.\n");
        return 0;
    }
    if (config->teams * config->cars_per_team >= config->length) {
        fprintf(stderr, "На дорожке должно быть больше клеток, чем машин. Увеличьте --length.\n");
        return 0;
    }
    return 1;
}

int main(int argc, char *argv[]) {
    Config config = {3, 2, 4, 24, 2, 100, GREEDY, 1, 0, NULL, 120};
    int parsed = parse_arguments(argc, argv, &config);
    if (parsed == 2) { help(); return 0; }
    if (parsed == 0) return 1;

    if (config.log_path != NULL) {
        log_file = fopen(config.log_path, "w");
        if (log_file == NULL) { perror("Не удалось открыть журнал"); return 1; }
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_interrupt;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 || sigaction(SIGTERM, &action, NULL) == -1) {
        perror("Не удалось установить обработчик прерывания");
        if (log_file != NULL) fclose(log_file);
        return 1;
    }

    quiet = config.quiet;
    const char *term = getenv("TERM");
    animation = !quiet && config.delay_ms > 0 && isatty(STDOUT_FILENO) &&
                (term == NULL || strcmp(term, "dumb") != 0);
    if (animation && atexit(stop_animation) != 0) animation = 0;
    srand(config.seed);
    Race race;
    initialize_race(&race, config);
    output(0, "Автогонки: команд %d, машин в команде %d, дорожек %d, клеток %d, кругов %d.\n",
           config.teams, config.cars_per_team, config.lanes, config.length, config.laps);
    output(0, "Предел раундов: %d. Seed: %u. Стратегия: %s.\n",
           config.max_rounds, config.seed, config.strategy == GREEDY ? "greedy" : "random");
    output(0, "Круговые премии и дополнительные правила снятия отставших отключены.\n");
    output(1, "Стартовая расстановка. Линия старта и финиша: клетка 1.\n");
    print_positions(&race);
    print_track(&race);

    start_animation();
    animate_frame(&race, NULL, "Стартовая расстановка", 0, 0, 0, 0, NULL);

    while (active_count(&race) > 0 && race.rounds < config.max_rounds && !interrupted) {
        run_round(&race);
    }
    const char *reason = "все машины финишировали или сошли";
    if (active_count(&race) > 0) {
        reason = interrupted ? "прерывание пользователем" : "достигнут предел раундов";
        for (int i = 0; i < race.car_count; i++) {
            if (race.cars[i].status == ACTIVE) race.cars[i].status = UNFINISHED;
        }
    }
    stop_animation();
    print_results(&race, reason);

    int failed = ferror(stdout);
    if (log_file != NULL) {
        if (ferror(log_file)) {
            fprintf(stderr, "Ошибка записи журнала.\n");
            failed = 1;
        }
        if (fclose(log_file) == EOF) {
            perror("Не удалось сохранить журнал");
            failed = 1;
        }
    }
    if (fflush(stdout) == EOF) failed = 1;
    if (failed) return 1;
    return interrupted ? 128 + interrupted : 0;
}
