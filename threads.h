#ifndef THREADS_H
#define THREADS_H

#include "race.h"
#include <pthread.h>
#include <semaphore.h>

typedef struct RaceThreads RaceThreads;

typedef struct {
    RaceThreads *owner;
    int car_index;
    pthread_t thread;
    sem_t permission;
    int permission_initialized; // Учёт ресурсов принадлежит только судье.
    int limit;
    int cost;
    unsigned int turns;
    pthread_t last_actor;
} CarWorker;

struct RaceThreads {
    Race *race;
    pthread_mutex_t mutex;
    sem_t completed;
    CarWorker workers[MAX_CARS];
    int created;
    int permissions_initialized;
    int mutex_initialized;
    int completed_initialized;
    int stopping;
};

// Возвращает код ошибки POSIX. При неудаче останавливает созданные потоки
// и пытается освободить все созданные ресурсы; ошибки очистки выводит отдельно.
int start_workers(RaceThreads *threads, Race *race);
void run_threaded_round(RaceThreads *threads);
// Вызывается, когда нет задания в работе. Повторная успешная очистка безопасна.
// Возвращает первую ошибку освобождения, проверяя и остальные ресурсы.
int stop_workers(RaceThreads *threads);

#endif
