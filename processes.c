#define _GNU_SOURCE
#include "processes.h"
#include "display.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { TEXT, DONE, FRAME };
enum { INITIALIZING, JOINING, RUNNING, STOPPING };
static RaceProcesses *worker;

static void setup(RaceProcesses *p, int mode) {
    memset(p, 0, sizeof(*p));
    p->mode = mode;
    p->judge_fd = -1;
    for (int i = 0; i < MAX_CARS; i++) p->pidfds[i] = -1;
}

static int session_name(char *name, size_t size, const char *session) {
    if (session == NULL || *session == '\0' || strlen(session) > 32) return EINVAL;
    for (const char *s = session; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return EINVAL;
    }
    snprintf(name, size, "/akos-session-%s", session);
    return 0;
}

static void semaphore_name(const RaceProcesses *p, int i, char name[128]) {
    snprintf(name, 128, "%s-%llx-%d", p->name,
             (unsigned long long)p->shared->generation, i);
}

static int open_semaphores(RaceProcesses *p, int create) {
    for (int i = 0; i < p->shared->race.car_count + 2; i++) {
        if (p->mode == UNNAMED) {
            sem_t *s = i == 0 ? &p->shared->ready : i == 1 ? &p->shared->consumed : &p->shared->permission[i - 2];
            if (sem_init(s, 1, 0) == -1) return errno;
            p->semaphores[i] = s;
        } else {
            char name[128];
            semaphore_name(p, i, name);
            sem_t *s = create ? sem_open(name, O_CREAT | O_EXCL, 0600, 0) : sem_open(name, 0);
            if (s == SEM_FAILED) return errno;
            p->semaphores[i] = s;
            p->linked[i] = create;
        }
        p->initialized[i] = 1;
    }
    return 0;
}

static int unlink_semaphores(RaceProcesses *p) {
    int error = 0;
    for (int i = 0; i < MAX_CARS + 2; i++) {
        if (!p->linked[i]) continue;
        char name[128];
        semaphore_name(p, i, name);
        if (sem_unlink(name) == -1 && errno != ENOENT) {
            if (error == 0) error = errno;
        } else p->linked[i] = 0;
    }
    return error;
}

static int process_exited(int fd) {
    struct pollfd event = {fd, POLLIN, 0};
    int result = poll(&event, 1, 0);
    return result == -1 ? (errno == EINTR ? 0 : -1) : result;
}

static int open_pid(pid_t pid) {
    return (int)syscall(SYS_pidfd_open, pid, 0);
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int wait_event(sem_t *event) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) == -1) return errno;
    deadline.tv_nsec += 100000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_nsec -= 1000000000L; deadline.tv_sec++; }
    return sem_timedwait(event, &deadline) == 0 ? 0 : errno;
}

// Единственный уцелевший участник удаляет имена после аварии судьи.
// Проверка inode не позволяет удалить новую гонку с тем же именем сессии.
static void emergency_unlink(RaceProcesses *p) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&p->shared->cleanup_claim, &expected, 1)) return;
    int file = shm_open(p->name, O_RDONLY, 0);
    struct stat info;
    int same = file >= 0 && fstat(file, &info) == 0 && info.st_ino == p->inode && info.st_dev == p->device;
    if (file >= 0) close(file);
    if (!same) return;
    for (int i = 0; i < p->shared->race.car_count + 2; i++) p->linked[i] = 1;
    unlink_semaphores(p);
    shm_unlink(p->name);
}

static void worker_wait(int index) {
    for (;;) {
        if (interrupted) atomic_store(&worker->shared->requested_signal, atomic_load(&interrupted));
        int error = wait_event(worker->semaphores[index]);
        if (error == 0) return;
        if (atomic_load(&worker->shared->stage) == STOPPING && index >= 2) return;
        int gone = worker->mode == INDEPENDENT ? process_exited(worker->judge_fd) : getppid() != worker->shared->judge;
        if (gone && worker->mode == INDEPENDENT) emergency_unlink(worker);
        if ((error != EINTR && error != ETIMEDOUT) || gone) _exit(1);
    }
}

static void publish(int event) {
    worker->shared->event = event;
    if (sem_post(worker->semaphores[0]) == -1) _exit(1);
    worker_wait(1);
}

static void child_output(int detail, const char *format, va_list args) {
    if (detail && !worker->shared->relay_details) return;
    SharedRace *s = worker->shared;
    int size = vsnprintf(s->text, sizeof(s->text), format, args);
    if (size < 0 || (size_t)size >= sizeof(s->text)) _exit(1);
    s->detail = detail;
    publish(TEXT);
}

