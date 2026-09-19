/**
 * @file hpu_time.h
 * @brief Platform contract: high-resolution clocks and local time conversion.
 */

#ifndef HPU_TIME_H
#define HPU_TIME_H

#include <stdint.h>

/**
 * @brief Broken-down civil time (subset of struct tm, conversion helper).
 */
typedef struct hpu_tm {
    int year;  /*!< Full year (e.g. 2026) */
    int mon;   /*!< Month 1-12 */
    int day;   /*!< Day of month 1-31 */
    int hour;  /*!< Hour 0-23 */
    int min;   /*!< Minute 0-59 */
    int sec;   /*!< Second 0-60 (leap second aware) */
    int wday;  /*!< Days since Sunday, 0-6 */
    int yday;  /*!< Days since January 1, 0-365 */
} hpu_tm_t;

/**
 * @brief Monotonic high-resolution clock, nanoseconds since an arbitrary
 *        fixed epoch. Never goes backwards; used for intervals and
 *        benchmarks.
 * @return Nanoseconds (unsigned wrap in ~584 years).
 */
uint64_t hpu_now_ns(void);

/**
 * @brief Realtime wall clock, nanoseconds since the Unix epoch.
 * @return Nanoseconds since 1970-01-01T00:00:00Z (may jump with clock
 *         adjustments).
 */
int64_t hpu_realtime_ns(void);

/**
 * @brief Convert an epoch timestamp to civil time.
 *
 * @param epoch_sec  Seconds since the Unix epoch.
 * @param out        Filled with the broken-down time.
 * @param use_utc    Non-zero for UTC, zero for the local timezone.
 */
void hpu_localtime(int64_t epoch_sec, hpu_tm_t* out, int use_utc);

/**
 * @brief Inverse of hpu_localtime() in the local timezone.
 *
 * Interprets the civil fields of @p tm as local time (DST resolved
 * automatically) and returns the epoch seconds.
 *
 * @param tm  Civil time to convert.
 * @return    Seconds since the Unix epoch; -1 when not representable.
 */
int64_t hpu_mktime_local(const hpu_tm_t* tm);

#endif /* HPU_TIME_H */
