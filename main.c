#define _POSIX_C_SOURCE 200809L
#include "race.h"
#include "display.h"
#include "processes.h"
#include "queues.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef RACE_MODE
#define RACE_MODE 0
#endif
static const char *session;
static int connection_timeout = 10;

static void help(const char *program) {
    printf("Автогонки (ИДЗ 3, процессы). Использование: %s [параметры]\n", program);
    puts("  --teams N       команды: 1..8 (по умолчанию 3)\n"
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
         "  --no-animation  текстовый вывод без анимации; имеет приоритет над --delay\n"
         "  --help          эта справка\n"
         "На дорожке должно быть больше клеток, чем машин на старте.");
    if (RACE_MODE >= INDEPENDENT) {
        printf("Судья: %s --session NAME [параметры]\n"
               "Машина в другом терминале: %s NAME ID [секунд_ожидания]\n"
               "  --wait SEC      ожидание машин: 1..60 секунд (по умолчанию 10)\n",
               program, RACE_MODE == QUEUED ? "./race-mq-car" : "./race-car");
    }
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
        if (strcmp(option, "--session") == 0 && RACE_MODE >= INDEPENDENT) {
            if (++i == argc) { fprintf(stderr, "Не задано имя сессии.\n"); return 0; }
            session = argv[i];
            continue;
        }
        if (strcmp(option, "--wait") == 0 && RACE_MODE >= INDEPENDENT) {
            unsigned int number;
            if (++i == argc || !read_number(argv[i], 60, &number) || number == 0) {
                fprintf(stderr, "--wait: ожидается число от 1 до 60.\n"); return 0;
            }
            connection_timeout = (int)number;
            continue;
        }
        if (strcmp(option, "--help") == 0) return 2;
        if (strcmp(option, "--quiet") == 0) {
            config->quiet = 1;
            continue;
        }
        if (strcmp(option, "--no-animation") == 0) {
            config->no_animation = 1;
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
    if (RACE_MODE >= INDEPENDENT && session == NULL) {
        fprintf(stderr, "Укажите --session NAME. См. --help.\n");
        return 0;
    }
    return 1;
}

int main(int argc, char *argv[]) {
    Config config = {3, 2, 4, 24, 2, 100, GREEDY, 1, 0, NULL, 120, 0};
    int parsed = parse_arguments(argc, argv, &config);
    if (parsed == 2) { help(argv[0]); return 0; }
    if (parsed == 0) return 1;

    if (config.log_path != NULL) {
        log_file = fopen(config.log_path, "w");
        if (log_file == NULL) { perror("Не удалось открыть журнал"); return 1; }
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_interrupt;
    // Запись stdio и ожидание семафора не должны ошибочно завершаться с EINTR.
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 || sigaction(SIGTERM, &action, NULL) == -1) {
        perror("Не удалось установить обработчик прерывания");
        if (log_file != NULL) fclose(log_file);
        return 1;
    }

    quiet = config.quiet;
    const char *term = getenv("TERM");
    animation = !config.no_animation && !quiet && config.delay_ms > 0 && isatty(STDOUT_FILENO) &&
                (term == NULL || strcmp(term, "dumb") != 0);
    if (animation && atexit(stop_animation) != 0) animation = 0;
    srand(config.seed);
    Race initial;
    initialize_race(&initial, config);
    output(0, "Автогонки: команд %d, машин в команде %d, дорожек %d, клеток %d, кругов %d.\n",
           config.teams, config.cars_per_team, config.lanes, config.length, config.laps);
    output(0, "Предел раундов: %d. Seed: %u. Стратегия: %s.\n",
           config.max_rounds, config.seed, config.strategy == GREEDY ? "greedy" : "random");
    output(0, "Круговые премии и дополнительные правила снятия отставших отключены.\n");
    output(1, "Стартовая расстановка. Линия старта и финиша: клетка 1.\n");
    print_positions(&initial);
    print_track(&initial);

    Race result = initial;
    int run_error = 0, cleanup_error = 0;
    if (RACE_MODE == QUEUED) {
        run_error = run_queue_judge(&result, session, connection_timeout);
        if (run_error) { errno = run_error; perror("Ошибка очередей сообщений"); }
    } else {
        RaceProcesses processes;
        int start_error = start_race(&processes, &initial, RACE_MODE, session);
        if (start_error != 0) {
            errno = start_error;
            perror("Не удалось запустить процессы машин");
            if (log_file != NULL) fclose(log_file);
            return 1;
        }

        Race *race = &processes.shared->race;
        run_error = wait_for_cars(&processes, connection_timeout);
        if (run_error != 0) { errno = run_error; perror("Не удалось дождаться машин"); }
        if (run_error == 0 && !interruption_requested()) {
            start_animation();
            animate_frame(race, NULL, "Стартовая расстановка", 0, 0, 0, 0, NULL);
        }
        while (run_error == 0 && active_count(race) > 0 && race->rounds < config.max_rounds && !interruption_requested()) {
            run_error = run_process_round(&processes);
            if (run_error != 0) {
                errno = run_error;
                perror("Ошибка взаимодействия с процессом машины");
                break;
            }
        }
        // Сохраняем итог до munmap. При аварии незавершённый ход не выдаётся за результат.
        if (run_error == 0) result = *race;
        cleanup_error = stop_processes(&processes, run_error != 0);
        if (cleanup_error != 0) {
            errno = cleanup_error;
            perror("Не удалось освободить ресурсы процессов");
        }
    }
    const char *reason = "все машины финишировали или сошли";
    if (active_count(&result) > 0) {
        reason = interrupted ? "прерывание пользователем" : "достигнут предел раундов";
        for (int i = 0; i < result.car_count; i++) {
            if (result.cars[i].status == ACTIVE) result.cars[i].status = UNFINISHED;
        }
    }
    stop_animation();
    if (run_error == 0) print_results(&result, reason);

    int failed = run_error != 0 || cleanup_error != 0 || ferror(stdout);
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
