/**
 * @file conf_model.h
 * @brief Internal configuration snapshot shared by code config and file
 *        config paths, plus validation/finalization helpers.
 */

#ifndef HPU_CONF_MODEL_H
#define HPU_CONF_MODEL_H

#include "../format/format.h"
#include "../output/output.h"
#include "hpulogc.h"

/** @brief fork behavior values (spec 9). */
#define HPU_FORK_REINIT  0
#define HPU_FORK_DISABLE 1
#define HPU_FORK_INHERIT 2

/** @brief pid/tid display formats. */
#define HPU_ID_FMT_DECIMAL 0
#define HPU_ID_FMT_HEX     1
#define HPU_ID_FMT_NONE    2

/** @brief Throttle parameters ([throttle] section / throttle build). */
typedef struct hpu_throttle_cfg {
    long   global_rate;      /*!< Global token rate (logs/sec), 0 = off */
    long   per_category_rate; /*!< Per-category token rate, 0 = off */
    long   burst;            /*!< Token bucket capacity */
    double sampling_rate;    /*!< 0.0 .. 1.0 */
    int    sampling_n;       /*!< Deterministic 1/N divisor (1 = no sampling) */
} hpu_throttle_cfg_t;

/**
 * @brief One output definition (public fields + owned string storage).
 */
typedef struct hpu_conf_output {
    hpulogc_output_t pub;              /*!< Public view (strings owned) */
    char name_buf[HPULOGC_MAX_NAME_LEN];  /*!< Output name (config key) */
    char path_buf[HPULOGC_MAX_PATH_LEN];
    char naming_buf[HPULOGC_MAX_FMT_LEN]; /*!< Rotate naming template */
    hpu_output_t* handle;              /*!< Opened backend, NULL if not */
} hpu_conf_output_t;

/**
 * @brief One routing rule with resolved format/output references.
 */
typedef struct hpu_conf_rule {
    char category[HPULOGC_MAX_NAME_LEN]; /*!< Selector: "*", name, name.* */
    int  min_level;                      /*!< Inclusive lower bound */
    int  max_level;                      /*!< Inclusive upper bound */
    char format_name[HPULOGC_MAX_NAME_LEN]; /*!< Format reference */
    int  output_idx[HPULOGC_MAX_OUTPUTS];   /*!< Resolved output indices */
    size_t output_count;
} hpu_conf_rule_t;

/**
 * @brief Full runtime configuration snapshot.
 *
 * Immutable once finalized; hot reload swaps the whole object under the
 * configuration lock (spec 10.5).
 */
typedef struct hpu_conf {
    /* [global] */
    int  level;                     /*!< Global filter threshold */
    char default_format[HPULOGC_MAX_NAME_LEN];
    char default_output_names[HPULOGC_MAX_OUTPUTS][HPULOGC_MAX_NAME_LEN];
    size_t default_output_name_count;
    int  default_output_idx[HPULOGC_MAX_OUTPUTS]; /*!< Resolved indices */
    size_t default_output_count;    /*!< Resolved count */
    int  use_utc;
    int  timestamp_source;          /*!< hpulogc_timestamp_source_t */
    char time_format[HPULOGC_MAX_FMT_LEN];
    int  newline_style;             /*!< hpulogc_newline_t */
    int  pid_fmt;                   /*!< HPU_ID_FMT_* */
    int  tid_fmt;
    int  capture_source_loc;
    int  strict_init;
    int  hot_reload_interval;       /*!< Seconds, 0 = polling off */
    int  signal_reload;             /*!< SIGHUP handler installed */
    /* [formats] / [outputs] / [rules] */
    hpu_format_t* formats;          /*!< Compiled formats (builtins first) */
    size_t format_count;
    hpu_conf_output_t* outputs;     /*!< Output definitions */
    size_t output_count;
    hpu_conf_rule_t* rules;         /*!< Routing rules in order */
    size_t rule_count;
    /* [buffer] */
    size_t buffer_size;
    int    overflow_policy;         /*!< hpulogc_overflow_policy_t */
    /* [async] */
    uint32_t batch_size;
    uint32_t flush_interval_ms;
    uint32_t shutdown_timeout_ms;
    /* [throttle] */
    hpu_throttle_cfg_t throttle;
    /* [advanced] */
    int    escape_injection;
    size_t max_log_length;
    char   truncation_marker[HPULOGC_MAX_FMT_LEN];
    int    fork_behavior;
    int    signal_safe;
    int    crash_safety;            /*!< hpulogc_crash_safety_t */
    int    stats_interval;          /*!< Seconds, 0 = off */
    int    stats_output_file;       /*!< 0 = stderr, 1 = stats_file */
    char   stats_file[HPULOGC_MAX_PATH_LEN];
    /* derived */
    hpu_fmt_env_t env;              /*!< Renderer environment */
    char source_path[HPULOGC_MAX_PATH_LEN]; /*!< "" = code config */
} hpu_conf_t;