static void child_frame(const Race *race, const Car *car, const char *event,
                        int step, int count, int used, int limit, const Route *route) {
    if (!worker->shared->animate) return;
    Frame *f = &worker->shared->frame;
    f->car = car == NULL ? -1 : (int)(car - race->cars);
    f->step = step; f->count = count; f->used = used; f->limit = limit;
    f->has_route = route != NULL;
    if (route != NULL) f->route = *route;
    snprintf(f->event, sizeof(f->event), "%s", event);
    publish(FRAME);
}

static void car_process(RaceProcesses *p, int index) {
    worker = p;
    if (p->mode != INDEPENDENT &&
        (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1 || getppid() != p->shared->judge)) _exit(1);
    if (log_file != NULL) close(fileno(log_file));
    log_file = NULL;
    output_sink = child_output;
    frame_sink = child_frame;
    remote_interrupt = NULL;
    srand(p->shared->race.config.seed + (unsigned int)p->shared->race.cars[index].id);
    for (;;) {
        worker_wait(index + 2);
        if (atomic_load(&p->shared->stage) == STOPPING) break;
        p->shared->cost = perform_turn(&p->shared->race, index, p->shared->limit);
        p->shared->event = DONE;
        if (sem_post(p->semaphores[0]) == -1) _exit(1);
    }
}

static int check_children(RaceProcesses *p) {
    for (int i = 0; i < p->created; i++) {
        if (p->mode == INDEPENDENT) {
            if (p->pidfds[i] >= 0 && process_exited(p->pidfds[i])) return ECHILD;
            continue;
        }
        if (p->children[i] == 0) continue;
        int status;
        pid_t result = waitpid(p->children[i], &status, WNOHANG | WUNTRACED);
        if ((result == -1 && errno == EINTR) || result == 0) continue;
        if (result > 0 && !WIFSTOPPED(status)) p->children[i] = 0;
        if (result == -1 && errno == ECHILD) p->children[i] = 0;
        return result == -1 ? errno : ECHILD;
    }
    return 0;
}

int start_processes(RaceProcesses *p, const Race *initial) {
    return start_race(p, initial, UNNAMED, NULL);
}

