/**
 * @file output.h
 * @brief Internal output target contract: console and file outputs with
 *        rotation, fsync policy and batched writes.
 */

#ifndef HPU_OUTPUT_H
#define HPU_OUTPUT_H

#include <stddef.h>
#include <stdint.h>

#include "hpulogc.h"

/** @brief Maximum bytes pending in a file output's write buffer before a
 *         flush is forced. */
#define HPU_OUT_IO_BUF_SIZE (64U * 1024U)

/** @brief fsync severity ordering (spec 9): none < shutdown < periodic <
 *         entry. Values map to hpulogc_crash_safety_t. */
#define HPU_FSYNC_SEV_NONE     0
#define HPU_FSYNC_SEV_SHUTDOWN 1
#define HPU_FSYNC_SEV_PERIODIC 2
#define HPU_FSYNC_SEV_ENTRY    3

/**
 * @brief Map a hpulogc_crash_safety_t value to its severity rank.
 * @param cs  hpulogc_crash_safety_t value.
 * @return    Severity rank (HPU_FSYNC_SEV_*).
 */
int hpu_fsync_severity(int cs);

/** @brief Rotation configuration (parsed from the output config). */
typedef struct hpu_rotate_cfg {
    int    enabled;   /*!< rotate != NONE */
    int    by_size;   /*!< SIZE or BOTH */
    int    by_time;   /*!< TIME or BOTH */
    size_t max_size;  /*!< Size threshold in bytes */
    int    time_unit; /*!< hpulogc_time_unit_t */
    int    max_files; /*!< Max archived files, 0 = unlimited */
    char   naming[HPULOGC_MAX_FMT_LEN]; /*!< Archive naming template */
} hpu_rotate_cfg_t;

/**
 * @brief Common output header embedded at offset 0 of every backend.
 */
typedef struct hpu_output_base {
    int type;                        /*!< hpulogc_output_type_t */
    char name[HPULOGC_MAX_NAME_LEN]; /*!< Config key / generated name */
    int  fsync_sev;                  /*!< Effective fsync severity */
} hpu_output_base_t;

/** @brief Opaque output handle (hpu_output_base_t at offset 0). */
typedef struct hpu_output hpu_output_t;

/* ---- Factory / dispatcher (output.c) ---- */

hpu_output_t* hpu_output_open(const hpulogc_output_t* cfg,
                              int effective_fsync);
void hpu_output_close(hpu_output_t* o);
int hpu_output_write_line(hpu_output_t* o, const char* line, size_t len,
                          int64_t ts_sec, int level);
int hpu_output_flush(hpu_output_t* o);
int hpu_output_sync(hpu_output_t* o);
int hpu_output_periodic(hpu_output_t* o, uint64_t now_ns,
                        uint64_t interval_ns);
const char* hpu_output_name(const hpu_output_t* o);

/* ---- Console backend (output_console.c) ---- */

hpu_output_t* hpu_console_output_open(const hpulogc_output_t* cfg,
                                      int effective_fsync);
int hpu_console_output_write_line(hpu_output_t* o, const char* line,
                                  size_t len, int level);
int hpu_console_output_flush(hpu_output_t* o);
int hpu_output_write_direct(hpu_output_t* o, const char* bytes, size_t len);
void hpu_output_close_impl(hpu_output_t* o);

/* ---- File backend (output_file.c) ---- */

hpu_output_t* hpu_file_output_open(const hpulogc_output_t* cfg,
                                   int effective_fsync);
void hpu_file_output_close(hpu_output_t* o);
int hpu_file_output_write_line(hpu_output_t* o, const char* line,
                               size_t len);
int hpu_file_output_flush(hpu_output_t* o);
int hpu_file_output_fsync(hpu_output_t* o);
int hpu_file_output_flush_fsync(hpu_output_t* o);
int hpu_file_output_periodic(hpu_output_t* o, uint64_t now_ns,
                             uint64_t interval_ns);
int hpu_file_output_before_write(hpu_output_t* o, int64_t ts_sec);

/** @brief Cumulative lines lost to write failures (core stats). */
unsigned long long hpu_output_lost_total(hpu_output_t* o);

/* Test-only I/O failure injection (see docs/implementation_notes.md).
 * op: 0 = open, 1 = write, 2 = fsync; return non-zero to force the
 * operation to fail. NULL (default) = never fail. */
extern int (*hpu_io_fail_hook)(int op, const char* path);

/** @brief Set the timezone mode for buckets/naming (config layer). */
void hpu_output_set_utc(hpu_output_t* o, int use_utc);

#endif /* HPU_OUTPUT_H */
