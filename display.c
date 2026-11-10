#define _POSIX_C_SOURCE 200809L
#include "display.h"
#include <errno.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

FILE *log_file;
int quiet;
int animation;
int screen_active;
// Только lock-free atomic допустим и для потоков, и для обработчика сигнала.
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Требуется lock-free atomic int");
atomic_int interrupted;
void (*output_sink)(int detail, const char *format, va_list args);
void (*frame_sink)(const Race *, const Car *, const char *, int, int, int, int, const Route *);
atomic_int *remote_interrupt;

int interruption_requested(void) {
    if (remote_interrupt != NULL) {
        int signal_number = atomic_load(remote_interrupt);
        if (signal_number != 0) atomic_store(&interrupted, signal_number);
    }
    return atomic_load(&interrupted);
}

// Подробные события можно скрыть на экране, но в журнал они попадают всегда.
void output(int detail, const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (output_sink != NULL) {
        output_sink(detail, format, args);
        va_end(args);
        return;
    }
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

void handle_interrupt(int signal_number) {
    atomic_store_explicit(&interrupted, signal_number, memory_order_relaxed);
}

void stop_animation(void) {
    if (screen_active) {
        printf("\033[0m\033[?25h\033[?1049l");
        fflush(stdout);
        screen_active = 0;
    }
}

void start_animation(void) {
    if (animation) {
        printf("\033[?1049h\033[?25l\033[2J");
        screen_active = 1;
    }
}

static void draw_border(int visible, int cell_width) {
    printf("   +");
    for (int i = 0; i < visible; i++) printf("%.*s+", cell_width - 1, "---");
    printf("\033[K\n");
}

// Отдельный кадр после каждого перехода. Управляющие коды не попадают в журнал.
void animate_frame(const Race *race, const Car *car, const char *event,
                          int step, int count, int used, int limit, const Route *route) {
    if (frame_sink != NULL) {
        frame_sink(race, car, event, step, count, used, limit, route);
        return;
    }
    if (!screen_active || interruption_requested()) return;
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

    // Проверяем также запрос прерывания от независимо запущенной машины.
    int remaining_ms = race->config.delay_ms;
    while (remaining_ms > 0 && !interruption_requested()) {
        int part_ms = remaining_ms > 20 ? 20 : remaining_ms;
        struct timespec delay = {0, part_ms * 1000000L};
        while (!interruption_requested() && nanosleep(&delay, &delay) == -1 && errno == EINTR) {}
        remaining_ms -= part_ms;
    }
}

void print_positions(const Race *race) {
    output(1, "Машина Команда Дорожка Клетка Продвижение Круги Состояние\n");
    for (int i = 0; i < race->car_count; i++) {
        const Car *car = &race->cars[i];
        int laps = car->distance > 0 ? car->distance / race->config.length : 0;
        output(1, "%6d %7d %7d %6d %11d %5d %s\n", car->id, car->team,
               car->lane + 1, car->position + 1, car->distance, laps,
               status_name(car->status));
    }
}

void print_track(const Race *race) {
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

