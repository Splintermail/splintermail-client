#include "server/libkvpsync/libkvpsync.h"

#include <time.h>

xtime_t xtime(void){
    struct timespec ts;
    int ret = clock_gettime(CLOCK_REALTIME, &ts);
    if(ret) LOG_FATAL("clock_gettime: %x\n", FE(&errno));
    if(ts.tv_sec < 0) LOG_FATAL("clock_gettime: negative tv_sec!\n");
    if(ts.tv_nsec < 0) LOG_FATAL("clock_gettime: negative tv_nsec!\n");
    return 1000*1000*1000*(xtime_t)ts.tv_sec + (xtime_t)ts.tv_nsec;
}