/**
 * @brief Fill a configuration snapshot with built-in defaults.
 * @param c  Snapshot to initialize.
 * @return   0 on success, HPULOGC_ERR_NO_MEM when the builtin formats
 *           could not be compiled.
 */
int hpu_conf_defaults(hpu_conf_t* c);

/**
 * @brief Apply a public code configuration on top of the defaults.
 *
 * Code-internal configuration values are validated strictly: invalid
 * values return an error, no clamping (spec 7.2). Ranges and enums are
 * checked here; reference resolution and output opening happen in
 * hpu_conf_finalize().
 *
 * @param c    Snapshot with defaults applied.
 * @param cfg  Public configuration (must be fully initialized by caller).
 * @return     0 or HPULOGC_ERR_INVALID_ARG / HPULOGC_ERR_CONFIG.
 */
int hpu_conf_from_code(hpu_conf_t* c, const hpulogc_config_t* cfg);

/**
 * @brief Validate references and open all file outputs.
 *
 * Checks rule format/output references, duplicate file paths, rotation
 * naming templates, overflow-policy availability and opens every file
 * output (fail-fast, spec 9/10.4). On failure everything opened is
 * closed again.
 *
 * @param c  Snapshot to finalize.
 * @return   0 or HPULOGC_ERR_CONFIG / HPULOGC_ERR_IO / HPULOGC_ERR_NO_MEM.
 */
int hpu_conf_finalize(hpu_conf_t* c);

/**
 * @brief Finalize for hot reload with fd reuse.
 *
 * Same as hpu_conf_finalize(), but a file output whose path and all
 * parameters match one in @p old reuses its handle (moved out of @p old)
 * instead of closing and reopening (spec 10.5).
 *
 * @param fresh  New snapshot.
 * @param old    Currently active snapshot (READ ONLY; the caller must
 *               hold the configuration read lock or otherwise guarantee
 *               exclusivity).
 * @param reuse_old_idx  Out: per fresh output, the old output index whose
 *               handle may be reused (-1 = open fresh).
 * @return       0 or an error code; on error the caller frees @p fresh.
 *
 * The handle move itself happens in the caller under the write lock.
 */
int hpu_conf_finalize_reload(hpu_conf_t* fresh, hpu_conf_t* old,
                              int reuse_old_idx[HPULOGC_MAX_OUTPUTS]);

/**
 * @brief Close every open output handle of a snapshot.
 * @param c  Snapshot.
 */
void hpu_conf_close_outputs(hpu_conf_t* c);

#if HPULOGC_ENABLE_INI
/**
 * @brief Load and parse a configuration file into a snapshot.
 *
 * @param c       Snapshot with defaults applied.
 * @param path    Configuration file path.
 * @param strict  Effective unknown-key handling (1 strict, 0 lenient).
 * @return        0 or HPULOGC_ERR_CONFIG.
 */
int hpu_conf_load_file(hpu_conf_t* c, const char* path, int strict);
#endif

/**
 * @brief Release all memory owned by a snapshot (closes outputs first).
 * @param c  Snapshot.
 */
void hpu_conf_free(hpu_conf_t* c);

/**
 * @brief Find a compiled format by name.
 * @param c     Snapshot.
 * @param name  Format name.
 * @return      Compiled format or NULL.
 */
const hpu_format_t* hpu_conf_find_format(const hpu_conf_t* c,
                                         const char* name);

/**
 * @brief Index of an output by name, -1 when undefined.
 */
int hpu_conf_find_output(const hpu_conf_t* c, const char* name);

#endif /* HPU_CONF_MODEL_H */
