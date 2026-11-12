/*
 * Interactive Lottery Scheduling benchmark.
 * User chooses 2..8 workers and each worker's ticket weight.
 * User chooses the measurement duration (1 to 3600 seconds).
 */
#define LOTTERY_NUM_WORKERS 8
#define LOTTERY_SECONDS 30
#define LOTTERY_TICKETS { 1, 1, 1, 1, 1, 1, 1, 1 }
#define LOTTERY_PROFILE "custom | interactive workers, ticket weights and duration"
#define LOTTERY_CUSTOM_INPUT
#include "lottery_common.h"
