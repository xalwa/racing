#define _GNU_SOURCE
#include "tcp_client.h"
#include "display.h"
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

static int wait_io(int fd, short events, int64_t deadline) {
    struct pollfd pfd = {.fd = fd, .events = events};
    while (!interruption_requested()) {
        int64_t left = deadline ? deadline - tcp_now_ms() : 200;
        if (left <= 0) { errno = ETIMEDOUT; return 0; }
        int ready = poll(&pfd, 1, left < 200 ? (int)left : 200);
        if (ready > 0) return 1;
        if (ready < 0 && errno != EINTR) return 0;
    }
    errno = EINTR;
    return 0;
}

int client_open(const char *host, const char *port) {
    uint32_t number;
    if (!tcp_number(port, 1, 65535, &number)) { errno = EINVAL; return -1; }
    struct sigaction action = {.sa_handler = handle_interrupt};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) return -1;

    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV};
    struct addrinfo *addr;
    if (getaddrinfo(host, port, &hints, &addr) != 0) { errno = EINVAL; return -1; }
    int fd = socket(addr->ai_family, addr->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, addr->ai_protocol);
    if (fd >= 0 && connect(fd, addr->ai_addr, addr->ai_addrlen) < 0) {
        int error = errno;
        if (error == EINPROGRESS) {
            socklen_t length = sizeof(error);
            if (!wait_io(fd, POLLOUT, tcp_now_ms() + 5000) ||
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) error = errno;
        }
        if (error) { close(fd); fd = -1; errno = error; }
    }
    freeaddrinfo(addr);
    return fd;
}

int client_send(int fd, unsigned char type, const void *data, size_t size) {
    TcpBuffer out = {0};
    if (!tcp_queue(&out, type, data, size)) return 0;
    int64_t deadline = tcp_now_ms() + 5000;
    while (!interruption_requested()) {
        int status = tcp_flush(fd, &out);
        if (status != TCP_WAIT) return status == TCP_FRAME;
        if (!wait_io(fd, POLLOUT, deadline)) break;
    }
    return 0;
}

int client_receive(int fd, TcpBuffer *input, TcpFrame *frame) {
    while (!interruption_requested()) {
        int status = tcp_receive(fd, input, frame);
        if (status != TCP_WAIT) return status;
        if (!wait_io(fd, POLLIN, 0)) break;
    }
    return TCP_IO_ERROR;
}

int client_finished(const TcpFrame *frame) {
    if (frame->size != 4) return -1;
    uint32_t code = tcp_get_word(frame->data, 0);
    if (frame->type == TCP_ERROR) {
        fprintf(stderr, "Судья отклонил запрос (код %u).\n", (unsigned int)code);
        return 1;
    }
    if (frame->type == TCP_STOP && code == TCP_INTERRUPTED) {
        puts("Судья остановил гонку.");
        return 0;
    }
    if (frame->type == TCP_STOP && code == TCP_ABORTED) {
        fprintf(stderr, "Судья завершил гонку с ошибкой.\n");
        return 1;
    }
    return -1;
}

int client_exit(int status) {
    int number = interruption_requested();
    if (number) return 128 + number;
    if (status < 0) {
        fprintf(stderr, "Связь с судьёй потеряна или получен неверный кадр.\n");
        return 1;
    }
    return fflush(stdout) == EOF ? 1 : status;
}
