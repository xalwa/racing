#define _GNU_SOURCE
#include "queues.h"
#include "processes.h"
#include "display.h"
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { HELLO, TEXT, FRAME, DONE, ACCEPT, REJECT, TURN, STOP };
typedef struct {
    int kind, id, cost, detail;
    pid_t pid;
    Frame frame;
    Race race;
    char text[512];
} Event;
typedef struct {
    int kind, index, animate, relay, limit;
    pid_t judge;
    Race race;
} Job;
typedef struct { mqd_t jobs; sem_t *permission; int pidfd; pid_t pid; } Peer;
typedef struct {
    mqd_t events;
    sem_t *ready, *ack, *cleanup;
    char base[64];
    Peer peers[MAX_CARS];
    unsigned int owned;
} QueueRace;
static QueueRace *channel;
static int server_fd = -1, forwarded, relay_details;

_Static_assert(sizeof(Event) <= 8192 && sizeof(Job) <= 8192, "Сообщение превышает стандартный предел Linux");

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static struct timespec deadline(void) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += 100000000;
    if (t.tv_nsec >= 1000000000) { t.tv_nsec -= 1000000000; t.tv_sec++; }
    return t;
}
static int exited(int fd) {
    struct pollfd event = {fd, POLLIN, 0};
    return poll(&event, 1, 0) > 0;
}
static int pidfd(pid_t pid) { return (int)syscall(SYS_pidfd_open, pid, 0); }
static void kill_peer(Peer *p) {
    if (p->pidfd >= 0) syscall(SYS_pidfd_send_signal, p->pidfd, SIGKILL, NULL, 0);
}
static int setup(QueueRace *q, const char *session) {
    memset(q, 0, sizeof(*q)); q->events = (mqd_t)-1;
    for (int i = 0; i < MAX_CARS; i++) { q->peers[i].jobs = (mqd_t)-1; q->peers[i].pidfd = -1; }
    if (!session || !*session || strlen(session) > 32) return EINVAL;
    for (const char *s = session; *s; s++)
        if (!( (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
               (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return EINVAL;
    snprintf(q->base, sizeof(q->base), "/akos-mq-%s", session);
    return 0;
}
static void owner_name(QueueRace *q, const char *suffix, char name[96]) {
    snprintf(name, 96, "%s-%s", q->base, suffix);
}
static void client_name(pid_t pid, char name[96]) {
    snprintf(name, 96, "/akos-client-%ld", (long)pid);
}
static int send_job(Peer *p, Job *job) {
    return mq_send(p->jobs, (const char *)job, sizeof(*job), 0) == 0 ? 0 : errno;
}
static int wait_sem(sem_t *s) {
    struct timespec t = deadline();
    return sem_timedwait(s, &t) == 0 ? 0 : errno;
}
static int peer_dead(QueueRace *q) {
    for (int i = 0; i < MAX_CARS; i++)
        if (q->peers[i].pidfd >= 0 && exited(q->peers[i].pidfd)) return ECHILD;
    return 0;
}
static void close_peer(Peer *p) {
    if (p->jobs != (mqd_t)-1) mq_close(p->jobs);
    if (p->permission) sem_close(p->permission);
    if (p->pidfd >= 0) close(p->pidfd);
    p->jobs = (mqd_t)-1; p->permission = NULL; p->pidfd = -1;
}
static int remove_owner(QueueRace *q) {
    char name[96]; int error = 0;
    const char *suffix[] = {"ready", "ack", "cleanup"};
    for (int i = 0; i < 3; i++) {
        if (!(q->owned & (2u << i))) continue;
        owner_name(q, suffix[i], name);
        if (sem_unlink(name) == -1 && errno != ENOENT && !error) error = errno;
    }
    if (q->owned & 1) {
        owner_name(q, "events", name);
        if (mq_unlink(name) == -1 && errno != ENOENT && !error) error = errno;
    }
    return error;
}
static int close_channel(QueueRace *q) {
    int error = 0;
    sem_t *sems[] = {q->ready, q->ack, q->cleanup};
    for (int i = 0; i < 3; i++) if (sems[i] && sem_close(sems[i]) == -1 && !error) error = errno;
    if (q->events != (mqd_t)-1 && mq_close(q->events) == -1 && !error) error = errno;
    return error;
}
static int open_owner(QueueRace *q) {
    char name[96]; owner_name(q, "events", name);
    struct mq_attr attr = {.mq_maxmsg = 4, .mq_msgsize = sizeof(Event)};
    q->events = mq_open(name, O_CREAT | O_EXCL | O_RDONLY | O_NONBLOCK, 0600, &attr);
    if (q->events == (mqd_t)-1) return errno;
    q->owned |= 1;
    sem_t **slots[] = {&q->ready, &q->ack, &q->cleanup};
    const char *suffix[] = {"ready", "ack", "cleanup"};
    for (int i = 0; i < 3; i++) {
        owner_name(q, suffix[i], name);
        sem_t *s = sem_open(name, O_CREAT | O_EXCL, 0600, i == 2 ? 1 : 0);
        if (s == SEM_FAILED) return errno;
        *slots[i] = s; q->owned |= 2u << i;
    }
    return 0;
}
static int receive_event(QueueRace *q, Event *event, double until, int cancellable) {
    for (;;) {
        int error = wait_sem(q->ready);
        if (!error) return mq_receive(q->events, (char *)event, sizeof(*event), NULL) == sizeof(*event) ? 0 : EPROTO;
        if (error != EINTR && error != ETIMEDOUT) return error;
        error = peer_dead(q);
        if (error) return error;
        if (now() >= until) return ETIMEDOUT;
        if (cancellable && interrupted) return EINTR;
    }
}
static int register_car(QueueRace *q, Event *e, Race *race, int accepting, int *added) {
    Peer p = {.jobs = (mqd_t)-1, .pidfd = -1, .pid = e->pid};
    if (e->pid <= 0) return EPROTO;
    char name[96]; client_name(e->pid, name);
    p.jobs = mq_open(name, O_WRONLY | O_NONBLOCK);
    if (p.jobs == (mqd_t)-1) return 0; // Отменённый запрос подключения.
    p.permission = sem_open(name, 0);
    if (p.permission == SEM_FAILED) { p.permission = NULL; close_peer(&p); return 0; }
    p.pidfd = pidfd(e->pid);
    mq_unlink(name); sem_unlink(name); // Оба участника уже открыли объекты.
    int index = -1;
    for (int i = 0; i < race->car_count; i++) if (race->cars[i].id == e->id) index = i;
    Job reply = {.kind = REJECT};
    if (accepting && index >= 0 && q->peers[index].pidfd < 0 && p.pidfd >= 0 && !exited(p.pidfd)) {
        reply = (Job){.kind = ACCEPT, .index = index, .animate = animation,
                      .relay = (!quiet && !animation) || log_file != NULL, .judge = getpid(), .race = *race};
        reply.race.config.log_path = NULL;
    }
    int error = send_job(&p, &reply);
    if (!error && reply.kind == ACCEPT) { q->peers[index] = p; (*added)++; }
    else close_peer(&p);
    return error == EAGAIN ? 0 : error;
}
static int stop_queue(QueueRace *q, int aborting) {
    int error = 0;
    Job stop = {.kind = STOP};
    for (int i = 0; i < MAX_CARS; i++) {
        Peer *p = &q->peers[i];
        if (p->pidfd < 0) continue;
        if (aborting || send_job(p, &stop) || sem_post(p->permission) == -1) kill_peer(p);
    }
    double until = now() + 1;
    for (;;) {
        int live = 0;
        for (int i = 0; i < MAX_CARS; i++) if (q->peers[i].pidfd >= 0 && !exited(q->peers[i].pidfd)) live++;
        if (!live || now() >= until) break;
        struct timespec pause = {0, 10000000}; nanosleep(&pause, NULL);
    }
    for (int i = 0; i < MAX_CARS; i++) {
        Peer *p = &q->peers[i];
        if (p->pidfd >= 0 && !exited(p->pidfd)) {
            error = ETIMEDOUT; kill_peer(p);
            struct pollfd fd = {p->pidfd, POLLIN, 0};
            while (poll(&fd, 1, -1) == -1 && errno == EINTR) {}
        }
        close_peer(p);
    }
    int e = remove_owner(q); if (!error) error = e;
    e = close_channel(q); return error ? error : e;
}

int run_queue_judge(Race *race, const char *session, int seconds) {
    QueueRace q;
    int error = setup(&q, session);
    if (error) return error;
    error = open_owner(&q);
    int added = 0;
    double until = now() + seconds;
    Event e;
    while (!error && added < race->car_count && !interrupted) {
        error = receive_event(&q, &e, until, 1);
        if (!error) error = e.kind == HELLO ? register_car(&q, &e, race, 1, &added) : EPROTO;
    }
    if (error == EINTR && interrupted) error = 0;
    if (!error && !interrupted) {
        start_animation();
        animate_frame(race, NULL, "Стартовая расстановка", 0, 0, 0, 0, NULL);
    }
    while (!error && !interrupted && active_count(race) && race->rounds < race->config.max_rounds) {
        begin_round(race);
        int count = race->order_count, previous = 0;
        for (int i = 0; !error && i < count; i++) {
            int index = race->order[i];
            Peer *p = &q.peers[index];
            Job job = {.kind = TURN, .index = index, .judge = getpid(), .race = *race};
            job.race.config.log_path = NULL;
            job.limit = i == 0 ? 4 : previous + (i == 1 ? 2 : 1);
            error = send_job(p, &job);
            if (!error && sem_post(p->permission) == -1) error = errno;
            int done = 0;
            while (!error && !done) {
                error = receive_event(&q, &e, now() + 3, 0);
                if (error == EINTR && interrupted) { error = 0; continue; }
                if (error) break;
                if (e.kind == HELLO) { error = register_car(&q, &e, race, 0, &added); continue; }
                if (e.id != race->cars[index].id || e.pid != p->pid) { error = EPROTO; break; }
                if (e.kind == TEXT) output(e.detail, "%s", e.text);
                else if (e.kind == FRAME || e.kind == DONE) {
                    *race = e.race;
                    if (e.kind == DONE) { previous = e.cost; done = 1; }
                    else {
                        Frame *f = &e.frame;
                        animate_frame(race, &race->cars[index], f->event, f->step, f->count,
                                      f->used, f->limit, f->has_route ? &f->route : NULL);
                    }
                } else error = EPROTO;
                if (!error && !done && sem_post(q.ack) == -1) error = errno;
            }
        }
        if (!error) end_round(race);
    }
    int cleanup = stop_queue(&q, error != 0);
    return error ? error : cleanup;
}

static void orphan_cleanup(QueueRace *q) {
    if (sem_trywait(q->cleanup) == -1) return;
    char name[96]; owner_name(q, "events", name);
    mqd_t current = mq_open(name, O_RDONLY | O_NONBLOCK);
    struct stat a, b;
    int same = current != (mqd_t)-1 && fstat((int)current, &a) == 0 && fstat((int)q->events, &b) == 0 &&
               a.st_ino == b.st_ino && a.st_dev == b.st_dev;
    if (current != (mqd_t)-1) mq_close(current);
    if (same) { q->owned = 15; remove_owner(q); }
}
static void check_server(void) {
    if (interrupted && !forwarded) {
        syscall(SYS_pidfd_send_signal, server_fd, atomic_load(&interrupted), NULL, 0); forwarded = 1;
    }
    if (exited(server_fd)) { orphan_cleanup(channel); _exit(1); }
}
static void car_wait(sem_t *s) {
    for (;;) {
        check_server();
        int error = wait_sem(s);
        if (!error) return;
        if (error != EINTR && error != ETIMEDOUT) _exit(1);
    }
}
static Event event_message;
static void car_publish(int kind) {
    event_message.kind = kind;
    for (;;) {
        struct timespec t = deadline();
        if (mq_timedsend(channel->events, (char *)&event_message, sizeof(event_message), 0, &t) == 0) break;
        if (errno != EINTR && errno != ETIMEDOUT) _exit(1);
        check_server();
    }
    if (sem_post(channel->ready) == -1) _exit(1);
    if (kind != DONE) car_wait(channel->ack);
}
static void queue_text(int detail, const char *format, va_list args) {
    if (detail && !relay_details) return;
    event_message.detail = detail;
    int n = vsnprintf(event_message.text, sizeof(event_message.text), format, args);
    if (n < 0 || (size_t)n >= sizeof(event_message.text)) _exit(1);
    car_publish(TEXT);
}
static void queue_frame(const Race *race, const Car *car, const char *event,
                        int step, int count, int used, int limit, const Route *route) {
    if (!animation) return;
    Frame *f = &event_message.frame;
    f->car = car == NULL ? -1 : (int)(car - race->cars); f->step = step; f->count = count; f->used = used; f->limit = limit;
    f->has_route = route != NULL; if (route) f->route = *route;
    snprintf(f->event, sizeof(f->event), "%s", event);
    event_message.race = *race;
    car_publish(FRAME);
}

int run_queue_car(const char *session, int id, int seconds) {
    QueueRace q;
    int error = setup(&q, session);
    if (error) return error;
    double until = now() + seconds;
    char name[96], own[96]; client_name(getpid(), own);
    Peer self = {.jobs = (mqd_t)-1, .pidfd = -1};
    int owns_permission = 0;
    struct mq_attr attr = {.mq_maxmsg = 1, .mq_msgsize = sizeof(Job)};
    self.jobs = mq_open(own, O_CREAT | O_EXCL | O_RDONLY, 0600, &attr);
    if (self.jobs == (mqd_t)-1) return errno;
    self.permission = sem_open(own, O_CREAT | O_EXCL, 0600, 0);
    if (self.permission == SEM_FAILED) { self.permission = NULL; error = errno; goto finished; }
    owns_permission = 1;
    for (;;) {
        owner_name(&q, "events", name); q.events = mq_open(name, O_WRONLY);
        if (q.events != (mqd_t)-1) {
            const char *suffix[] = {"ready", "ack", "cleanup"};
            sem_t **slots[] = {&q.ready, &q.ack, &q.cleanup};
            for (int i = 0; i < 3; i++) {
                owner_name(&q, suffix[i], name);
                sem_t *s = sem_open(name, 0);
                if (s == SEM_FAILED) { error = errno; break; }
                *slots[i] = s;
            }
            if (!error) break;
            close_channel(&q); q.events = (mqd_t)-1; q.ready = q.ack = q.cleanup = NULL;
        } else if (errno != ENOENT) { error = errno; goto finished; }
        if (interrupted || now() >= until) { error = interrupted ? EINTR : ETIMEDOUT; goto finished; }
        error = 0; struct timespec pause = {0, 10000000}; nanosleep(&pause, NULL);
    }
    Event hello = {.kind = HELLO, .id = id, .pid = getpid()};
    for (;;) {
        struct timespec t = deadline();
        if (mq_timedsend(q.events, (char *)&hello, sizeof(hello), 0, &t) == 0) break;
        if ((errno != EINTR && errno != ETIMEDOUT) || interrupted || now() >= until) {
            error = interrupted ? EINTR : ETIMEDOUT; goto finished;
        }
    }
    if (sem_post(q.ready) == -1) { error = errno; goto finished; }
    Job job;
    for (;;) {
        struct timespec t = deadline();
        ssize_t n = mq_timedreceive(self.jobs, (char *)&job, sizeof(job), NULL, &t);
        if (n == sizeof(job)) break;
        if ((errno != EINTR && errno != ETIMEDOUT) || interrupted || now() >= until) {
            error = interrupted ? EINTR : ETIMEDOUT; goto finished;
        }
    }
    if (job.kind != ACCEPT) { error = EEXIST; goto finished; }
    server_fd = pidfd(job.judge);
    if (server_fd == -1) { error = errno; goto finished; }
    channel = &q; forwarded = 0;
    memset(&event_message, 0, sizeof(event_message));
    event_message.id = id; event_message.pid = getpid(); relay_details = job.relay;
    animation = job.animate;
    output_sink = queue_text; frame_sink = queue_frame;
    srand(job.race.config.seed + (unsigned int)id);
    for (;;) {
        car_wait(self.permission);
        if (mq_receive(self.jobs, (char *)&job, sizeof(job), NULL) != sizeof(job)) { error = EPROTO; break; }
        if (job.kind == STOP) break;
        if (job.kind != TURN) { error = EPROTO; break; }
        event_message.cost = perform_turn(&job.race, job.index, job.limit);
        event_message.race = job.race;
        car_publish(DONE);
    }
finished:
    output_sink = NULL; frame_sink = NULL;
    if (server_fd >= 0) { close(server_fd); server_fd = -1; }
    close_peer(&self);
    mq_unlink(own); if (owns_permission) sem_unlink(own);
    int cleanup = close_channel(&q);
    return error ? error : cleanup;
}
