/* -*- coding: utf-8 -*- */

/**
 * @file signal_safe.c
 * @brief Async-signal-safe restricted log channel (spec 7.3/9).
 *
 * No locks, no formatting, no memory allocation: the line is composed by
 * bounded byte copies into a preallocated static buffer and written to
 * stderr (fd 2) with a single write. The caller's errno is saved and
 * restored. Disabled (default) the call is a silent no-op.
 */

#include "core_internal.h"

#include <errno.h>
#include <string.h>

/** @brief Channel gate (init with signal_safe = true enables it). */
static hpu_atomic_u32 g_ss_enabled;

/** @brief Preallocated compose buffer (fixed size keeps the channel
 *         allocation-free; longer messages are truncated). */
static char g_ss_buf[512];

/** @brief Level tags, uppercase; index 6 covers invalid levels ("LOG"). */
static const char* const g_ss_tags[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "LOG"
};

void hpu_signal_safe_enable(void)
{
    hpu_at_store_u32(&g_ss_enabled, 1, HPU_MO_RELEASE);
}

void hpu_signal_safe_disable(void)
{
    hpu_at_store_u32(&g_ss_enabled, 0, HPU_MO_RELEASE);
}

void hpulogc_log_signal_safe(hpulogc_level_t level, const char* msg)
{
    int saved_errno = errno;
    const char* tag;
    char* p = g_ss_buf;
    size_t left = sizeof(g_ss_buf);
    size_t n;
    size_t msg_len;

    if (hpu_at_load_u32(&g_ss_enabled, HPU_MO_ACQUIRE) == 0) {
        return; /* gated off: silent drop without touching errno */
    }
    if (msg == NULL) {
        msg = "";
    }

    tag = (level >= HPULOGC_LEVEL_TRACE && level <= HPULOGC_LEVEL_FATAL)
              ? g_ss_tags[level]
              : g_ss_tags[6];

    /* Manual bounded compose (no snprintf: async-signal-safe by design). */
#define SS_APPEND(bytes, len)                 \
    do {                                      \
        size_t nn_ = (len);                   \
        if (nn_ > left) {                     \
            nn_ = left;                       \
        }                                     \
        memcpy(p, (bytes), nn_);              \
        p += nn_;                             \
        left -= nn_;                          \
    } while (0)

    SS_APPEND("[hpulogc][", 10);
    n = strlen(tag);
    SS_APPEND(tag, n);
    SS_APPEND("] ", 2);
    msg_len = strlen(msg);
    if (msg_len > left - 2) {
        msg_len = left - 2; /* room for the newline */
    }
    SS_APPEND(msg, msg_len);
    SS_APPEND("\n", 1);
#undef SS_APPEND

    (void)hpu_fs_write(2, g_ss_buf, (size_t)(p - g_ss_buf));
    errno = saved_errno;
}
