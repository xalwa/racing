#define _GNU_SOURCE
#include "display.h"
#include "race_wire.h"
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { MAX_WATCHERS = 16, MAX_PEERS = MAX_CARS + MAX_WATCHERS + 8, HISTORY = 128 };
enum { JOINING, RUNNING, DRAINING };
_Static_assert(HISTORY > MAX_CARS + MAX_STEPS + 2, "История должна вмещать все кадры хода или раунда");

typedef struct {
    int fd, car_id, watching, closing;
    int64_t hello_deadline;
    uint64_t next_view;
    TcpBuffer input, output;
} Peer;

typedef struct {
    Race race;
    Peer peers[MAX_PEERS];
    int car_index[MAX_CARS + 1], places[MAX_CARS + 1], team_places[MAX_TEAMS + 1];
    uint32_t draws[MAX_CARS + 1], last_move[MAX_CARS + 1], turn;
    int phase, result, reason, round_open, awaiting, paused, next, previous_cost, budget;
    int turn_timeout, reconnect_timeout;
    int64_t deadline, frame_at;
    uint64_t produced, published;
    ViewPacket history[HISTORY], standings;
} Server;

static Server server;

static void capture(const Race *race, const Car *car, const char *event,
                    int step, int count, int used, int limit, const Route *route) {
    ViewPacket *packet = &server.history[++server.produced % HISTORY];
    pack_view(packet, race, car, event, step, count, used, limit, route);
}

static void close_peer(Peer *peer) {
    close(peer->fd);
    peer->fd = -1;
}

static Peer *find_car(Server *s, int id) {
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *peer = &s->peers[i];
        if (peer->fd >= 0 && peer->car_id == id) return peer;
    }
    return NULL;
}

static Car *current_car(Server *s) {
    return &s->race.cars[s->race.order[s->next]];
}

static void pause_turn(Server *s) {
    s->paused = 1;
    s->deadline = tcp_now_ms() + 1000 * s->reconnect_timeout;
    printf("Ход %u приостановлен. Ожидается машина %d.\n", (unsigned int)s->turn, current_car(s)->id);
}

static void disconnected(Server *s, Peer *peer) {
    int id = peer->car_id;
    close_peer(peer);
    if (!id || s->phase == DRAINING) return;
    fprintf(stderr, "Машина %d потеряла соединение.\n", id);
    const Car *car = &s->race.cars[s->car_index[id]];
    if (s->phase != RUNNING || car->status != ACTIVE || car->zero_move) return;
    if (!s->reconnect_timeout) s->result = 1;
    else if (s->awaiting && !s->paused && current_car(s)->id == id) pause_turn(s);
}

static int queue_code(Peer *peer, unsigned char type, unsigned int code) {
    unsigned char body[4];
    tcp_put_word(body, 0, code);
    return tcp_queue(&peer->output, type, body, sizeof(body));
}

static void reject(Server *s, Peer *peer, unsigned int code) {
    fprintf(stderr, "Сообщение отклонено (код %u, ID машины %d).\n", code, peer->car_id);
    if (peer->car_id && s->phase == RUNNING) s->result = 1;
    peer->car_id = 0;
    peer->watching = 0;
    if (queue_code(peer, TCP_ERROR, code)) peer->closing = 1;
    else close_peer(peer);
}