int start_race(RaceProcesses *p, const Race *initial, int mode, const char *session) {
    setup(p, mode);
    p->owner = 1;
    int error = 0;
    if (mode == INDEPENDENT) error = session_name(p->name, sizeof(p->name), session);
    else snprintf(p->name, sizeof(p->name), "/akos-race-%ld", (long)getpid());
    if (error != 0) return error;
    int file = shm_open(p->name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (file == -1) return errno;
    p->owns_name = 1;
    if (ftruncate(file, sizeof(SharedRace)) == -1) error = errno;
    if (error == 0) {
        void *memory = mmap(NULL, sizeof(SharedRace), PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
        if (memory == MAP_FAILED) error = errno;
        else p->shared = memory;
    }
    if (close(file) == -1 && error == 0) error = errno;
    if (error != 0) goto failed;
    SharedRace *s = p->shared;
    s->judge = getpid();
    s->generation = (uint64_t)(now() * 1e9);
    s->race = *initial;
    s->race.config.log_path = NULL;
    s->animate = animation;
    s->relay_details = (!quiet && !animation) || log_file != NULL;
    error = open_semaphores(p, 1);
    if (error != 0) goto failed;
    if (mode != INDEPENDENT) {
        // Дети наследуют открытые объекты; имена больше не нужны.
        if (mode == NAMED) error = unlink_semaphores(p);
        if (error != 0) goto failed;
        if (shm_unlink(p->name) == -1) { error = errno; goto failed; }
        p->owns_name = 0;
    }
    atomic_store(&s->stage, mode == INDEPENDENT ? JOINING : RUNNING);
    remote_interrupt = &s->requested_signal;
    if (mode == INDEPENDENT) return 0;
    sigset_t blocked, saved;
    sigemptyset(&blocked); sigaddset(&blocked, SIGINT); sigaddset(&blocked, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &blocked, &saved) == -1) { error = errno; goto failed; }
    for (int i = 0; i < initial->car_count; i++) {
        pid_t child = fork();
        if (child == -1) { error = errno; break; }
        if (child == 0) { car_process(p, i); _exit(0); }
        p->children[p->created++] = child;
    }
    if (sigprocmask(SIG_SETMASK, &saved, NULL) == -1 && error == 0) error = errno;
    if (error == 0) return 0;
failed:;
    int cleanup_error = stop_processes(p, 1);
    if (cleanup_error != 0) { errno = cleanup_error; perror("Ошибка очистки после неудачного запуска"); }
    return error;
}

static int remember_cars(RaceProcesses *p, int stopping) {
    p->created = p->shared->race.car_count;
    int error = 0;
    for (int i = 0; i < p->created; i++) {
        // Замораживаем свободные слоты при остановке: поздний CAS не зарегистрирует машину.
        pid_t pid = stopping ? atomic_exchange(&p->shared->registered[i], -1) : atomic_load(&p->shared->registered[i]);
        if (pid <= 0 || p->pidfds[i] >= 0) continue;
        int fd = open_pid(pid);
        if (fd == -1) { if (error == 0) error = errno; continue; }
        p->pidfds[i] = fd;
    }
    int status = check_children(p);
    return error == 0 ? status : error;
}

int wait_for_cars(RaceProcesses *p, int seconds) {
    if (p->mode != INDEPENDENT) return 0;
    double deadline = now() + seconds;
    int arrivals = 0;
    while (arrivals < p->shared->race.car_count) {
        if (interruption_requested()) return 0;
        int error = remember_cars(p, 0);
        if (error != 0) return error;
        error = wait_event(p->semaphores[0]);
        if (error == 0) arrivals++;
        else if (error != EINTR && error != ETIMEDOUT) return error;
        if (now() >= deadline) return ETIMEDOUT;
    }
    int error = remember_cars(p, 0);
    if (error != 0) return error;
    atomic_store(&p->shared->stage, RUNNING);
    return unlink_semaphores(p);
}

int run_process_round(RaceProcesses *p) {
    int error = check_children(p);
    if (error != 0) return error;
    SharedRace *s = p->shared;
    Race *race = &s->race;
    begin_round(race);
    int count = race->order_count, previous = 0;
    for (int i = 0; i < count; i++) {
        s->limit = i == 0 ? 4 : previous + (i == 1 ? 2 : 1);
        if (sem_post(p->semaphores[race->order[i] + 2]) == -1) return errno;
        double deadline = now() + 3;
        for (;;) {
            interruption_requested();
            error = wait_event(p->semaphores[0]);
            if (error == EINTR || error == ETIMEDOUT) {
                error = check_children(p);
                if (error != 0) return error;
                if (now() >= deadline) return ETIMEDOUT;
                continue;
            }
            if (error != 0) return error;
            if (s->event == DONE) break;
            if (s->event == TEXT) output(s->detail, "%s", s->text);
            else if (s->event == FRAME) {
                Frame *f = &s->frame;
                animate_frame(race, f->car < 0 ? NULL : &race->cars[f->car], f->event,
                              f->step, f->count, f->used, f->limit, f->has_route ? &f->route : NULL);
            } else return EPROTO;
            if (sem_post(p->semaphores[1]) == -1) return errno;
            deadline = now() + 3;
        }
        previous = s->cost;
    }
    end_round(race);
    return 0;
}

static int close_resources(RaceProcesses *p) {
    int error = 0;
    remote_interrupt = NULL;
    for (int i = 0; i < MAX_CARS + 2; i++) {
        if (!p->initialized[i]) continue;
        int result = p->mode == UNNAMED ? sem_destroy(p->semaphores[i]) : sem_close(p->semaphores[i]);
        if (result == -1) { if (error == 0) error = errno; }
        else p->initialized[i] = 0;
    }
    if (p->owner && p->shared != NULL && p->mode != UNNAMED) {
        int e = unlink_semaphores(p);
        if (error == 0) error = e;
    }
    if (p->owner && p->owns_name) {
        if (shm_unlink(p->name) == -1 && errno != ENOENT) { if (error == 0) error = errno; }
        else p->owns_name = 0;
    }
    if (p->shared != NULL) {
        if (munmap(p->shared, sizeof(SharedRace)) == -1) { if (error == 0) error = errno; }
        else p->shared = NULL;
    }
    if (p->judge_fd >= 0) { close(p->judge_fd); p->judge_fd = -1; }
    return error;
}

int stop_processes(RaceProcesses *p, int aborting) {
    int error = 0;
    if (p->shared != NULL) atomic_store(&p->shared->stage, STOPPING);
    if (p->mode == INDEPENDENT && p->shared != NULL) {
        error = remember_cars(p, 1);
        if (error == ECHILD || error == ESRCH) error = 0;
    }
    for (int i = 0; i < p->created; i++) {
        if (p->mode != INDEPENDENT && p->children[i] == 0) continue;
        if (p->mode == INDEPENDENT && p->pidfds[i] < 0) continue;
        if (!aborting && sem_post(p->semaphores[i + 2]) == 0) continue;
        if (!aborting && error == 0) error = errno;
        if (p->mode == INDEPENDENT) syscall(SYS_pidfd_send_signal, p->pidfds[i], SIGKILL, NULL, 0);
        else kill(p->children[i], SIGKILL);
    }
    for (int attempt = 0; attempt < 100; attempt++) {
        int remaining = 0;
        for (int i = 0; i < p->created; i++) {
            if (p->mode == INDEPENDENT) {
                if (p->pidfds[i] >= 0 && !process_exited(p->pidfds[i])) remaining++;
                continue;
            }
            if (p->children[i] == 0) continue;
            int status;
            pid_t result = waitpid(p->children[i], &status, WNOHANG);
            if (result == 0 || (result == -1 && errno == EINTR)) { remaining++; continue; }
            if (result == -1 && error == 0) error = errno;
            if (result > 0 && !aborting && (!WIFEXITED(status) || WEXITSTATUS(status)) && error == 0) error = ECHILD;
            p->children[i] = 0;
        }
        if (remaining == 0) break;
        struct timespec pause = {0, 10000000L}; nanosleep(&pause, NULL);
    }
    for (int i = 0; i < p->created; i++) {
        if (p->mode == INDEPENDENT) {
            if (p->pidfds[i] < 0) continue;
            if (!process_exited(p->pidfds[i])) {
                if (error == 0) error = ETIMEDOUT;
                syscall(SYS_pidfd_send_signal, p->pidfds[i], SIGKILL, NULL, 0);
                struct pollfd event = {p->pidfds[i], POLLIN, 0};
                while (poll(&event, 1, -1) == -1 && errno == EINTR) {}
            }
            close(p->pidfds[i]); p->pidfds[i] = -1;
        } else if (p->children[i] != 0) {
            if (error == 0) error = ETIMEDOUT;
            kill(p->children[i], SIGKILL);
            while (waitpid(p->children[i], NULL, 0) == -1 && errno == EINTR) {}
            p->children[i] = 0;
        }
    }
    p->created = 0;
    int cleanup_error = close_resources(p);
    return error == 0 ? cleanup_error : error;
}

int run_independent_car(const char *session, int id, int seconds) {
    RaceProcesses p;
    setup(&p, INDEPENDENT);
    int error = session_name(p.name, sizeof(p.name), session);
    if (error != 0) return error;
    double deadline = now() + seconds;
    for (;;) {
        if (interrupted) return EINTR;
        int file = shm_open(p.name, O_RDWR, 0);
        if (file >= 0) {
            struct stat info;
            if (fstat(file, &info) == -1) error = errno;
            else if (info.st_size == sizeof(SharedRace)) {
                void *memory = mmap(NULL, sizeof(SharedRace), PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
                if (memory == MAP_FAILED) error = errno;
                else { p.shared = memory; p.inode = info.st_ino; p.device = info.st_dev; }
            }
            close(file);
        } else if (errno != ENOENT) return errno;
        if (error != 0) break;
        if (p.shared != NULL && atomic_load(&p.shared->stage) != INITIALIZING) break;
        if (p.shared != NULL) { munmap(p.shared, sizeof(SharedRace)); p.shared = NULL; }
        if (now() >= deadline) return ETIMEDOUT;
        struct timespec pause = {0, 10000000L}; nanosleep(&pause, NULL);
    }
    int index = -1;
    if (error == 0) {
        if (p.shared->race.car_count < 1 || p.shared->race.car_count > MAX_CARS) error = EPROTO;
        else {
            p.judge_fd = open_pid(p.shared->judge);
            if (p.judge_fd == -1 || process_exited(p.judge_fd)) {
                emergency_unlink(&p); error = ESRCH;
            } else if (atomic_load(&p.shared->stage) != JOINING) error = EBUSY;
        }
    }
    if (error == 0) {
        for (int i = 0; i < p.shared->race.car_count; i++) if (p.shared->race.cars[i].id == id) index = i;
        if (index < 0) error = EINVAL;
        else error = open_semaphores(&p, 0);
    }
    if (error == 0) {
        int expected = 0;
        if (!atomic_compare_exchange_strong(&p.shared->registered[index], &expected, getpid())) error = EEXIST;
        else if (atomic_load(&p.shared->stage) != JOINING) error = EBUSY;
        else if (sem_post(p.semaphores[0]) == -1) error = errno;
        else car_process(&p, index);
    }
    int cleanup_error = close_resources(&p);
    output_sink = NULL; frame_sink = NULL;
    return error == 0 ? cleanup_error : error;
}
