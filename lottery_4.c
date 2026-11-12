/* Scaling test: 4 workers, different ticket weights, 30 seconds. */
#define LOTTERY_NUM_WORKERS 4
#define LOTTERY_SECONDS 30
#define LOTTERY_TICKETS { 10, 20, 30, 40 }
#define LOTTERY_PROFILE "4 workers | 10:20:30:40 tickets | 30 seconds"
#include "lottery_common.h"
