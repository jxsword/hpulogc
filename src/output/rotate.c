/**
 * @file rotate.c
 * @brief Rotation implementation: naming template expansion, time bucket
 *        boundaries, archive cleanup and the .latest symlink.
 */

#include "rotate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Phase 1 defect C fix (docs/implementation_notes.md): <unistd.h> is not
 * available under MSVC; _getpid() provides the same value. */
#if defined(_MSC_VER)
#include <process.h>
#define hpu_getpid() ((long)_getpid())
#else
#include <unistd.h>
#define hpu_getpid() ((long)getpid())
#endif

#include "../platform/platform.h"

/* ------------------------------------------------------------------ */
/* Time bucket boundaries                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Days from 1970-01-01 to a civil date (Howard Hinnant's algorithm).
 */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    /* Phase 1 defect H fix: the original `(m > 2 ? -3u : 9u)` applied a
     * unary minus to unsigned (MSVC /W4 C4146); the shifted month is
     * computed in signed arithmetic instead. */
    int m_shift = (int)m + (m > 2 ? -3 : 9);
    unsigned doy = ((unsigned)(153u * (unsigned)m_shift + 2u) / 5u) + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;

    return era * 146097 + (int64_t)doe - 719468;
}

/**
 * @brief Convert a day count to a civil date (year/month/day).
 */
static void civil_from_days(int64_t z, int64_t* y, unsigned* m, unsigned* d)
{
    z += 719468;
    {
        int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        int64_t doe = z - era * 146097;
        int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int64_t yy = yoe + era * 400;
        int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        int64_t mp = (5 * doy + 2) / 153;
        int64_t dd = doy - (153 * mp + 2) / 5 + 1;
        int64_t mm = mp + (mp < 10 ? 3 : -9);

        *y = yy + (mm <= 2);
        *m = (unsigned)mm;
        *d = (unsigned)dd;
    }
}

int64_t hpu_rotate_next_boundary(int64_t ts_sec, int time_unit, int use_utc)
{
    if (use_utc) {
        /* UTC boundaries are exact civil arithmetic. */
        int64_t day = ts_sec / 86400;

        switch (time_unit) {
        case HPULOGC_TU_HOUR:
            return ((ts_sec / 3600) + 1) * 3600;
        case HPULOGC_TU_DAY:
            return (day + 1) * 86400;
        case HPULOGC_TU_WEEK: {
            /* 1970-01-01 was a Thursday (wday 4 with Sunday=0). */
            int wday = (int)((day + 4) % 7);
            int days_to_monday = (1 - wday + 7) % 7;
            int64_t boundary = (day + days_to_monday) * 86400;

            if (boundary <= ts_sec) {
                boundary += 7 * 86400;
            }
            return boundary;
        }
        case HPULOGC_TU_MONTH:
        default: {
            int64_t y;
            unsigned m;
            unsigned d;

            civil_from_days(day, &y, &m, &d);
            m += 1;
            if (m > 12) {
                m = 1;
                y += 1;
            }
            return days_from_civil(y, m, 1) * 86400;
        }
        }
    }

    {
        /* Local timezone: use the platform conversion helpers. */
        hpu_tm_t tm;
        int64_t base;

        hpu_localtime(ts_sec, &tm, 0);

        switch (time_unit) {
        case HPULOGC_TU_HOUR:
            tm.min = 0;
            tm.sec = 0;
            tm.hour += 1;
            break;
        case HPULOGC_TU_DAY:
            tm.hour = 0;
            tm.min = 0;
            tm.sec = 0;
            tm.day += 1;
            break;
        case HPULOGC_TU_WEEK: {
            int days_to_monday = (1 - tm.wday + 7) % 7;

            tm.hour = 0;
            tm.min = 0;
            tm.sec = 0;
            tm.day += days_to_monday;
            base = hpu_mktime_local(&tm);
            while (base <= ts_sec) {
                tm.day += 7;
                base = hpu_mktime_local(&tm);
            }
            return base;
        }
        case HPULOGC_TU_MONTH:
        default:
            tm.hour = 0;
            tm.min = 0;
            tm.sec = 0;
            tm.day = 1;
            tm.mon += 1;
            break;
        }
        return hpu_mktime_local(&tm);
    }
}

/* ------------------------------------------------------------------ */
/* Archive naming                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Extract the file name without its final extension ({base}).
 */
static void rotate_base_name(const char* path, char* out, size_t outsz)
{
    if (hpu_path_stem(path, out, outsz) != 0) {
        snprintf(out, outsz, "log");
    }
}

