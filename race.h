#ifndef RACE_H
#define RACE_H

#define MAX_TEAMS 8
#define MAX_CARS_PER_TEAM 4
#define MAX_CARS (MAX_TEAMS * MAX_CARS_PER_TEAM)
#define MAX_LANES 6
#define MAX_LENGTH 120
#define MAX_STEPS (MAX_CARS + 4)

enum { ACTIVE, FINISHED, RETIRED, UNFINISHED };
enum { FORWARD, INSIDE, OUTSIDE };
enum { GREEDY, RANDOM };

typedef struct {
    int teams;
    int cars_per_team;
    int lanes;
    int length;
    int laps;
    int max_rounds;
    int strategy;
    unsigned int seed;
    int quiet;
    const char *log_path;
    int delay_ms;
    int no_animation;
} Config;

typedef struct {
    int id;
    int team;
    int lane;
    int position;
    int distance;       // Продвижение относительно стартовой линии; на старте отрицательное.
    int status;
    int zero_move;
    int finish_place;
} Car;

typedef struct {
    int directions[MAX_STEPS];
    int count;
    int cost;
} Route;

typedef struct {
    Config config;
    Car cars[MAX_CARS];
    int track[MAX_LANES][MAX_LENGTH]; // 0 — свободно, другое число — номер машины.
    int car_count;
    int finish_count;
    int rounds;
    int order[MAX_CARS];
    int order_count;
} Race;

const char *status_name(int status);
void initialize_race(Race *race, Config config);
int active_count(const Race *race);
int ahead(const Car *a, const Car *b);
int create_order(const Race *race, int order[MAX_CARS]);
int step_cost(int direction);
void destination(const Config *config, int direction, int *lane, int *position);
Route choose_route(const Race *race, const Car *car, int limit, int *blocked);
void execute_route(Race *race, Car *car, const Route *route, int limit);
void finish_round(Race *race);
void begin_round(Race *race);
int perform_turn(Race *race, int car_index, int limit);
void end_round(Race *race);
int result_before(const Car *a, const Car *b);
void create_results(const Race *race, int order[MAX_CARS]);
void print_results(const Race *race, const char *reason);

#endif
