/**
 * @file rotate.h
 * @brief Internal rotation helpers: archive naming, next time boundary,
 *        cleanup of old archives and the .latest symlink.
 */

#ifndef HPU_ROTATE_H
#define HPU_ROTATE_H

#include <stddef.h>
#include <stdint.h>

#include "output.h"

/** @brief Default archive naming template (spec 4.6). */
#define HPU_ROTATE_DEFAULT_NAMING "{base}.{timestamp}.{index}.log"

/** @brief Sort key / name of one archived file. */
typedef struct hpu_rotate_key {
    int64_t ts;                        /*!< Timestamp key (0 if absent) */
    long index;                        /*!< Index key (0 if absent) */
    char name[HPULOGC_MAX_PATH_LEN];   /*!< File name */
} hpu_rotate_key_t;

/**
 * @brief Compute the next time-bucket boundary (O(1) per check; this
 *        runs once per bucket).
 *
 * @param ts_sec   Current wall-clock seconds.
 * @param time_unit  hpulogc_time_unit_t value.
 * @param use_utc  Non-zero for UTC buckets, else local timezone.
 * @return         Epoch seconds of the next boundary (> ts_sec).
 */
int64_t hpu_rotate_next_boundary(int64_t ts_sec, int time_unit, int use_utc);

/**
 * @brief Rotate the active file to an archive name and start a new one.
 *
 * Renames the active file to the template-generated archive name
 * (increasing {index} on conflicts), opens a fresh active file, updates
 * the .latest symlink and prunes old archives beyond max_files.
 *
 * @param path        Active file path.
 * @param fd          In/out active descriptor (closed and reopened).
 * @param file_size   In/out active file size (reset to <= 0).
 * @param cfg         Rotation configuration.
 * @param use_utc     Timezone mode for {timestamp} and buckets.
 * @param ts_sec      Rotation trigger time (epoch seconds).
 * @param symlink_latest  Non-zero to maintain <path>.latest.
 * @param file_mode   Permission bits for the new active file.
 * @return            0 on success, -1 when the new file could not be
 *                    created (the old file was already renamed).
 */
int hpu_rotate_file(const char* path, int* fd, int64_t* file_size,
                    const hpu_rotate_cfg_t* cfg, int use_utc,
                    int64_t ts_sec, int symlink_latest, unsigned file_mode);

#endif /* HPU_ROTATE_H */
