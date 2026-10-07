CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2

.PHONY: all test clean

all: race

race: main.c
	$(CC) $(CFLAGS) main.c -o race

tests/race_tests: tests/test_race.c main.c
	$(CC) $(CFLAGS) tests/test_race.c -o tests/race_tests

test: race tests/race_tests
	./tests/race_tests
	python3 tests/test_cli.py ./race
	python3 tests/test_animation.py ./race

clean:
	$(RM) race tests/race_tests
