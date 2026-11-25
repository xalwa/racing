#ifndef RACE_WIRE_H
#define RACE_WIRE_H

#include "race.h"
#include "tcp.h"

enum { TURN_WORDS = 9, VIEW_TEXT = 192 };

typedef struct {
    unsigned char data[TCP_MAX_PAYLOAD];
    size_t size;
} ViewPacket;

typedef struct {
    Race race;
    Route route;
    int focus, step, count, used, limit;
    char event[VIEW_TEXT];
} RaceView;

size_t pack_turn(unsigned char *body, const Race *race, const Car *car, uint32_t turn, int budget);
int unpack_turn(const TcpFrame *frame, Race *race, Car *car, uint32_t *turn, int *budget);
void pack_view(ViewPacket *packet, const Race *race, const Car *car, const char *event,
               int step, int count, int used, int limit, const Route *route);
int unpack_view(const TcpFrame *frame, RaceView *view);

#endif
