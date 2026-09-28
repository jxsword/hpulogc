/* -*- coding: utf-8 -*- */

/**
 * @file api.c
 * @brief Public log-write entry points: errno preservation and forwarding
 *        into the pipeline (spec 7.3/9).
 */

#include "core_internal.h"

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
