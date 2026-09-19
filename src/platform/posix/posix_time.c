/**
 * @file posix_time.c
 * @brief POSIX implementation of the clock and time conversion contract.
 */

#include "platform/platform.h"

#include <errno.h>
#include <time.h>

uint64_t hpu_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int64_t hpu_realtime_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

void hpu_localtime(int64_t epoch_sec, hpu_tm_t* out, int use_utc)
{
    struct tm tm_buf;
    time_t tt;

    if (out == NULL) {
        return;
    }

    /* Contract (hpu_time.h): epoch_sec is seconds since the Unix epoch.
     * (Phase 1 defect E fix: previously divided by 1e9, which truncated
     * second-granularity callers to the 1970 epoch; recorded in
     * docs/implementation_notes.md.) */
    tt = (time_t)epoch_sec;

    if (use_utc) {
        gmtime_r(&tt, &tm_buf);
    } else {
        localtime_r(&tt, &tm_buf);
    }

    out->year = tm_buf.tm_year + 1900;
    out->mon  = tm_buf.tm_mon + 1;
    out->day  = tm_buf.tm_mday;
    out->hour = tm_buf.tm_hour;
    out->min  = tm_buf.tm_min;
    out->sec  = tm_buf.tm_sec;
    out->wday = tm_buf.tm_wday;
    out->yday = tm_buf.tm_yday;
}

int64_t hpu_mktime_local(const hpu_tm_t* tm)
{
    struct tm tm_buf;

    if (tm == NULL) {
        return -1;
    }

    tm_buf.tm_year = tm->year - 1900;
    tm_buf.tm_mon  = tm->mon - 1;
    tm_buf.tm_mday = tm->day;
    tm_buf.tm_hour = tm->hour;
    tm_buf.tm_min  = tm->min;
    tm_buf.tm_sec  = tm->sec;
    tm_buf.tm_isdst = -1;

    return (int64_t)mktime(&tm_buf);
}
