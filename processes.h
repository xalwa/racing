#ifndef PROCESSES_H
#define PROCESSES_H

#include "race.h"
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/types.h>

enum { UNNAMED, NAMED, INDEPENDENT, QUEUED };
typedef struct {
    int car, step, count, used, limit, has_route;
    Route route;
    char event[192];
} Frame;

// В общей памяти нет локальных указателей и файловых дескрипторов.
typedef struct {
    Race race;
    sem_t ready, consumed, permission[MAX_CARS];
    atomic_int stage, requested_signal, cleanup_claim, registered[MAX_CARS];
    pid_t judge;
    uint64_t generation;
    int limit, cost, event, detail, animate, relay_details;
    char text[512];
    Frame frame;
} SharedRace;

typedef struct {
    SharedRace *shared;
    sem_t *semaphores[MAX_CARS + 2];
    pid_t children[MAX_CARS];
    int pidfds[MAX_CARS], judge_fd;
    unsigned char initialized[MAX_CARS + 2], linked[MAX_CARS + 2];
    char name[64];
    dev_t device;
    ino_t inode;
    int created, mode, owner, owns_name;
} RaceProcesses;

// 0 — успех, иначе errno; при ошибке запуска выполняется откат.
int start_processes(RaceProcesses *, const Race *);
int start_race(RaceProcesses *, const Race *, int mode, const char *session);
int wait_for_cars(RaceProcesses *, int seconds);
int run_process_round(RaceProcesses *);
int stop_processes(RaceProcesses *, int aborting);
// Отдельная программа машины открывает существующую сессию, не создавая судью.
int run_independent_car(const char *session, int id, int seconds);

#endif
