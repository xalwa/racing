// Подмена POSIX-вызовов только в тестах. В саму программу не включается.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int creates;
static int semaphores;
static int destroyed_semaphores;

static int failure_at(const char *name, int call) {
    const char *value = getenv(name);
    return value != NULL && atoi(value) == call;
}

static void record(const char *event) {
    const char *path = getenv("AKOS_TEST_EVENTS");
    if (path == NULL) return;
    int saved = errno;
    int file = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (file >= 0) {
        dprintf(file, "%s\n", event);
        close(file);
    }
    errno = saved;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*entry)(void *), void *argument) {
    int (*real_call)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    *(void **)(&real_call) = dlsym(RTLD_NEXT, "pthread_create");
    if (failure_at("AKOS_TEST_FAIL_CREATE", ++creates)) return EAGAIN;
    if (failure_at("AKOS_TEST_SIGNAL_CREATE", creates)) {
        record("signal_create");
        raise(atoi(getenv("AKOS_TEST_SIGNAL_NUMBER")));
    }
    int error = real_call(thread, attr, entry, argument);
    if (error == 0) record("create");
    return error;
}

int pthread_join(pthread_t thread, void **result) {
    int (*real_call)(pthread_t, void **);
    *(void **)(&real_call) = dlsym(RTLD_NEXT, "pthread_join");
    int error = real_call(thread, result);
    if (error == 0) record("join");
    return error;
}

int sem_init(sem_t *semaphore, int shared, unsigned int value) {
    int (*real_call)(sem_t *, int, unsigned int);
    *(void **)(&real_call) = dlsym(RTLD_NEXT, "sem_init");
    if (failure_at("AKOS_TEST_FAIL_SEM", ++semaphores)) {
        errno = ENOSPC;
        return -1;
    }
    int error = real_call(semaphore, shared, value);
    if (error == 0) record("sem_init");
    return error;
}

int sem_destroy(sem_t *semaphore) {
    int (*real_call)(sem_t *);
    *(void **)(&real_call) = dlsym(RTLD_NEXT, "sem_destroy");
    record("sem_destroy_attempt");
    if (failure_at("AKOS_TEST_FAIL_DESTROY_SEM", ++destroyed_semaphores)) {
        errno = EINVAL;
        return -1;
    }
    int error = real_call(semaphore);
    if (error == 0) record("sem_destroy");
    return error;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    int (*real_call)(pthread_mutex_t *, const pthread_mutexattr_t *);
    *(void **)(&real_call) = dlsym(RTLD_NEXT, "pthread_mutex_init");
    if (getenv("AKOS_TEST_FAIL_MUTEX") != NULL) return EAGAIN;
    int error = real_call(mutex, attr);
    if (error == 0) record("mutex_init");
    return error;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    int (*real_call)(pthread_mutex_t *);
    *(void **)(&real_call) = dlsym(RTLD_NEXT, "pthread_mutex_destroy");
    record("mutex_destroy_attempt");
    if (getenv("AKOS_TEST_FAIL_DESTROY_MUTEX") != NULL) return EBUSY;
    int error = real_call(mutex);
    if (error == 0) record("mutex_destroy");
    return error;
}

int sem_wait(sem_t *semaphore) {
    static _Thread_local int interrupted_once;
    int (*real_call)(sem_t *);
    *(void **)(&real_call) = dlsym(RTLD_NEXT, "sem_wait");
    if (getenv("AKOS_TEST_EINTR") != NULL && !interrupted_once) {
        interrupted_once = 1;
        record("eintr");
        errno = EINTR;
        return -1;
    }
    if (getenv("AKOS_TEST_JITTER") != NULL) {
        struct timespec delay = {0, (syscall(SYS_gettid) % 13 + 1) * 10000L};
        nanosleep(&delay, NULL);
    }
    return real_call(semaphore);
}

int fclose(FILE *file) {
    int (*real_call)(FILE *);
    *(void **)(&real_call) = dlsym(RTLD_NEXT, "fclose");
    int error = real_call(file);
    record("fclose");
    return error;
}

int nanosleep(const struct timespec *request, struct timespec *remaining) {
    int (*real_call)(const struct timespec *, struct timespec *);
    *(void **)(&real_call) = dlsym(RTLD_NEXT, "nanosleep");
    if (getenv("AKOS_TEST_SKIP_MAIN_DELAY") != NULL && syscall(SYS_gettid) == getpid()) return 0;
    return real_call(request, remaining);
}
