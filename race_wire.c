#include "race_wire.h"
#include <string.h>

enum { VIEW_WORDS = 17, CAR_WORDS = 6 };
_Static_assert(4 * (VIEW_WORDS + MAX_CARS * CAR_WORDS) + MAX_CARS + MAX_STEPS +
               MAX_LANES * MAX_LENGTH + VIEW_TEXT <= TCP_MAX_PAYLOAD, "Снимок гонки превышает предел кадра");

static size_t put_track(unsigned char *body, const Race *race) {
    size_t size = 0;
    for (int lane = 0; lane < race->config.lanes; lane++) {
        for (int cell = 0; cell < race->config.length; cell++) body[size++] = (unsigned char)race->track[lane][cell];
    }
    return size;
}

static int get_track(Race *race, const unsigned char *body, int max_id) {
    for (int lane = 0; lane < race->config.lanes; lane++) {
        for (int cell = 0; cell < race->config.length; cell++) {
            if (*body > max_id) return 0;
            race->track[lane][cell] = *body++;
        }
    }
    return 1;
}

static int track_size_ok(uint32_t lanes, uint32_t length, uint32_t laps) {
    return lanes >= 1 && lanes <= MAX_LANES && length >= 3 && length <= MAX_LENGTH && laps >= 1 && laps <= 100;
}

size_t pack_turn(unsigned char *body, const Race *race, const Car *car, uint32_t turn, int budget) {
    const Config *cfg = &race->config;
    uint32_t fields[TURN_WORDS] = {turn, race->rounds, budget, car->lane, car->position, car->distance,
                                 cfg->lanes, cfg->length, cfg->laps};
    for (size_t i = 0; i < TURN_WORDS; i++) tcp_put_word(body, i, fields[i]);
    return sizeof(fields) + put_track(body + sizeof(fields), race);
}

int unpack_turn(const TcpFrame *frame, Race *race, Car *car, uint32_t *turn, int *budget) {
    if (frame->size < TURN_WORDS * 4) return 0;
    uint32_t words[TURN_WORDS];
    for (size_t i = 0; i < TURN_WORDS; i++) words[i] = tcp_get_word(frame->data, i);
    uint32_t lanes = words[6], length = words[7], laps = words[8];
    int distance = tcp_get_signed(frame->data, 5);
    if (!track_size_ok(lanes, length, laps) || frame->size != sizeof(words) + lanes * length ||
        words[0] == 0 || words[1] == 0 || words[1] > 10000 || words[2] == 0 || words[2] > MAX_STEPS ||
        words[3] >= lanes || words[4] >= length || distance < -MAX_CARS || distance >= (int)(laps * length)) return 0;
    *turn = words[0];
    *budget = (int)words[2];
    race->rounds = (int)words[1];
    race->config.lanes = (int)lanes;
    race->config.length = (int)length;
    race->config.laps = (int)laps;
    car->lane = (int)words[3];
    car->position = (int)words[4];
    car->distance = distance;
    return get_track(race, frame->data + sizeof(words), MAX_CARS) && race->track[car->lane][car->position] == car->id;
}

void pack_view(ViewPacket *packet, const Race *race, const Car *car, const char *event,
               int step, int count, int used, int limit, const Route *route) {
    const Config *cfg = &race->config;
    unsigned char *data = packet->data;
    size_t event_len = strlen(event);
    if (event_len >= VIEW_TEXT) event_len = VIEW_TEXT - 1;
    uint32_t fields[VIEW_WORDS] = {race->rounds, cfg->max_rounds, cfg->teams, cfg->cars_per_team,
        cfg->lanes, cfg->length, cfg->laps, race->finish_count, car ? car->id : 0,
        step, count, used, limit, route ? route->count : 0, race->order_count, (uint32_t)event_len, cfg->delay_ms};
    for (size_t i = 0; i < VIEW_WORDS; i++) tcp_put_word(data, i, fields[i]);
    data += sizeof(fields);
    for (int i = 0; i < race->car_count; i++) {
        const Car *c = &race->cars[i];
        uint32_t values[CAR_WORDS] = {c->id, c->lane, c->position, c->status, c->finish_place, c->distance};
        for (size_t j = 0; j < CAR_WORDS; j++) tcp_put_word(data, j, values[j]);
        data += sizeof(values);
    }
    data += put_track(data, race);
    for (int i = 0; i < race->order_count; i++) *data++ = (unsigned char)race->order[i];
    if (route) for (int i = 0; i < route->count; i++) *data++ = (unsigned char)route->directions[i];
    memcpy(data, event, event_len);
    packet->size = (size_t)(data - packet->data) + event_len;
}

