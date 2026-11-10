#define _POSIX_C_SOURCE 200809L
#include "processes.h"
#include "display.h"
#include "queues.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUEUE_WORKER
#define QUEUE_WORKER 0
#endif

static int number(const char *text, int maximum) {
    if (*text < '0' || *text > '9') return 0;
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    return errno == 0 && *end == '\0' && value > 0 && value <= maximum ? (int)value : 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Машина: %s SESSION ID [WAIT_SECONDS]\n"
             "ID: 1..32; ожидание судьи: 1..60 секунд (по умолчанию 10).\n"
             "Ctrl+C или SIGTERM запрашивает остановку всей гонки.\n", argv[0]);
        return 0;
    }
    int id = argc >= 3 ? number(argv[2], MAX_CARS) : 0;
    int seconds = argc == 4 ? number(argv[3], 60) : 10;
    if ((argc != 3 && argc != 4) || id == 0 || seconds == 0) {
        fprintf(stderr, "Использование: %s SESSION ID [WAIT_SECONDS]\n", argv[0]);
        return 1;
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_interrupt;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 || sigaction(SIGTERM, &action, NULL) == -1) {
        perror("Обработчик сигнала"); return 1;
    }
    int error = QUEUE_WORKER ? run_queue_car(argv[1], id, seconds) : run_independent_car(argv[1], id, seconds);
    if (error != 0 && error != EINTR) { errno = error; perror("Не удалось выполнить роль машины"); }
    return interrupted ? 128 + interrupted : error == 0 ? 0 : 1;
}
