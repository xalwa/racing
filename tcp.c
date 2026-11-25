#define _POSIX_C_SOURCE 200809L
#include "tcp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

enum { TCP_VERSION = 1 };

uint32_t tcp_get_word(const unsigned char *data, size_t index) {
    uint32_t value;
    memcpy(&value, data + index * sizeof(value), sizeof(value));
    return ntohl(value);
}

int32_t tcp_get_signed(const unsigned char *data, size_t index) {
    uint32_t value = tcp_get_word(data, index);
    return value <= INT32_MAX ? (int32_t)value : -1 - (int32_t)(UINT32_MAX - value);
}

void tcp_put_word(unsigned char *data, size_t index, uint32_t value) {
    value = htonl(value);
    memcpy(data + index * sizeof(value), &value, sizeof(value));
}

int64_t tcp_now_ms(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

int tcp_number(const char *text, uint32_t min, uint32_t max, uint32_t *value) {
    if (*text < '0' || *text > '9') return 0;
    char *end;
    errno = 0;
    unsigned long n = strtoul(text, &end, 10);
    if (errno || *end || n < min || n > max) return 0;
    *value = (uint32_t)n;
    return 1;
}

int tcp_queue(TcpBuffer *out, unsigned char type, const void *data, size_t size) {
    if (out->size != 0 || size > TCP_MAX_PAYLOAD) return 0;
    out->bytes[0] = 'R';
    out->bytes[1] = 'C';
    out->bytes[2] = TCP_VERSION;
    out->bytes[3] = type;
    tcp_put_word(out->bytes, 1, (uint32_t)size);
    if (size != 0) memcpy(out->bytes + TCP_HEADER_SIZE, data, size);
    out->size = TCP_HEADER_SIZE + size;
    out->used = 0;
    return 1;
}

int tcp_flush(int fd, TcpBuffer *out) {
    while (out->used < out->size) {
        ssize_t n = send(fd, out->bytes + out->used, out->size - out->used, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return TCP_WAIT;
        if (n <= 0) return TCP_IO_ERROR;
        out->used += (size_t)n;
    }
    out->used = out->size = 0;
    return TCP_FRAME;
}

int tcp_receive(int fd, TcpBuffer *in, TcpFrame *frame) {
    for (;;) {
        size_t target = in->size ? in->size : TCP_HEADER_SIZE;
        if (in->used < target) {
            ssize_t n = recv(fd, in->bytes + in->used, target - in->used, MSG_DONTWAIT);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return TCP_WAIT;
            if (n < 0) return TCP_IO_ERROR;
            if (n == 0) return in->used ? TCP_BAD_FRAME : TCP_EOF;
            in->used += (size_t)n;
            continue;
        }
        if (in->size == 0) {
            uint32_t size = tcp_get_word(in->bytes, 1);
            if (in->bytes[0] != 'R' || in->bytes[1] != 'C' || in->bytes[2] != TCP_VERSION ||
                size > TCP_MAX_PAYLOAD) return TCP_BAD_FRAME;
            in->size = TCP_HEADER_SIZE + size;
            continue;
        }
        *frame = (TcpFrame){in->bytes[3], in->size - TCP_HEADER_SIZE, in->bytes + TCP_HEADER_SIZE};
        in->used = in->size = 0;
        return TCP_FRAME;
    }
}
