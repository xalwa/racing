#ifndef RACE_TCP_CLIENT_H
#define RACE_TCP_CLIENT_H

#include "tcp.h"

int client_open(const char *host, const char *port);
int client_send(int fd, unsigned char type, const void *data, size_t size);
int client_receive(int fd, TcpBuffer *input, TcpFrame *frame);
int client_finished(const TcpFrame *frame);
int client_exit(int status);

#endif
