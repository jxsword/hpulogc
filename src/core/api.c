/* -*- coding: utf-8 -*- */

/**
 * @file api.c
 * @brief Public log-write entry points: errno preservation and forwarding
 *        into the pipeline (spec 7.3/9), plus the error-description API
 *        (spec 7.3/7.4, decision D-R7).
 */

#include "core_internal.h"

#include <stdio.h>
#include <string.h>

void hpulogc_vlog(hpulogc_level_t level, const char* category,
                  const char* file, int line, const char* func,
                  const char* fmt, va_list ap)
{
    hpu_pipeline_submit(level, category, file, line, func, fmt, ap);
}

void hpulogc_log(hpulogc_level_t level, const char* category,
                 const char* file, int line, const char* func,
                 const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    hpu_pipeline_submit(level, category, file, line, func, fmt, ap);
    va_end(ap);
}

void hpulogc_vlog_ex(hpulogc_level_t level, const char* category,
                     const char* file, int line, const char* func,
                     const hpulogc_field_t* fields, size_t field_count,
                     const char* fmt, va_list ap)
{
    hpu_pipeline_submit_ex(level, category, file, line, func, fields,
                           field_count, fmt, ap);
}

void hpulogc_log_ex(hpulogc_level_t level, const char* category,
                    const char* file, int line, const char* func,
                    const hpulogc_field_t* fields, size_t field_count,
                    const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    hpu_pipeline_submit_ex(level, category, file, line, func, fields,
                           field_count, fmt, ap);
    va_end(ap);
}

/* Descriptions are plain ASCII so hpulogc_strerror stays locale-independent
 * and needs no dynamic allocation (rd_v0.6 §7.3, D-R7). */
static const char* const g_error_descs[] = {
    "success",             /*!< HPULOGC_OK (0) */
    "invalid argument",    /*!< HPULOGC_ERR_INVALID_ARG (-1) */
    "out of memory",       /*!< HPULOGC_ERR_NO_MEM (-2) */
    "I/O error",           /*!< HPULOGC_ERR_IO (-3) */
    "configuration error", /*!< HPULOGC_ERR_CONFIG (-4) */
    "invalid state"        /*!< HPULOGC_ERR_STATE (-5) */
};

int hpulogc_strerror(int code, char* buf, size_t len)
{
    char fallback[32];
    const char* desc;
    int full;

    if (buf == NULL && len != 0) {
        return HPULOGC_ERR_INVALID_ARG;
    }
    if (code <= 0 && code >= -(int)(sizeof(g_error_descs) /
                                    sizeof(g_error_descs[0]))) {
        desc = g_error_descs[-code];
    } else {
        snprintf(fallback, sizeof(fallback), "unknown error %d", code);
        desc = fallback;
    }
    full = (int)strlen(desc);
    /* len == 0 (with or without buf) is the length query: nothing written. */
    if (buf != NULL && len != 0) {
        size_t copy = (size_t)full < len - 1 ? (size_t)full : len - 1;

        memcpy(buf, desc, copy);
        buf[copy] = '\0';
    }
    return full;
}
