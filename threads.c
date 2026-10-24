#define _POSIX_C_SOURCE 200809L
#include "threads.h"
#include "display.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

// У корректно созданных локальных примитивов ошибки во время работы означают
// нарушение протокола. Их нельзя игнорировать и оставлять другой поток ждать.
static void require_success(int error, const char *operation) {
    if (error != 0) {
        errno = error;
        perror(operation);
        stop_animation();
        exit(EXIT_FAILURE);
    }
}

static void lock_state(RaceThreads *threads) {
    require_success(pthread_mutex_lock(&threads->mutex), "pthread_mutex_lock");
}

static void unlock_state(RaceThreads *threads) {
    require_success(pthread_mutex_unlock(&threads->mutex), "pthread_mutex_unlock");
}

static void post_event(sem_t *event) {
    if (sem_post(event) == -1) require_success(errno, "sem_post");
}

static void wait_event(sem_t *event) {
    int result;
    do {
        result = sem_wait(event);
    } while (result == -1 && errno == EINTR);
    if (result == -1) require_success(errno, "sem_wait");
}

static void *car_thread(void *argument) {
    CarWorker *worker = argument;
    RaceThreads *threads = worker->owner;
    for (;;) {
        wait_event(&worker->permission);
        lock_state(threads);
        if (threads->stopping) {
            unlock_state(threads);
            return NULL;
        }
        // Правила допускают ровно одну движущуюся машину. Мьютекс защищает
        // весь ход, включая выбор маршрута, модель, журнал и кадры. Судья
        // в это время ждёт completed; остальные машины ждут свои permission.
        worker->cost = perform_turn(threads->race, worker->car_index, worker->limit);
        worker->turns++;
        worker->last_actor = pthread_self();
        unlock_state(threads);
        post_event(&threads->completed);
    }
}

int stop_workers(RaceThreads *threads) {
    if (threads->created > 0) {
        lock_state(threads);
        threads->stopping = 1;
        unlock_state(threads);
    } else {
        // В том числе при неудаче самого первого pthread_mutex_init:
        // рабочих потоков ещё нет, неинициализированный mutex не трогаем.
        threads->stopping = 1;
    }
    for (int i = 0; i < threads->created; i++) post_event(&threads->workers[i].permission);
    for (int i = 0; i < threads->created; i++) {
        require_success(pthread_join(threads->workers[i].thread, NULL), "pthread_join");
    }
    threads->created = 0;
    // Сначала join: ни один поток уже не обращается к семафорам и мьютексу.
    // После join ошибка одного destroy не мешает попытке очистить остальные.
    int error = 0;
    for (int i = 0; i < MAX_CARS; i++) {
        CarWorker *worker = &threads->workers[i];
        if (!worker->permission_initialized) continue;
        if (sem_destroy(&worker->permission) == -1) {
            if (error == 0) error = errno;
        } else {
            worker->permission_initialized = 0;
            threads->permissions_initialized--;
        }
    }
    if (threads->completed_initialized) {
        if (sem_destroy(&threads->completed) == -1) {
            if (error == 0) error = errno;
        } else {
            threads->completed_initialized = 0;
        }
    }
    if (threads->mutex_initialized) {
        int mutex_error = pthread_mutex_destroy(&threads->mutex);
        if (mutex_error != 0) {
            if (error == 0) error = mutex_error;
        } else {
            threads->mutex_initialized = 0;
        }
    }
    return error;
}

int start_workers(RaceThreads *threads, Race *race) {
    memset(threads, 0, sizeof(*threads));
    threads->race = race;
    int error = pthread_mutex_init(&threads->mutex, NULL);
    if (error != 0) goto failed;
    threads->mutex_initialized = 1;
    if (sem_init(&threads->completed, 0, 0) == -1) {
        error = errno;
        goto failed;
    }
    threads->completed_initialized = 1;
    for (int i = 0; i < race->car_count; i++) {
        CarWorker *worker = &threads->workers[i];
        worker->owner = threads;
        worker->car_index = i;
        if (sem_init(&worker->permission, 0, 0) == -1) {
            error = errno;
            goto failed;
        }
        worker->permission_initialized = 1;
        threads->permissions_initialized++;
        error = pthread_create(&worker->thread, NULL, car_thread, worker);
        if (error != 0) goto failed;
        threads->created++;
    }
    return 0;

failed:;
    int cleanup_error = stop_workers(threads);
    if (cleanup_error != 0) {
        errno = cleanup_error;
        perror("Ошибка освобождения ресурсов после неудачного запуска");
    }
    return error;
}

void run_threaded_round(RaceThreads *threads) {
    lock_state(threads);
    Race *race = threads->race;
    begin_round(race);
    int count = race->order_count;
    unlock_state(threads);

    int previous_cost = 0;
    for (int i = 0; i < count; i++) {
        lock_state(threads);
        CarWorker *worker = &threads->workers[race->order[i]];
        worker->limit = i == 0 ? 4 : previous_cost + (i == 1 ? 2 : 1);
        unlock_state(threads);
        // Судья не держит mutex при ожидании: машина сможет выполнить задание.
        post_event(&worker->permission);
        wait_event(&threads->completed);
        lock_state(threads);
        previous_cost = worker->cost;
        unlock_state(threads);
    }
    lock_state(threads);
    end_round(race);
    unlock_state(threads);
}