/**
 * @brief Build an archive name from the template (directory-prefixed).
 * @param index  {index} value; ignored (not emitted) when the template
 *               lacks the placeholder.
 */
static void rotate_build_name(const hpu_rotate_cfg_t* cfg, const char* path,
                              int64_t ts_sec, int use_utc, unsigned long index,
                              char* out, size_t outsz)
{
    char base[HPULOGC_MAX_PATH_LEN];
    char ts[32];
    char dir[HPULOGC_MAX_PATH_LEN];
    char name_part[HPULOGC_MAX_PATH_LEN];
    const char* p = cfg->naming;
    char* w = name_part;
    size_t left = sizeof(name_part) - 1;

    rotate_base_name(path, base, sizeof(base));

    {
        hpu_tm_t tm;

        hpu_localtime(ts_sec, &tm, use_utc);
        snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d", tm.year,
                 tm.mon, tm.day, tm.hour, tm.min, tm.sec);
    }

    out[0] = '\0';
    while (*p != '\0' && left > 0) {
        if (strncmp(p, "{base}", 6) == 0) {
            size_t n = strlen(base);

            if (n > left) {
                n = left;
            }
            memcpy(w, base, n);
            w += n;
            left -= n;
            p += 6;
        } else if (strncmp(p, "{timestamp}", 11) == 0) {
            size_t n = strlen(ts);

            if (n > left) {
                n = left;
            }
            memcpy(w, ts, n);
            w += n;
            left -= n;
            p += 11;
        } else if (strncmp(p, "{index}", 7) == 0) {
            int n = snprintf(w, left + 1, "%lu", index);

            if (n < 0) {
                break;
            }
            if ((size_t)n > left) {
                n = (int)left;
            }
            w += n;
            left -= (size_t)n;
            p += 7;
        } else {
            *w++ = *p++;
            left--;
        }
    }
    *w = '\0';

    /* Prefix the archive with the directory of the active path. */
    if (hpu_path_dirname(path, dir, sizeof(dir)) == 0 &&
        strcmp(dir, ".") != 0) {
        snprintf(out, outsz, "%s/%s", dir, name_part);
    } else {
        snprintf(out, outsz, "%s", name_part);
    }
}

/**
 * @brief Create or refresh the <path>.latest symlink atomically.
 */
static void rotate_update_symlink(const char* path)
{
    char dir[HPULOGC_MAX_PATH_LEN];
    char base[HPULOGC_MAX_PATH_LEN];
    char linkpath[HPULOGC_MAX_PATH_LEN + 16];
    char tmppath[HPULOGC_MAX_PATH_LEN + 32];

    if (hpu_path_dirname(path, dir, sizeof(dir)) != 0 ||
        hpu_path_basename(path, base, sizeof(base)) != 0) {
        return;
    }
    snprintf(linkpath, sizeof(linkpath), "%s/%s.latest", dir, base);
    snprintf(tmppath, sizeof(tmppath), "%s/%s.latest.tmp.%ld", dir, base,
             hpu_getpid());

    hpu_fs_unlink(tmppath);
    if (hpu_fs_symlink(base, tmppath) == 0) {
        /* rename() replaces the existing symlink atomically. */
        (void)hpu_fs_rename(tmppath, linkpath);
    }
}

/**
 * @brief Match an archived file name against the naming template and
 *        extract its sort keys.
 * @return 1 when the name matches, 0 otherwise.
 */
static int rotate_match_archive(const hpu_rotate_cfg_t* cfg,
                                const char* base, const char* name,
                                int64_t* ts_out, long* index_out)
{
    const char* p = cfg->naming;
    const char* n = name;

    *ts_out = 0;
    *index_out = 0;

    while (*p != '\0') {
        if (strncmp(p, "{base}", 6) == 0) {
            size_t bl = strlen(base);

            if (strncmp(n, base, bl) != 0) {
                return 0;
            }
            n += bl;
            p += 6;
        } else if (strncmp(p, "{timestamp}", 11) == 0) {
            int64_t ymd = 0, hms = 0;
            int i;

            for (i = 0; i < 8; i++, n++) {
                if (*n < '0' || *n > '9') {
                    return 0;
                }
                ymd = ymd * 10 + (*n - '0');
            }
            if (*n != '_') {
                return 0;
            }
            n++;
            for (i = 0; i < 6; i++, n++) {
                if (*n < '0' || *n > '9') {
                    return 0;
                }
                hms = hms * 10 + (*n - '0');
            }
            *ts_out = ymd * 1000000 + hms;
            p += 11;
        } else if (strncmp(p, "{index}", 7) == 0) {
            long idx = 0;

            if (*n < '0' || *n > '9') {
                return 0;
            }
            while (*n >= '0' && *n <= '9') {
                idx = idx * 10 + (*n - '0');
                n++;
            }
            *index_out = idx;
            p += 7;
        } else {
            if (*n != *p) {
                return 0;
            }
            n++;
            p++;
        }
    }
    return *n == '\0';
}

