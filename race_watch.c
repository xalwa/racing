#define _POSIX_C_SOURCE 200809L
#include "display.h"
#include "race_wire.h"
#include "tcp_client.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void show_view(const RaceView *view) {
    const Race *race = &view->race;
    const Car *car = view->focus < 0 ? NULL : &race->cars[view->focus];
    if (animation) {
        draw_frame(race, car, view->event, view->step, view->count, view->used, view->limit, &view->route);
        return;
    }
    printf("Раунд %d. %s\n", race->rounds, view->event);
    if (car) {
        printf("Машина %d, дорожка %d, клетка %d. Шаг %d/%d, потрачено %d из %d.\n",
               car->id, car->lane + 1, car->position + 1, view->step, view->count, view->used, view->limit);
    }
    print_positions(race);
}

static int watch(int fd) {
    if (!client_send(fd, TCP_WATCH, NULL, 0)) return -1;
    TcpBuffer input = {0};
    TcpFrame frame;
    while (client_receive(fd, &input, &frame) == TCP_FRAME) {
        if (frame.type != TCP_VIEW && frame.type != TCP_STANDINGS) {
            stop_animation();
            return client_finished(&frame);
        }
        RaceView view;
        if (!unpack_view(&frame, &view)) return -1;
        if (frame.type == TCP_STANDINGS) {
            stop_animation();
            print_results(&view.race, view.event);
            return 0;
        }
        show_view(&view);
    }
    return -1;
}

int main(int argc, char **argv) {
    int help = argc == 2 && !strcmp(argv[1], "--help");
    if (help || argc < 3 || argc > 4 || (argc == 4 && strcmp(argv[3], "--no-animation"))) {
        fprintf(help ? stdout : stderr, "Наблюдатель TCP: %s IP PORT [--no-animation]\n", argv[0]);
        return help ? 0 : 1;
    }
    int fd = client_open(argv[1], argv[2]);
    if (fd < 0) {
        if (!interruption_requested()) perror("Подключение к судье");
        return client_exit(1);
    }
    const char *term = getenv("TERM");
    animation = argc == 3 && isatty(STDOUT_FILENO) && (!term || strcmp(term, "dumb"));
    start_animation();
    int result = watch(fd);
    stop_animation();
    close(fd);
    return client_exit(result);
}