static void register_peer(Server *s, Peer *peer, const TcpFrame *frame) {
    if (frame->type == TCP_WATCH && frame->size == 0) {
        int viewers = 0;
        for (int i = 0; i < MAX_PEERS; i++) viewers += s->peers[i].fd >= 0 && s->peers[i].watching;
        if (viewers == MAX_WATCHERS) { reject(s, peer, TCP_WATCH_FULL); return; }
        peer->watching = 1;
        // Новому зрителю достаточно уже опубликованного кадра.
        peer->next_view = s->published ? s->published : 1;
        puts("Наблюдатель подключился.");
        return;
    }
    if (frame->type != TCP_HELLO || frame->size != 4) {
        reject(s, peer, TCP_BAD_MESSAGE);
        return;
    }
    uint32_t id = tcp_get_word(frame->data, 0);
    int returning = s->phase == RUNNING;
    if (id == 0 || id > (uint32_t)s->race.car_count) reject(s, peer, TCP_BAD_ID);
    else if (returning && !s->reconnect_timeout) reject(s, peer, TCP_ALREADY_STARTED);
    else if (find_car(s, (int)id)) reject(s, peer, TCP_ID_BUSY);
    else {
        unsigned char body[16];
        tcp_put_word(body, 0, id);
        tcp_put_word(body, 1, s->race.config.seed);
        tcp_put_word(body, 2, (uint32_t)s->race.config.strategy);
        tcp_put_word(body, 3, s->draws[id]);
        if (!tcp_queue(&peer->output, TCP_WELCOME, body, returning ? 16 : 12)) { close_peer(peer); return; }
        peer->car_id = (int)id;
        printf("Машина %u %s.\n", (unsigned int)id, returning ? "переподключилась" : "подключилась");
    }
}

static void receive_move(Server *s, Peer *peer, const TcpFrame *frame) {
    if (frame->type != TCP_MOVE || frame->size < 8) {
        reject(s, peer, TCP_BAD_MESSAGE);
        return;
    }
    uint32_t turn = tcp_get_word(frame->data, 0);
    uint32_t count = tcp_get_word(frame->data, 1);
    if (count > MAX_STEPS || frame->size != 8 + count) {
        reject(s, peer, TCP_BAD_MESSAGE);
        return;
    }
    if (turn && s->last_move[peer->car_id] == turn) return;
    if (!s->awaiting || s->paused || peer->car_id != current_car(s)->id || turn != s->turn) {
        reject(s, peer, TCP_BAD_TURN);
        return;
    }
    Route route = {.count = (int)count};
    for (size_t i = 0; i < count; i++) route.directions[i] = frame->data[8 + i];
    int blocked;
    if (!validate_route(&s->race, current_car(s), s->budget, &route, &blocked)) {
        reject(s, peer, TCP_BAD_ROUTE);
        return;
    }
    s->previous_cost = apply_turn(&s->race, s->race.order[s->next], s->budget, &route, blocked);
    s->last_move[peer->car_id] = turn;
    if (s->race.config.strategy == RANDOM) s->draws[peer->car_id] += count;
    printf("Ход %u получен от машины %d. Шагов: %d, стоимость: %d.\n",
           (unsigned int)s->turn, peer->car_id, route.count, route.cost);
    s->next++;
    s->awaiting = 0;
    s->deadline = 0;
}

static void prepare_output(Server *s, Peer *peer);

static void peer_events(Server *s, Peer *peer, short events) {
    if (events & POLLOUT) {
        for (int i = 0; i < 64 && peer->output.size; i++) {
            int status = tcp_flush(peer->fd, &peer->output);
            if (status == TCP_IO_ERROR) { disconnected(s, peer); return; }
            if (status == TCP_WAIT) break;
            if (peer->closing) { close_peer(peer); return; }
            if (!peer->watching) break;
            prepare_output(s, peer);
            if (peer->fd < 0) return;
        }
    }
    if (!(events & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) return;
    if (peer->closing || s->phase == DRAINING) { disconnected(s, peer); return; }
    TcpFrame frame;
    switch (tcp_receive(peer->fd, &peer->input, &frame)) {
        case TCP_FRAME:
            if (peer->car_id) receive_move(s, peer, &frame);
            else if (peer->watching) reject(s, peer, TCP_BAD_MESSAGE);
            else register_peer(s, peer, &frame);
            break;
        case TCP_BAD_FRAME: reject(s, peer, TCP_BAD_MESSAGE); break;
        case TCP_IO_ERROR:
        case TCP_EOF: disconnected(s, peer); break;
        default:
            if (events & (POLLERR | POLLHUP | POLLNVAL)) disconnected(s, peer);
            break;
    }
}

static int cars_ready(const Server *s) {
    int ready = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        const Peer *peer = &s->peers[i];
        ready += peer->fd >= 0 && peer->car_id && !peer->output.size;
    }
    return ready == s->race.car_count;
}

