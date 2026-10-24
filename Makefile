CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
THREAD_FLAGS = -pthread
SOURCES = race.c display.c threads.c
HEADERS = race.h display.h threads.h
# Зафиксированное состояние ИДЗ №1; извлекается из истории без переключения ветки.
REFERENCE_REF ?= 3480fc4cf98ce7653480a155baa3fd36151f2a16

.PHONY: all test test-shutdown test-display clean

all: race

race: main.c $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) main.c $(SOURCES) $(LDFLAGS) -o $@

tests/race_tests: tests/test_race.c $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_race.c $(SOURCES) $(LDFLAGS) -o $@

tests/thread_tests: tests/test_threads.c $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_threads.c $(SOURCES) $(LDFLAGS) -o $@

test: race tests/race_tests tests/thread_tests
	./tests/race_tests
	./tests/thread_tests
	python3 tests/test_cli.py ./race
	python3 tests/test_animation.py ./race
	python3 tests/test_threads.py ./race $(REFERENCE_REF)
	python3 tests/test_shutdown.py ./race

test-shutdown: race
	python3 tests/test_shutdown.py ./race

test-display: race
	python3 tests/test_animation.py ./race

clean:
	$(RM) race tests/race_tests tests/thread_tests
