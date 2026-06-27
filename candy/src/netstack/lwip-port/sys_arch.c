#include "lwip/opt.h"
#include "lwip/sys.h"

#include <stdlib.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

u32_t lwip_port_rand(void) {
    return (u32_t)rand();
}

static void get_monotonic_time(struct timespec *ts) {
#if defined(__APPLE__)
    u64_t t = mach_absolute_time();
    mach_timebase_info_data_t timebase_info = {0, 0};
    mach_timebase_info(&timebase_info);
    u64_t nano = (t * timebase_info.numer) / (timebase_info.denom);
    u64_t sec = nano / 1000000000L;
    nano -= sec * 1000000000L;
    ts->tv_sec = sec;
    ts->tv_nsec = nano;
#else
    clock_gettime(CLOCK_MONOTONIC, ts);
#endif
}

u32_t sys_now(void) {
    struct timespec ts;
    get_monotonic_time(&ts);
    return (u32_t)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

u32_t sys_jiffies(void) {
    struct timespec ts;
    get_monotonic_time(&ts);
    return (u32_t)(ts.tv_sec * 1000000000L + ts.tv_nsec);
}
