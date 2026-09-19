/**
 * @file output.c
 * @brief Output dispatcher: batching policy and backend choice.
 */

#include "output.h"

#include <stdlib.h>
#include <string.h>

int hpu_fsync_severity(int cs)
{
    switch (cs) {
    case HPULOGC_CRASH_NONE:
        return HPU_FSYNC_SEV_NONE;
    case HPULOGC_CRASH_SHUTDOWN:
        return HPU_FSYNC_SEV_SHUTDOWN;
    case HPULOGC_CRASH_PERIODIC:
        return HPU_FSYNC_SEV_PERIODIC;
    case HPULOGC_CRASH_ENTRY:
        return HPU_FSYNC_SEV_ENTRY;
    default:
        return HPU_FSYNC_SEV_NONE;
    }
}

hpu_output_t* hpu_output_open(const hpulogc_output_t* cfg,
                              int effective_fsync)
{
    if (cfg == NULL) {
        return NULL;
    }
    if (cfg->type == HPULOGC_OUT_FILE) {
        return hpu_file_output_open(cfg, effective_fsync);
    }
    return hpu_console_output_open(cfg, effective_fsync);
}

void hpu_output_close(hpu_output_t* o)
{
    if (o == NULL) {
        return;
    }
    hpu_output_flush(o);
    hpu_output_close_impl(o);
}

int hpu_output_write_line(hpu_output_t* o, const char* line, size_t len,
                          int64_t ts_sec, int level)
{
    hpu_output_base_t* b = (hpu_output_base_t*)(void*)o;

    if (o == NULL || len == 0) {
        return 0;
    }
    if (b->type == HPULOGC_OUT_FILE) {
        /* Rotation check before the line is committed (O(1)). */
        if (hpu_file_output_before_write(o, ts_sec) != 0) {
            return -1; /* the line is lost; caller counts a drop */
        }
        return hpu_file_output_write_line(o, line, len);
    }
    return hpu_console_output_write_line(o, line, len, level);
}

int hpu_output_flush(hpu_output_t* o)
{
    hpu_output_base_t* b;

    if (o == NULL) {
        return 0;
    }
    b = (hpu_output_base_t*)(void*)o;
    if (b->type == HPULOGC_OUT_FILE) {
        return hpu_file_output_flush(o);
    }
    return hpu_console_output_flush(o);
}

int hpu_output_sync(hpu_output_t* o)
{
    if (o == NULL) {
        return 0;
    }
    if (hpu_output_flush(o) != 0) {
        return -1;
    }
    if (((hpu_output_base_t*)(void*)o)->type == HPULOGC_OUT_FILE) {
        return hpu_file_output_fsync(o);
    }
    return 0;
}

int hpu_output_periodic(hpu_output_t* o, uint64_t now_ns,
                        uint64_t interval_ns)
{
    if (o == NULL) {
        return 0;
    }
    if (((hpu_output_base_t*)(void*)o)->type != HPULOGC_OUT_FILE) {
        return 0;
    }
    return hpu_file_output_periodic(o, now_ns, interval_ns);
}

unsigned long long hpu_output_lost_total(hpu_output_t* o)
{
    if (o == NULL) {
        return 0;
    }
    if (((hpu_output_base_t*)(void*)o)->type != HPULOGC_OUT_FILE) {
        return 0;
    }
    {
        extern unsigned long long hpu_file_output_lost_total(hpu_output_t*);
        return hpu_file_output_lost_total(o);
    }
}

void hpu_output_set_utc(hpu_output_t* o, int use_utc)
{
    if (o == NULL) {
        return;
    }
    if (((hpu_output_base_t*)(void*)o)->type != HPULOGC_OUT_FILE) {
        return;
    }
    {
        extern void hpu_file_output_set_utc(hpu_output_t*, int);
        hpu_file_output_set_utc(o, use_utc);
    }
}

const char* hpu_output_name(const hpu_output_t* o)
{
    if (o == NULL) {
        return "(null)";
    }
    return ((hpu_output_base_t*)(void*)o)->name;
}