static void send_turn(Server *s, Peer *peer) {
    unsigned char body[TCP_MAX_PAYLOAD];
    size_t size = pack_turn(body, &s->race, current_car(s), s->turn, s->budget);
    if (!tcp_queue(&peer->output, TCP_TURN, body, size)) { s->result = 1; return; }
    printf("Ход %u %s машине %d. Раунд: %d, бюджет: %d.\n", (unsigned int)s->turn,
           s->paused ? "выдан повторно" : "назначен", peer->car_id, s->race.rounds, s->budget);
    s->paused = 0;
    s->deadline = tcp_now_ms() + 1000 * s->turn_timeout;
}

static void start_draining(Server *s, int reason) {
    s->phase = DRAINING;
    s->reason = reason;
    s->deadline = tcp_now_ms() + 2000;
    if (reason == TCP_INTERRUPTED || reason == TCP_ABORTED) return;
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *peer = &s->peers[i];
        if (peer->fd >= 0 && !peer->closing && !peer->car_id && !peer->watching) close_peer(peer);
    }
}

static void finish_race(Server *s, int reason) {
    Race *race = &s->race;
    for (int i = 0; i < race->car_count; i++) {
        if (race->cars[i].status == ACTIVE) race->cars[i].status = UNFINISHED;
    }
    int order[MAX_CARS], teams = 0;
    create_results(race, order);
    for (int i = 0; i < race->car_count; i++) {
        const Car *car = &race->cars[order[i]];
        s->places[car->id] = i + 1;
        if (!s->team_places[car->team]) s->team_places[car->team] = ++teams;
    }
    pack_view(&s->standings, race, NULL, reason == TCP_COMPLETED ?
              "все машины финишировали или сошли" : "достигнут предел раундов", 0, 0, 0, 0, NULL);
    start_draining(s, reason);
}

static void prepare_output(Server *s, Peer *peer) {
    if (peer->closing) return;
    int stopping = s->reason == TCP_INTERRUPTED || s->reason == TCP_ABORTED;
    if (!stopping && peer->watching && peer->next_view <= s->published &&
        s->produced - peer->next_view >= HISTORY) {
        fputs("Кадры наблюдателя отстали, соединение закрыто.\n", stderr);
        close_peer(peer);
        return;
    }
    if (peer->output.size) return;
    if (!stopping && peer->watching && peer->next_view <= s->published) {
        const ViewPacket *packet = &s->history[peer->next_view++ % HISTORY];
        tcp_queue(&peer->output, TCP_VIEW, packet->data, packet->size);
        return;
    }
    if (s->phase != DRAINING) return;
    if (stopping) {
        queue_code(peer, TCP_STOP, (unsigned int)s->reason);
    } else if (peer->watching) {
        tcp_queue(&peer->output, TCP_STANDINGS, s->standings.data, s->standings.size);
    } else {
        const Car *car = &s->race.cars[s->car_index[peer->car_id]];
        uint32_t fields[] = {s->reason, s->race.rounds, s->places[car->id],
                             s->team_places[car->team], car->status, car->distance};
        unsigned char body[sizeof(fields)];
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) tcp_put_word(body, i, fields[i]);
        tcp_queue(&peer->output, TCP_RESULT, body, sizeof(body));
    }
    peer->closing = 1;
}

static void advance(Server *s) {
    if (s->phase == JOINING) {
        if (!cars_ready(s)) return;
        s->phase = RUNNING;
        s->deadline = 0;
    }
    if (s->awaiting) {
        if (s->paused) {
            Peer *peer = find_car(s, current_car(s)->id);
            if (peer && !peer->output.size) send_turn(s, peer);
        }
        return;
    }
    if (s->published != s->produced) return;
    Race *race = &s->race;
    if (!s->round_open) {
        if (!active_count(race) || race->rounds >= race->config.max_rounds) {
            finish_race(s, active_count(race) ? TCP_ROUND_LIMIT : TCP_COMPLETED);
            return;
        }
        begin_round(race);
        s->round_open = 1;
        s->next = 0;
        return;
    }
    if (s->next == race->order_count) {
        end_round(race);
        s->round_open = 0;
        return;
    }
    s->budget = s->next == 0 ? 4 : s->previous_cost + (s->next == 1 ? 2 : 1);
    s->turn++;
    s->awaiting = 1;
    Peer *peer = find_car(s, current_car(s)->id);
    if (peer && !peer->output.size) send_turn(s, peer);
    else if (s->reconnect_timeout) pause_turn(s);
    else s->result = 1;
}

