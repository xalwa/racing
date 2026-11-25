CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDLIBS = -pthread -lrt
COMMON = processes.c queues.c race.c display.c
HEADERS = processes.h queues.h race.h display.h
NET_COMMON = tcp.c race_wire.c race.c display.c
NET_HEADERS = tcp.h race_wire.h race.h display.h

.PHONY: all network unnamed named independent test test-tcp test-processes test-display test-variants queues clean

all: network race race-named race-judge race-car race-mq-judge race-mq-car
network: race-server race-driver race-watch
unnamed: race
named: race-named
independent: race-judge race-car
queues: race-mq-judge race-mq-car

race-server: race_server.c $(NET_COMMON) $(NET_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) race_server.c $(NET_COMMON) $(LDFLAGS) -o $@

race-driver: race_driver.c tcp_client.c tcp_client.h $(NET_COMMON) $(NET_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) race_driver.c tcp_client.c $(NET_COMMON) $(LDFLAGS) -o $@

race-watch: race_watch.c tcp_client.c tcp_client.h $(NET_COMMON) $(NET_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) race_watch.c tcp_client.c $(NET_COMMON) $(LDFLAGS) -o $@

race: main.c $(COMMON) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) main.c $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

race-named: main.c $(COMMON) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DRACE_MODE=1 main.c $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

race-judge: main.c $(COMMON) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DRACE_MODE=2 main.c $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

race-car: car_main.c $(COMMON) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) car_main.c $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

race-mq-judge: main.c $(COMMON) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DRACE_MODE=3 main.c $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

race-mq-car: car_main.c $(COMMON) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DQUEUE_WORKER=1 car_main.c $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

tests/race_tests: tests/test_race.c $(COMMON) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_race.c $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

test: all tests/race_tests
	./tests/race_tests
	python3 tests/test_cli.py ./race
	python3 tests/test_processes.py ./race
	python3 tests/test_animation.py ./race
	python3 tests/test_variants.py
	python3 tests/test_variants.py --queues
	python3 tests/test_tcp.py

test-tcp: network race
	python3 tests/test_tcp.py

test-processes: race
	python3 tests/test_processes.py ./race

test-display: race
	python3 tests/test_animation.py ./race

test-variants: all
	python3 tests/test_variants.py
	python3 tests/test_variants.py --queues

clean:
	$(RM) race-server race-driver race-watch race race-named race-judge race-car race-mq-judge race-mq-car tests/race_tests
