#include "display.h"
#include "race_wire.h"
#include "tcp_client.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int drive(int fd, uint32_t id) {
    unsigned char body[8 + MAX_STEPS];
    tcp_put_word(body, 0, id);
    if (!client_send(fd, TCP_HELLO, body, 4)) return -1;
    TcpBuffer input = {0};
    TcpFrame frame;
    if (client_receive(fd, &input, &frame) != TCP_FRAME) return -1;
    if (frame.type != TCP_WELCOME) return client_finished(&frame);
    if ((frame.size != 12 && frame.size != 16) || tcp_get_word(frame.data, 0) != id ||
        tcp_get_word(frame.data, 2) > RANDOM) return -1;
    Race race = {0};
    Car car = {.id = (int)id, .status = ACTIVE};
    race.config.strategy = (int)tcp_get_word(frame.data, 2);
    srand(tcp_get_word(frame.data, 1) + id);
    uint32_t draws = frame.size == 16 ? tcp_get_word(frame.data, 3) : 0;
    if (draws > 10000 * MAX_STEPS) return -1;
    for (uint32_t i = 0; i < draws; i++) rand();
    printf("Машина %u подключена.\n", (unsigned int)id);

    uint32_t previous = 0;
    while (client_receive(fd, &input, &frame) == TCP_FRAME) {
        if (frame.type == TCP_TURN) {
            uint32_t turn;
            int budget, blocked;
            if (!unpack_turn(&frame, &race, &car, &turn, &budget) || turn <= previous) return -1;
            Route route = choose_route(&race, &car, budget, &blocked);
            tcp_put_word(body, 0, turn);
            tcp_put_word(body, 1, (uint32_t)route.count);
            for (int i = 0; i < route.count; i++) body[8 + i] = (unsigned char)route.directions[i];
            printf("Ход %u, раунд %d, бюджет %d. Маршрут:", (unsigned int)turn, race.rounds, budget);
            static const char *steps[] = {"вперёд", "внутрь", "наружу"};
            for (int i = 0; i < route.count; i++) printf(" %s", steps[route.directions[i]]);
            if (!route.count) printf(" нулевой ход");
            printf(". Стоимость %d.\n", route.cost);
            if (!client_send(fd, TCP_MOVE, body, 8 + (size_t)route.count)) return -1;
            previous = turn;
        } else if (frame.type == TCP_RESULT) {
            if (frame.size != 24) return -1;
            uint32_t reason = tcp_get_word(frame.data, 0), status = tcp_get_word(frame.data, 4);
            if ((reason != TCP_COMPLETED && reason != TCP_ROUND_LIMIT) || status < FINISHED || status > UNFINISHED) return -1;
            printf("Итог машины %u: %s, место %u, место команды %u, продвижение %d. Раундов: %u.\n",
                   (unsigned int)id, status_name((int)status), (unsigned int)tcp_get_word(frame.data, 2),
                   (unsigned int)tcp_get_word(frame.data, 3), (int)tcp_get_signed(frame.data, 5),
                   (unsigned int)tcp_get_word(frame.data, 1));
            return 0;
        } else {
            return client_finished(&frame);
        }
    }
    return -1;
}

int main(int argc, char **argv) {
    uint32_t id;
    int help = argc == 2 && !strcmp(argv[1], "--help");
    if (help || argc != 4 || !tcp_number(argv[3], 1, MAX_CARS, &id)) {
        fprintf(help ? stdout : stderr, "Машина TCP: %s IP PORT ID\n", argv[0]);
        return help ? 0 : 1;
    }
    int fd = client_open(argv[1], argv[2]);
    if (fd < 0) {
        if (!interruption_requested()) perror("Подключение к судье");
        return client_exit(1);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    int result = drive(fd, id);
    close(fd);
    return client_exit(result);
}
