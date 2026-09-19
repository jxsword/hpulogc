/* -*- coding: utf-8 -*- */

/**
 * @file win32_time.c
 * @brief Win32 implementation of the clock and time conversion contract.
 *
 * Monotonic time uses QueryPerformanceCounter; realtime uses
 * GetSystemTimePreciseAsFileTime (>= Windows 8) with the 1601-01-01
 * FILETIME epoch converted to the Unix epoch. Civil-time conversion uses
 * the secure CRT helpers (gmtime_s / localtime_s / _mktime64).
 */

#include "platform/platform.h"

#include <errno.h>
#include <time.h>

/**
 * @brief QPC frequency cached on first use (constant for a boot session).
 */
static int64_t qpc_freq(void)
{
    static int64_t freq = 0;
    LARGE_INTEGER f;

    if (freq == 0) {
        if (!QueryPerformanceFrequency(&f) || f.QuadPart == 0) {
            return 10000000; /* defensive default (100ns granularity) */
        }
        freq = (int64_t)f.QuadPart;
    }
    return freq;
}

uint64_t hpu_now_ns(void)
{
    LARGE_INTEGER c;

    if (!QueryPerformanceCounter(&c)) {
        return 0;
    }
    /* Multiply first for precision; the split keeps 64-bit math safe for
     * any realistic uptime (freq * seconds fits comfortably). */
    return (uint64_t)(((__int64)c.QuadPart * 1000000000LL) / qpc_freq());
}

/** @brief 100ns intervals between the 1601 FILETIME and 1970 Unix epochs. */
#define HPU_EPOCH_DIFF_100NS 116444736000000000ULL

int64_t hpu_realtime_ns(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;

    GetSystemTimePreciseAsFileTime(&ft);
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart < HPU_EPOCH_DIFF_100NS) {
        return 0; /* pre-1970 clock: clamp */
    }
    return (int64_t)((u.QuadPart - HPU_EPOCH_DIFF_100NS) * 100ULL);
}

void hpu_localtime(int64_t epoch_sec, hpu_tm_t* out, int use_utc)
{
    struct tm tm_buf;
    __time64_t tt;

    if (out == NULL) {
        return;
    }
    tt = (__time64_t)epoch_sec;

    if (use_utc) {
        if (gmtime_s(&tm_buf, &tt) != 0) {
            memset(out, 0, sizeof(*out));
            return;
        }
    } else {
        if (localtime_s(&tm_buf, &tt) != 0) {
            memset(out, 0, sizeof(*out));
            return;
        }
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

    memset(&tm_buf, 0, sizeof(tm_buf));
    tm_buf.tm_year  = tm->year - 1900;
    tm_buf.tm_mon   = tm->mon - 1;
    tm_buf.tm_mday  = tm->day;
    tm_buf.tm_hour  = tm->hour;
    tm_buf.tm_min   = tm->min;
    tm_buf.tm_sec   = tm->sec;
    tm_buf.tm_isdst = -1; /* DST resolved automatically */

    return (int64_t)_mktime64(&tm_buf);
}