int unpack_view(const TcpFrame *frame, RaceView *view) {
    if (frame->size < VIEW_WORDS * 4) return 0;
    uint32_t w[VIEW_WORDS];
    for (size_t i = 0; i < VIEW_WORDS; i++) w[i] = tcp_get_word(frame->data, i);
    uint32_t teams = w[2], drives = w[3], lanes = w[4], length = w[5], laps = w[6];
    if (teams < 1 || teams > MAX_TEAMS || drives < 1 || drives > MAX_CARS_PER_TEAM ||
        !track_size_ok(lanes, length, laps)) return 0;
    uint32_t cars = teams * drives;
    if (length <= cars || w[1] == 0 || w[1] > 10000 || w[0] > w[1] ||
        w[7] > cars || w[8] > cars || w[9] > w[10] || w[10] > MAX_STEPS ||
        w[11] > w[12] || w[12] > MAX_STEPS || w[13] > MAX_STEPS || w[14] > cars ||
        w[15] >= VIEW_TEXT || w[16] > 5000 ||
        frame->size != sizeof(w) + 4 * CAR_WORDS * cars + lanes * length + w[13] + w[14] + w[15]) return 0;

    *view = (RaceView){.focus = -1, .step = (int)w[9], .count = (int)w[10], .used = (int)w[11], .limit = (int)w[12]};
    Race *race = &view->race;
    race->config = (Config){.teams = (int)teams, .cars_per_team = (int)drives, .lanes = (int)lanes,
                          .length = (int)length, .laps = (int)laps, .max_rounds = (int)w[1], .delay_ms = (int)w[16]};
    race->rounds = (int)w[0];
    race->finish_count = (int)w[7];
    race->car_count = (int)cars;
    const unsigned char *data = frame->data + sizeof(w);
    int seen[MAX_CARS + 1] = {0};
    for (uint32_t i = 0; i < cars; i++, data += 4 * CAR_WORDS) {
        uint32_t id = tcp_get_word(data, 0), lane = tcp_get_word(data, 1), cell = tcp_get_word(data, 2);
        uint32_t status = tcp_get_word(data, 3), place = tcp_get_word(data, 4);
        int distance = tcp_get_signed(data, 5);
        if (id == 0 || id > cars || seen[id] || lane >= lanes || cell >= length || status > UNFINISHED ||
            place > cars || distance < -(int)cars || distance > (int)(length * laps)) return 0;
        seen[id] = 1;
        if (id == w[8]) view->focus = (int)i;
        race->cars[i] = (Car){.id = (int)id, .team = (int)((id - 1) / drives + 1),
            .lane = (int)lane, .position = (int)cell, .status = (int)status, .finish_place = (int)place, .distance = distance};
    }
    if (!get_track(race, data, (int)cars)) return 0;
    data += lanes * length;
    race->order_count = (int)w[14];
    for (uint32_t i = 0; i < w[14]; i++, data++) {
        if (*data >= cars) return 0;
        race->order[i] = *data;
    }
    view->route.count = (int)w[13];
    for (uint32_t i = 0; i < w[13]; i++, data++) {
        if (*data > OUTSIDE) return 0;
        view->route.directions[i] = *data;
    }
    for (uint32_t i = 0; i < w[15]; i++) {
        if (data[i] < 32 || data[i] == 127) return 0;
    }
    memcpy(view->event, data, w[15]);
    return 1;
}
