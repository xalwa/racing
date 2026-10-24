#define _POSIX_C_SOURCE 200809L
#include "../race.h"
#include "../display.h"
#include "../threads.h"
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_int delivered;

static void harmless_signal(int number) {
    (void)number;
    atomic_fetch_add_explicit(&delivered, 1, memory_order_relaxed);
}

static void check_board(const Race *race) {
    int seen[MAX_CARS + 1] = {0};
    for (int lane = 0; lane < race->config.lanes; lane++) {
        for (int cell = 0; cell < race->config.length; cell++) {
            int id = race->track[lane][cell];
            assert(id >= 0 && id <= race->car_count);
            if (!id) continue;
            assert(++seen[id] == 1);
            int found = 0;
            for (int i = 0; i < race->car_count; i++) {
                const Car *car = &race->cars[i];
                if (car->id != id) continue;
                assert(car->lane == lane && car->position == cell && car->status == ACTIVE);
                found = 1;
            }
            assert(found);
        }
    }
    for (int i = 0; i < race->car_count; i++) {
        assert(seen[race->cars[i].id] == (race->cars[i].status == ACTIVE));
    }
}

int main(void) {
    quiet = 1;
    struct sigaction action = {0};
    action.sa_handler = harmless_signal;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1, &action, NULL) == 0);

    int completed_races = 0;
    int checked_turns = 0;
    for (int strategy = GREEDY; strategy <= RANDOM; strategy++) {
        for (int seed = 0; seed < 20; seed++) {
            Config config = {1 + seed % 8, 1 + seed % 4, 1 + seed % 6,
                             33 + seed, 1 + seed % 3, 200, strategy, (unsigned)seed, 1, NULL, 0, 0};
            Race race;
            srand(config.seed);
            initialize_race(&race, config);
            RaceThreads threads;
            assert(start_workers(&threads, &race) == 0);
            assert(threads.created == race.car_count);

            // SIGUSR1 без SA_RESTART прерывает sem_wait у живых участников.
            for (int i = 0; i < threads.created; i++) {
                assert(!pthread_equal(threads.workers[i].thread, pthread_self()));
                assert(pthread_kill(threads.workers[i].thread, SIGUSR1) == 0);
            }
            for (int round = 0; round < config.max_rounds && active_count(&race); round++) {
                Car before[MAX_CARS];
                unsigned int turns[MAX_CARS];
                memcpy(before, race.cars, sizeof(before));
                for (int i = 0; i < race.car_count; i++) turns[i] = threads.workers[i].turns;
                run_threaded_round(&threads);
                check_board(&race);
                int expected[MAX_CARS];
                int count = create_order(&race, expected);
                assert(count == race.order_count);
                for (int i = 0; i < count; i++) assert(expected[i] == race.order[i]);
                for (int i = 0; i < race.car_count; i++) {
                    CarWorker *worker = &threads.workers[i];
                    assert(worker->turns == turns[i] + (before[i].status == ACTIVE));
                    if (before[i].status == ACTIVE) {
                        assert(pthread_equal(worker->last_actor, worker->thread));
                        assert(!pthread_equal(worker->last_actor, pthread_self()));
                        checked_turns++;
                    } else {
                        assert(memcmp(&before[i], &race.cars[i], sizeof(Car)) == 0);
                    }
                }
            }
            assert(active_count(&race) == 0);
            completed_races++;
            assert(stop_workers(&threads) == 0);
            assert(threads.created == 0 && threads.permissions_initialized == 0);
            assert(!threads.mutex_initialized && !threads.completed_initialized);
            assert(stop_workers(&threads) == 0);
        }
    }
    // Выход без единого хода: аналог сигнала после создания рабочих потоков.
    Race race;
    Config config = {8, 4, 2, 33, 2, 100, RANDOM, 0, 1, NULL, 0, 0};
    initialize_race(&race, config);
    RaceThreads threads;
    assert(start_workers(&threads, &race) == 0);
    assert(stop_workers(&threads) == 0);
    assert(stop_workers(&threads) == 0);
    assert(atomic_load(&delivered) > 0);
    printf("Потоки C: %d гонок, %d ходов в потоках машин, неизменность выбывших, EINTR и остановка проверены.\n",
           completed_races, checked_turns);
    return 0;
}