static int accept_peer(Server *s, int listener) {
    int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s->peers[i].fd == -1) {
            int enabled = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
            s->peers[i] = (Peer){.fd = fd, .hello_deadline = tcp_now_ms() + 5000};
            return 1;
        }
    }
    close(fd);
    return 1;
}

static void handle_timeout(Server *s, int64_t now) {
    if (!s->deadline || now < s->deadline) return;
    if (s->phase == JOINING) fputs("Истекло время ожидания подключения машин.\n", stderr);
    else if (s->paused) fputs("Истекло время ожидания переподключения машины.\n", stderr);
    else {
        fputs("Истекло время ожидания ответа на ход.\n", stderr);
        Peer *peer = find_car(s, current_car(s)->id);
        if (peer) disconnected(s, peer);
        if (s->paused) return;
    }
    s->result = 1;
}

static int run_server(Server *s, int listener) {
    for (;;) {
        int64_t now = tcp_now_ms();
        if (s->phase != DRAINING) {
            int signal_number = interruption_requested();
            if (signal_number) {
                s->result = 128 + signal_number;
                start_draining(s, TCP_INTERRUPTED);
            } else {
                handle_timeout(s, now);
                while (s->published < s->produced && now >= s->frame_at) {
                    s->published++;
                    s->frame_at = now + s->race.config.delay_ms;
                }
                if (!s->result) advance(s);
                if (s->result) start_draining(s, TCP_ABORTED);
            }
        }
        struct pollfd fds[MAX_PEERS + 1];
        fds[0] = (struct pollfd){.fd = s->phase == DRAINING ? -1 : listener, .events = POLLIN};
        int clients = 0;
        for (int i = 0; i < MAX_PEERS; i++) {
            Peer *peer = &s->peers[i];
            if (s->phase != DRAINING && peer->fd >= 0 && !peer->car_id && !peer->watching && !peer->closing &&
                now >= peer->hello_deadline) close_peer(peer);
            if (peer->fd >= 0) prepare_output(s, peer);
            short events = peer->closing || s->phase == DRAINING ? 0 : POLLIN;
            if (peer->output.size) events |= POLLOUT;
            fds[i + 1] = (struct pollfd){.fd = peer->fd, .events = events};
            clients += peer->fd >= 0;
        }
        if (s->phase == DRAINING && (clients == 0 || now >= s->deadline)) return s->result;
        int64_t wake = now + 200;
        if (s->deadline && s->deadline < wake) wake = s->deadline;
        if (s->phase != DRAINING && s->published < s->produced && s->frame_at < wake) wake = s->frame_at;
        int ready = poll(fds, MAX_PEERS + 1, wake > now ? (int)(wake - now) : 0);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { perror("poll"); return 1; }
        if (ready == 0) continue;
        for (int i = 0; i < MAX_PEERS; i++) {
            if (s->result && s->phase != DRAINING) break;
            if (fds[i + 1].revents) peer_events(s, &s->peers[i], fds[i + 1].revents);
        }
        if ((!s->result && (fds[0].revents & POLLIN) && !accept_peer(s, listener)) ||
            (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL))) {
            fputs("Ошибка приёма TCP-подключений.\n", stderr);
            s->result = 1;
        }
    }
}

static int open_listener(const char *host, const char *port) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV};
    struct addrinfo *addr;
    int status = getaddrinfo(host, port, &hints, &addr);
    if (status != 0) {
        fprintf(stderr, "Неверный IP-адрес или порт: %s.\n", gai_strerror(status));
        return -1;
    }
    int fd = socket(addr->ai_family, addr->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, addr->ai_protocol);
    if (fd < 0) {
        perror("socket");
    } else {
        int enabled = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
            bind(fd, addr->ai_addr, addr->ai_addrlen) < 0 || listen(fd, MAX_PEERS) < 0) {
            perror("Не удалось открыть TCP-сервер");
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(addr);
    return fd;
}

