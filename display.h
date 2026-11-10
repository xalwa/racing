#ifndef DISPLAY_H
#define DISPLAY_H

#include "race.h"
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdarg.h>

extern FILE *log_file;
extern int quiet;
extern int animation;
extern atomic_int interrupted;
// В дочернем процессе вывод передаётся судье, который владеет экраном и файлом.
extern void (*output_sink)(int detail, const char *format, va_list args);
extern void (*frame_sink)(const Race *, const Car *, const char *, int, int, int, int, const Route *);
extern atomic_int *remote_interrupt;

int interruption_requested(void);

void output(int detail, const char *format, ...);
void handle_interrupt(int signal_number);
void stop_animation(void);
void start_animation(void);
void animate_frame(const Race *race, const Car *car, const char *event,
                   int step, int count, int used, int limit, const Route *route);
void print_positions(const Race *race);
void print_track(const Race *race);

#endif
