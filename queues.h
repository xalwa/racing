#ifndef QUEUES_H
#define QUEUES_H
#include "race.h"
int run_queue_judge(Race *race, const char *session, int seconds);
int run_queue_car(const char *session, int id, int seconds);
#endif
