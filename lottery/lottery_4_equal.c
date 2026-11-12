/* Fairness test: 4 workers with equal weights, 30 seconds. */
#define LOTTERY_NUM_WORKERS 4
#define LOTTERY_SECONDS 30
#define LOTTERY_TICKETS { 10, 10, 10, 10 }
#define LOTTERY_PROFILE "4 workers | equal 10:10:10:10 tickets | 30 seconds"
#include "lottery_common.h"
