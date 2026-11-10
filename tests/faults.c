// Только тесты: выбранный вызов завершается ошибкой или замедляет запуск ребёнка.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int fail_at(const char *operation, int call) {
    const char *value = getenv("AKOS_FAIL");
    return value != NULL && strncmp(value, operation, strlen(operation)) == 0 &&
           value[strlen(operation)] == ':' && atoi(value + strlen(operation) + 1) == call;
}

#define REAL(name) *(void **)(&original) = dlsym(RTLD_NEXT, name)

mqd_t mq_open(const char *name, int flags, ...) {
    static int calls;
    mqd_t (*original)(const char *, int, ...); REAL("mq_open");
    if (fail_at("mq", ++calls)) { errno = ENOSPC; return (mqd_t)-1; }
    if (!(flags & O_CREAT)) return original(name, flags);
    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    struct mq_attr *attr = va_arg(args, struct mq_attr *);
    va_end(args);
    return original(name, flags, mode, attr);
}

sem_t *sem_open(const char *name, int flags, ...) {
    static int calls;
    sem_t *(*original)(const char *, int, ...); REAL("sem_open");
    if (fail_at("open", ++calls)) { errno = ENOSPC; return SEM_FAILED; }
    if (!(flags & O_CREAT)) return original(name, flags);
    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    unsigned int value = va_arg(args, unsigned int);
    va_end(args);
    return original(name, flags, mode, value);
}

int sem_close(sem_t *s) {
    static int calls;
    int (*original)(sem_t *); REAL("sem_close");
    if (fail_at("close", ++calls)) { errno = EINVAL; return -1; }
    return original(s);
}

int sem_unlink(const char *name) {
    static int calls;
    int (*original)(const char *); REAL("sem_unlink");
    if (fail_at("sem_unlink", ++calls)) { errno = EACCES; return -1; }
    return original(name);
}

int shm_open(const char *name, int flags, mode_t mode) {
    int (*original)(const char *, int, mode_t); REAL("shm_open");
    if (fail_at("shm", 1)) { errno = ENOSPC; return -1; }
    return original(name, flags, mode);
}

int ftruncate(int file, off_t size) {
    int (*original)(int, off_t); REAL("ftruncate");
    if (fail_at("truncate", 1)) { errno = ENOSPC; return -1; }
    return original(file, size);
}

void *mmap(void *address, size_t size, int protection, int flags, int file, off_t offset) {
    void *(*original)(void *, size_t, int, int, int, off_t); REAL("mmap");
    if (fail_at("map", 1)) { errno = ENOMEM; return MAP_FAILED; }
    return original(address, size, protection, flags, file, offset);
}

int shm_unlink(const char *name) {
    static int calls;
    int (*original)(const char *); REAL("shm_unlink");
    if (fail_at("unlink", ++calls)) { errno = EACCES; return -1; }
    return original(name);
}

int sem_init(sem_t *semaphore, int shared, unsigned int value) {
    static int calls;
    int (*original)(sem_t *, int, unsigned int); REAL("sem_init");
    if (fail_at("sem", ++calls)) { errno = ENOSPC; return -1; }
    return original(semaphore, shared, value);
}

int sem_destroy(sem_t *semaphore) {
    static int calls;
    int (*original)(sem_t *); REAL("sem_destroy");
    if (fail_at("destroy", ++calls)) { errno = EINVAL; return -1; }
    return original(semaphore);
}

pid_t fork(void) {
    static int calls;
    pid_t (*original)(void); REAL("fork");
    int number = ++calls;
    if (fail_at("fork", number)) { errno = EAGAIN; return -1; }
    pid_t child = original();
    if (child == 0 && getenv("AKOS_SLOW_CHILD") != NULL) usleep(300000);
    if (child == 0 && number == 1 && getenv("AKOS_CHILD_EXIT") != NULL) _exit(7);
    return child;
}

int sem_timedwait(sem_t *semaphore, const struct timespec *deadline) {
    static int interrupted;
    int (*original)(sem_t *, const struct timespec *); REAL("sem_timedwait");
    if (!interrupted && getenv("AKOS_EINTR") != NULL) {
        interrupted = 1;
        errno = EINTR;
        return -1;
    }
    return original(semaphore, deadline);
}
