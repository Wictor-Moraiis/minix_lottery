/*
 * Interactive Lottery Scheduling benchmark.
 * User chooses 2..8 workers and each worker's ticket weight.
 * Measurement duration remains fixed at 30 seconds.
 */
#define LOTTERY_NUM_WORKERS 8
#define LOTTERY_SECONDS 30
#define LOTTERY_TICKETS { 1, 1, 1, 1, 1, 1, 1, 1 }
#define LOTTERY_PROFILE "custom | interactive workers and ticket weights | 30 seconds"
#define LOTTERY_CUSTOM_INPUT
#include "lottery_common.h"
