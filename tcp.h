#ifndef RACE_TCP_H
#define RACE_TCP_H

#include <stddef.h>
#include <stdint.h>

enum { TCP_HELLO = 1, TCP_WELCOME, TCP_TURN, TCP_MOVE, TCP_RESULT, TCP_ERROR,
       TCP_WATCH, TCP_VIEW, TCP_STANDINGS, TCP_STOP };
enum { TCP_BAD_MESSAGE = 1, TCP_BAD_ID, TCP_ID_BUSY, TCP_ALREADY_STARTED,
       TCP_BAD_TURN, TCP_BAD_ROUTE, TCP_WATCH_FULL };
enum { TCP_COMPLETED = 1, TCP_ROUND_LIMIT, TCP_INTERRUPTED, TCP_ABORTED };
enum { TCP_WAIT, TCP_FRAME, TCP_EOF, TCP_IO_ERROR, TCP_BAD_FRAME };

enum { TCP_HEADER_SIZE = 8, TCP_MAX_PAYLOAD = 2048 };

typedef struct {
    unsigned char bytes[TCP_HEADER_SIZE + TCP_MAX_PAYLOAD];
    size_t used, size;
} TcpBuffer;

typedef struct {
    unsigned int type;
    size_t size;
    const unsigned char *data;
} TcpFrame;

uint32_t tcp_get_word(const unsigned char *data, size_t index);
int32_t tcp_get_signed(const unsigned char *data, size_t index);
void tcp_put_word(unsigned char *data, size_t index, uint32_t value);
int64_t tcp_now_ms(void);
int tcp_number(const char *text, uint32_t min, uint32_t max, uint32_t *value);
// Размер и смещение исходящей записи сохраняются между вызовами poll.
int tcp_queue(TcpBuffer *out, unsigned char type, const void *data, size_t size);
int tcp_flush(int fd, TcpBuffer *out);
// Данные кадра остаются действительными до следующего чтения из этого буфера.
int tcp_receive(int fd, TcpBuffer *in, TcpFrame *frame);

#endif