/**
 * @brief Comparison for descending sort of archive keys.
 */
static int rotate_cmp_desc(const void* a, const void* b)
{
    const hpu_rotate_key_t* ka = a;
    const hpu_rotate_key_t* kb = b;

    if (ka->ts != kb->ts) {
        return ka->ts > kb->ts ? -1 : 1;
    }
    return ka->index > kb->index ? -1 : (ka->index < kb->index ? 1 : 0);
}

int hpu_rotate_prune(const char* path, const hpu_rotate_cfg_t* cfg,
                     int use_utc)
{
    (void)use_utc; /* sorting keys carry the timezone already */
    char dir[HPULOGC_MAX_PATH_LEN];
    char base[HPULOGC_MAX_PATH_LEN];
    char fname[HPULOGC_MAX_PATH_LEN];
    hpu_dir_entry_t* entries = NULL;
    size_t count = 0;
    size_t n_keys = 0;
    hpu_rotate_key_t* keys = NULL;
    size_t i;
    int rc = 0;

    if (cfg->max_files <= 0 || !cfg->enabled) {
        return 0;
    }
    if (hpu_path_dirname(path, dir, sizeof(dir)) != 0 ||
        hpu_path_basename(path, fname, sizeof(fname)) != 0 ||
        hpu_path_stem(path, base, sizeof(base)) != 0) {
        return -1;
    }
    if (hpu_fs_list_dir(dir, &entries, &count) != 0) {
        return -1;
    }

    keys = calloc(count, sizeof(*keys));
    if (keys == NULL) {
        hpu_fs_list_free(entries, count);
        return -1;
    }
    for (i = 0; i < count; i++) {
        int64_t ts = 0;
        long idx = 0;

        if (strcmp(entries[i].name, fname) == 0) {
            continue; /* never prune the active file */
        }
        if (rotate_match_archive(cfg, base, entries[i].name, &ts, &idx)) {
            keys[n_keys].ts = ts;
            keys[n_keys].index = idx;
            snprintf(keys[n_keys].name, sizeof(keys[n_keys].name), "%s",
                     entries[i].name);
            n_keys++;
        }
    }
    hpu_fs_list_free(entries, count);

    if (n_keys > (size_t)cfg->max_files) {
        qsort(keys, n_keys, sizeof(*keys), rotate_cmp_desc);
        for (i = (size_t)cfg->max_files; i < n_keys; i++) {
            char full[HPULOGC_MAX_PATH_LEN * 2];

            snprintf(full, sizeof(full), "%s/%s", dir, keys[i].name);
            if (hpu_fs_unlink(full) != 0) {
                rc = -1;
            }
        }
    }
    free(keys);
    return rc;
}

int hpu_rotate_file(const char* path, int* fd, int64_t* file_size,
                    const hpu_rotate_cfg_t* cfg, int use_utc,
                    int64_t ts_sec, int symlink_latest, unsigned file_mode)
{
    char archive[HPULOGC_MAX_PATH_LEN * 2];
    unsigned long index = 1;

    if (*fd >= 0) {
        hpu_fs_close(*fd);
        *fd = -1;
    }

    /* Find a free archive name: {index} increments on conflicts. */
    for (;;) {
        rotate_build_name(cfg, path, ts_sec, use_utc, index, archive,
                          sizeof(archive));
        if (hpu_fs_stat_kind(archive) == HPU_FS_MISSING) {
            break;
        }
        if (!strstr(cfg->naming, "{index}")) {
            break; /* timestamp-only template: last name wins */
        }
        index++;
        if (index > 999999) {
            break;
        }
    }

    if (hpu_fs_rename(path, archive) != 0) {
        /* Nothing to rename (file vanished): continue with a fresh one. */
    }

    *fd = hpu_fs_open_append(path, file_mode);
    if (*fd < 0) {
        *file_size = 0;
        return -1;
    }
    if (file_mode != 0) {
        (void)hpu_fs_fchmod(*fd, file_mode);
    }
    *file_size = hpu_fs_size(*fd);
    if (*file_size < 0) {
        *file_size = 0;
    }

    if (symlink_latest) {
        rotate_update_symlink(path);
    }
    (void)hpu_rotate_prune(path, cfg, use_utc);
    return 0;
}