static int parse_options(int argc, char **argv, Config *config, int *join, int *turn, int *reconnect) {
    struct { const char *name; int *value; uint32_t min, max; } options[] = {
        {"--teams", &config->teams, 1, MAX_TEAMS}, {"--cars", &config->cars_per_team, 1, MAX_CARS_PER_TEAM},
        {"--lanes", &config->lanes, 1, MAX_LANES}, {"--length", &config->length, 3, MAX_LENGTH},
        {"--laps", &config->laps, 1, 100}, {"--rounds", &config->max_rounds, 1, 10000},
        {"--wait", join, 1, 60}, {"--turn-timeout", turn, 1, 60},
        {"--reconnect", reconnect, 0, 60}, {"--delay", &config->delay_ms, 0, 5000}
    };
    uint32_t number;
    if (argc < 3 || !tcp_number(argv[2], 1, 65535, &number) || (argc - 3) % 2) return 0;
    for (int i = 3; i < argc; i += 2) {
        const char *key = argv[i], *value = argv[i + 1];
        if (!strcmp(key, "--strategy")) {
            if (!strcmp(value, "greedy")) config->strategy = GREEDY;
            else if (!strcmp(value, "random")) config->strategy = RANDOM;
            else return 0;
        } else if (!strcmp(key, "--seed")) {
            if (!tcp_number(value, 0, UINT32_MAX, &number)) return 0;
            config->seed = number;
        } else {
            size_t j = 0, count = sizeof(options) / sizeof(options[0]);
            while (j < count && strcmp(key, options[j].name)) j++;
            if (j == count || !tcp_number(value, options[j].min, options[j].max, &number)) return 0;
            *options[j].value = (int)number;
        }
    }
    return config->length > config->teams * config->cars_per_team;
}

static void usage(FILE *stream, const char *name) {
    fprintf(stream, "Судья TCP (ИДЗ 4): %s IP PORT [параметры]\n"
        "  --teams 1..8  --cars 1..4  --lanes 1..6  --length 3..120\n"
        "  --laps 1..100  --rounds 1..10000  --seed N  --strategy greedy|random\n"
        "  --wait 1..60  --turn-timeout 1..60 (секунды, по умолчанию 10)\n"
        "  --delay 0..5000 мс на кадр (по умолчанию 120)\n"
        "  --reconnect 0..60 с на возврат машины (0 отключает, по умолчанию 0)\n"
        "Длина дорожки должна быть больше общего числа машин.\n", name);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage(stdout, argv[0]); return 0; }
    Config config = {.teams = 3, .cars_per_team = 2, .lanes = 4, .length = 24,
        .laps = 2, .max_rounds = 100, .strategy = GREEDY, .seed = 1, .delay_ms = 120};
    int join = 10, turn = 10, reconnect = 0;
    if (!parse_options(argc, argv, &config, &join, &turn, &reconnect)) { usage(stderr, argv[0]); return 1; }
    struct sigaction action = {.sa_handler = handle_interrupt};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) {
        perror("sigaction");
        return 1;
    }
    int listener = open_listener(argv[1], argv[2]);
    if (listener < 0) return 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    quiet = 1; // Общий экран трассы будет у клиента-наблюдателя.
    srand(config.seed);
    server.turn_timeout = turn;
    server.reconnect_timeout = reconnect;
    server.deadline = tcp_now_ms() + 1000 * join;
    initialize_race(&server.race, config);
    for (int i = 0; i < server.race.car_count; i++) server.car_index[server.race.cars[i].id] = i;
    frame_sink = capture;
    capture(&server.race, NULL, "Стартовая расстановка", 0, 0, 0, 0, NULL);
    for (int i = 0; i < MAX_PEERS; i++) server.peers[i].fd = -1;
    printf("Судья слушает %s, порт %s. Ожидается машин: %d.\n", argv[1], argv[2], server.race.car_count);
    int result = run_server(&server, listener);
    close(listener);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (server.peers[i].fd >= 0) close_peer(&server.peers[i]);
    }
    return result;
}
