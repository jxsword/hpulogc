/**
 * @file hpulogc.h
 * @brief Public API of hpulogc - a pure C high-performance logging library.
 *
 * This is the only header applications include. All public symbols use the
 * `hpulogc_` prefix. The header is C99/C11 clean and C++-safe (extern "C").
 *
 * @ingroup hpulogc
 */

#ifndef HPULOGC_H
#define HPULOGC_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! @defgroup hpulogc hpulogc public API
 *  @brief Pure C high-performance logging library.
 *  @{
 */

/* ---- Version macros ---- */

/** @brief Major version of the library (semantic versioning). */
#define HPULOGC_VERSION_MAJOR 0
/** @brief Minor version of the library (semantic versioning). */
#define HPULOGC_VERSION_MINOR 1
/** @brief Patch version of the library (semantic versioning). */
#define HPULOGC_VERSION_PATCH 0

/** @brief Version string literal, e.g. "0.1.0". */
#define HPULOGC_VERSION_STRING \
    "0.1.0"

/* ---- Dynamic library export attribute ---- */

/**
 * @brief Symbol export annotation for public API functions.
 *
 * On Windows this expands to `__declspec(dllexport)` while building the
 * library DLL (HPULOGC_BUILDING_DLL defined) or `__declspec(dllimport)`
 * while consuming it (HPULOGC_USE_DLL defined). On GCC/Clang it requests
 * default ELF visibility so the symbols survive `-fvisibility=hidden`.
 */
#ifndef HPULOGC_API
#  if defined(_WIN32)
#    if defined(HPULOGC_BUILDING_DLL)
#      define HPULOGC_API __declspec(dllexport)
#    elif defined(HPULOGC_USE_DLL)
#      define HPULOGC_API __declspec(dllimport)
#    else
#      define HPULOGC_API
#    endif
#  elif defined(__GNUC__) || defined(__clang__)
#    define HPULOGC_API __attribute__((visibility("default")))
#  else
#    define HPULOGC_API
#  endif
#endif

/**
 * @brief printf-format attribute wrapper (compile-time format string checks).
 */
#ifndef HPULOGC_PRINTF
#  if defined(__GNUC__) || defined(__clang__)
#    define HPULOGC_PRINTF(fmt_idx, va_idx) \
        __attribute__((format(printf, fmt_idx, va_idx)))
#  else
#    define HPULOGC_PRINTF(fmt_idx, va_idx)
#  endif
#endif

/* ---- Capacity limits (overridable at compile time with -D) ---- */

#ifndef HPULOGC_MAX_OUTPUTS
/** @brief Maximum number of outputs (code config array limit; config file uses the same). */
#define HPULOGC_MAX_OUTPUTS 16
#endif

#ifndef HPULOGC_MAX_RULES
/** @brief Maximum number of routing rules (code config array limit; config file uses the same). */
#define HPULOGC_MAX_RULES 64
#endif

#ifndef HPULOGC_MAX_CATEGORIES
/** @brief Capacity of the internal category registry. */
#define HPULOGC_MAX_CATEGORIES 64
#endif

#ifndef HPULOGC_MAX_NAME_LEN
/** @brief Maximum length (bytes incl. NUL) of format/output/category names. */
#define HPULOGC_MAX_NAME_LEN 64
#endif

#ifndef HPULOGC_MAX_PATH_LEN
/** @brief Maximum length (bytes incl. NUL) of file paths. */
#define HPULOGC_MAX_PATH_LEN 512
#endif

#ifndef HPULOGC_MAX_FMT_LEN
/** @brief Maximum length (bytes incl. NUL) of format templates / config string values. */
#define HPULOGC_MAX_FMT_LEN 256
#endif

/**
 * @brief Log severity levels, ordered by increasing severity.
 *
 * HPULOGC_LEVEL_TRACE .. HPULOGC_LEVEL_FATAL are valid log levels.
 * HPULOGC_LEVEL_OFF is only a valid runtime filter threshold (it disables all
 * output); passing it as the level argument of a write API silently drops
 * the record.
 */
typedef enum {
    HPULOGC_LEVEL_TRACE = 0, /*!< Most verbose level */
    HPULOGC_LEVEL_DEBUG = 1, /*!< Debug information */
    HPULOGC_LEVEL_INFO  = 2, /*!< Informational messages */
    HPULOGC_LEVEL_WARN  = 3, /*!< Warning conditions */
    HPULOGC_LEVEL_ERROR = 4, /*!< Error conditions */
    HPULOGC_LEVEL_FATAL = 5, /*!< Fatal conditions */
    HPULOGC_LEVEL_OFF   = 6  /*!< Only a filter threshold, not a valid log level */
} hpulogc_level_t;

/**
 * @brief Output target type.
 */
typedef enum {
    HPULOGC_OUT_CONSOLE = 0, /*!< Console (stdout/stderr) */
    HPULOGC_OUT_FILE    = 1  /*!< Regular file */
} hpulogc_output_type_t;

/**
 * @brief Log rotation policy of a file output.
 */
typedef enum {
    HPULOGC_ROTATE_NONE = 0, /*!< Never rotate */
    HPULOGC_ROTATE_SIZE = 1, /*!< Rotate when file reaches max_size */
    HPULOGC_ROTATE_TIME = 2, /*!< Rotate at time bucket boundaries */
    HPULOGC_ROTATE_BOTH = 3  /*!< Rotate on either condition */
} hpulogc_rotate_t;

/**
 * @brief Time bucket unit for time-based rotation.
 *
 * Buckets follow the configured timezone (local or UTC); week starts on
 * Monday 00:00 (ISO-8601).
 */
typedef enum {
    HPULOGC_TU_HOUR  = 0, /*!< Hourly buckets */
    HPULOGC_TU_DAY   = 1, /*!< Daily buckets (natural day) */
    HPULOGC_TU_WEEK  = 2, /*!< Weekly buckets (ISO-8601, Monday) */
    HPULOGC_TU_MONTH = 3  /*!< Monthly buckets (natural month) */
} hpulogc_time_unit_t;

/**
 * @brief Ring buffer overflow policy (chosen at init, fixed afterwards).
 *
 * Availability is constrained by the build: the lock-free ring supports
 * discard (SPSC and MPSC) and overwrite (SPSC only); wait is only available
 * with the locked ring. Configuring an unsupported combination fails init
 * with HPULOGC_ERR_CONFIG.
 */
typedef enum {
    HPULOGC_OVERFLOW_DISCARD   = 0, /*!< Drop the new log; dropped counter increments */
    HPULOGC_OVERFLOW_OVERWRITE = 1, /*!< Drop the oldest unconsumed entry; overwritten increments */
    HPULOGC_OVERFLOW_WAIT      = 2  /*!< Block the producer until space is available */
} hpulogc_overflow_policy_t;

/**
 * @brief Timestamp source.
 */
typedef enum {
    HPULOGC_TS_REALTIME  = 0, /*!< CLOCK_REALTIME (wall clock) */
    HPULOGC_TS_MONOTONIC = 1  /*!< Monotonic clock, seconds.microseconds relative to init */
} hpulogc_timestamp_source_t;

/**
 * @brief Newline style for the %n placeholder.
 */
typedef enum {
    HPULOGC_NEWLINE_AUTO = 0, /*!< Unix=\n / Windows=\r\n */
    HPULOGC_NEWLINE_LF   = 1, /*!< Force \n */
    HPULOGC_NEWLINE_CRLF = 2  /*!< Force \r\n */
} hpulogc_newline_t;

/**
 * @brief fsync (crash safety) policy.
 *
 * @note The enum numeric order is NOT the severity order. Severity is
 *       none < shutdown < periodic < entry; the effective policy of an
 *       output is the more strict of the global crash_safety and the
 *       per-output fsync flag (fsync=true implies at least entry).
 */
typedef enum {
    HPULOGC_CRASH_NONE     = 0, /*!< Never fsync */
    HPULOGC_CRASH_PERIODIC = 1, /*!< Consumer fsyncs periodically (flush interval); sync mode equals shutdown */
    HPULOGC_CRASH_ENTRY    = 2, /*!< fsync after every log entry */
    HPULOGC_CRASH_SHUTDOWN = 3  /*!< fsync only at shutdown */
} hpulogc_crash_safety_t;

/**
 * @brief Output target descriptor (code-internal configuration).
 *
 * Populate an array of these and reference the entries by name from rules.
 * The library copies all required metadata during init; strings must stay
 * valid until hpulogc_init() returns.
 */
typedef struct {
    hpulogc_output_type_t type;        /*!< Output type */
    int          stream;               /*!< console: 0=stdout, 1=stderr */
    int          color;                /*!< console: enable ANSI color (terminal only) */
    const char*  path;                 /*!< file: log file path */
    hpulogc_rotate_t      rotate;      /*!< file: rotation policy */
    size_t               max_size;     /*!< file: rotation threshold in bytes (size rotation) */
    hpulogc_time_unit_t   time_unit;   /*!< file: time bucket unit (time rotation) */
    int          max_files;            /*!< file: max archived files, 0 = unlimited */
    int          fsync;                /*!< file: per-entry fsync (combined with global crash_safety) */
    int          symlink_latest;       /*!< file: create/update <path>.latest symlink (ignored on Windows) */
    const char*  rotate_naming;        /*!< file: archive naming template; NULL = "{base}.{timestamp}.{index}.log" */
    unsigned     file_mode;            /*!< file: POSIX octal permission bits, e.g. 0644 (ignored on Windows) */
    unsigned     dir_mode;             /*!< file: parent directory permission bits, e.g. 0755 (ignored on Windows) */
} hpulogc_output_t;

/**
 * @brief Routing rule (code-internal configuration).
 *
 * Rules are evaluated top to bottom; the first rule whose category selector
 * matches and whose level range contains the record level wins. Only the
 * five built-in format names are allowed in @p format.
 */
typedef struct {
    const char*     category;         /*!< Exact name / "name.*" / "*"; NULL or "" equals "*" */
    hpulogc_level_t min_level;        /*!< Inclusive lower bound; min > max is a config error */
    hpulogc_level_t max_level;        /*!< Inclusive upper bound */
    const char*     format;           /*!< "minimal"/"standard"/"categorized"/"detailed"/"json" */
    const char* const* outputs;       /*!< Array of output names */
    size_t         output_count;      /*!< Number of entries in outputs */
} hpulogc_rule_t;

/**
 * @brief Code-internal configuration.
 *
 * MUST be initialized with hpulogc_config_default() (or by explicitly
 * assigning every field) before passing to hpulogc_init(); content of
 * untouched fields is undefined. Invalid values make hpulogc_init() fail
 * with HPULOGC_ERR_INVALID_ARG or HPULOGC_ERR_CONFIG (no clamping).
 */
typedef struct {
    /* Level and fallbacks */
    hpulogc_level_t       level;            /*!< Global filter threshold, default INFO */
    const char*          default_format;   /*!< Fallback format name; NULL = "standard" */
    const char* const*   default_outputs;  /*!< Fallback output names; NULL/empty = drop unmatched */
    size_t               default_output_count; /*!< Number of entries in default_outputs */
    /* Outputs and rules */
    const hpulogc_output_t* outputs;       /*!< Output array (owned by caller, copied at init) */
    size_t output_count;                   /*!< Number of outputs */
    const hpulogc_rule_t*   rules;         /*!< Rule array (owned by caller, copied at init) */
    size_t rule_count;                     /*!< Number of rules */
    /* Ring buffer */
    size_t                       buffer_size;     /*!< Bytes, default 1MB, range 4KB~1GB; raised if < 2 x max_log_length */
    hpulogc_overflow_policy_t     overflow_policy; /*!< Overflow policy, default discard */
    /* Async (ignored when the build has HPULOGC_ENABLE_ASYNC=OFF) */
    uint32_t             batch_size;          /*!< Consumer batch size, default 64 */
    uint32_t             flush_interval_ms;   /*!< Force-submit interval in ms, default 100 */
    uint32_t             shutdown_timeout_ms; /*!< Shutdown drain timeout in ms, default 5000, 0 = wait forever */
    /* Advanced */
    int                  escape_injection;    /*!< Escape newlines/ANSI in %msg, default 1 */
    size_t               max_log_length;      /*!< Max rendered message/full line bytes, default 4096, range 256~65536 */
    const char*          truncation_marker;   /*!< Marker appended on full-line truncation, default "...[TRUNCATED]" */
    hpulogc_crash_safety_t crash_safety;      /*!< Global fsync policy, default shutdown */
    int                  signal_safe;         /*!< Enable hpulogc_log_signal_safe output, default 0 */
} hpulogc_config_t;

/**
 * @brief Error codes returned by init/control APIs (0 = success).
 */
typedef enum {
    HPULOGC_OK            =  0, /*!< Success */
    HPULOGC_ERR_INVALID_ARG = -1, /*!< Invalid argument (NULL, out-of-range enum, code config value out of range) */
    HPULOGC_ERR_NO_MEM      = -2, /*!< Memory allocation failure */
    HPULOGC_ERR_IO          = -3, /*!< File/device I/O failure (e.g. output open failed at init) */
    HPULOGC_ERR_CONFIG      = -4, /*!< Configuration missing/invalid, or value valid but trimmed by this build */
    HPULOGC_ERR_STATE       = -5  /*!< State error (uninitialized, double init, called after shutdown) */
} hpulogc_error_t;

/* ---- Init / shutdown ---- */

/**
 * @brief Initialize the logging library from a caller-provided configuration.
 *
 * @param cfg  Configuration; NULL is equivalent to hpulogc_init_default().
 *             When non-NULL it must have been initialized with
 *             hpulogc_config_default() first. Strings are only read during
 *             this call.
 * @return     HPULOGC_OK on success; negative hpulogc_error_t value on
 *             failure. On failure the library stays uninitialized and all
 *             resources opened during the attempt are released.
 *
 * @note Not thread-safe with respect to other API calls: the caller must
 *       guarantee init completes before any other thread uses the library.
 *       Calling init again while initialized returns HPULOGC_ERR_STATE and
 *       leaves the running instance untouched.
 */
HPULOGC_API int hpulogc_init(const hpulogc_config_t* cfg);

/**
 * @brief Initialize the logging library from an INI configuration file.
 *
 * @param config_path  Path to the configuration file; NULL is invalid and
 *                     yields HPULOGC_ERR_INVALID_ARG.
 * @return             HPULOGC_OK on success; HPULOGC_ERR_CONFIG on parse/
 *                     validation failure (diagnostics with file name and
 *                     line number go to stderr); HPULOGC_ERR_IO when an
 *                     output cannot be opened; other negative values
 *                     per hpulogc_error_t.
 * @see hpulogc_init()
 */
HPULOGC_API int hpulogc_init_from_file(const char* config_path);

/**
 * @brief Initialize the logging library with built-in defaults.
 *
 * Default semantics: level=INFO, "standard" format, built-in stderr console
 * output, 1MB buffer, discard overflow policy.
 *
 * @return HPULOGC_OK on success, negative error value otherwise.
 */
HPULOGC_API int hpulogc_init_default(void);

/**
 * @brief Fill a configuration structure with default values.
 *
 * @param cfg  Structure to fill; NULL is safely ignored.
 *
 * @note Callers must run this (or explicitly set every field) before
 *       passing the structure to hpulogc_init().
 */
HPULOGC_API void hpulogc_config_default(hpulogc_config_t* cfg);

/**
 * @brief Shut the library down and release all resources.
 *
 * Flushes and drains the queue first (waiting at most
 * shutdown_timeout_ms), then closes all outputs and frees memory.
 * Calling shutdown twice is safe (idempotent). Not thread-safe with
 * respect to concurrent log writes.
 */
HPULOGC_API void hpulogc_shutdown(void);

/* ---- Flush / sync ---- */

/**
 * @brief Flush queued logs to their outputs (without fsync).
 *
 * In async mode this notifies the consumer to submit everything already
 * enqueued; in sync mode it refreshes stdio streams. Returns
 * HPULOGC_ERR_STATE when the library is not initialized.
 *
 * @return HPULOGC_OK on success, negative error value otherwise.
 */
HPULOGC_API int hpulogc_flush(void);

/**
 * @brief Flush like hpulogc_flush() and additionally fsync all file outputs.
 *
 * @return HPULOGC_OK on success, negative error value otherwise.
 */
HPULOGC_API int hpulogc_sync(void);

/* ---- Runtime controls ---- */

/**
 * @brief Change the global filter threshold at runtime.
 *
 * @param level  New threshold; HPULOGC_LEVEL_OFF disables all output.
 * @return       HPULOGC_OK on success, HPULOGC_ERR_STATE when
 *               uninitialized, HPULOGC_ERR_INVALID_ARG when level is not a
 *               valid threshold (levels above HPULOGC_LEVEL_OFF).
 */
HPULOGC_API int hpulogc_set_level(hpulogc_level_t level);

/**
 * @brief Override the filter threshold for one category.
 *
 * Override semantics: once a category has its own level, that level wins
 * over the global threshold; clearing back to global is done by passing
 * @p level = (hpulogc_level_t)-1.
 *
 * @param category  Category name; NULL returns HPULOGC_ERR_INVALID_ARG.
 * @param level     New threshold for the category (TRACE..OFF), or
 *                  (hpulogc_level_t)-1 to clear the override.
 * @return          HPULOGC_OK on success, negative error value otherwise.
 */
HPULOGC_API int hpulogc_set_level_for_category(const char* category,
                                               hpulogc_level_t level);

/* ---- Build information ---- */

/**
 * @brief Static description of the library build.
 */
typedef struct {
    const char* build_version; /*!< "full" / "min" / "sync_thread" / "async_single" / "custom" */
    int         has_async;     /*!< Non-zero when the async consumer is compiled in */
    int         has_color;     /*!< Non-zero when ANSI color support is compiled in */
    int         has_rotate;    /*!< Non-zero when log rotation is compiled in */
    int         has_hot_reload;/*!< Non-zero when config hot reload is compiled in */
    int         has_category;  /*!< Non-zero when category routing is compiled in */
    int         has_throttle;  /*!< Non-zero when rate limiting/sampling is compiled in */
    int         has_ini;       /*!< Non-zero when the INI parser is compiled in */
    int         lockfree;      /*!< Non-zero when the lock-free ring buffer is compiled in */
    const char* concurrency;   /*!< "spsc" or "mpsc" */
} hpulogc_build_info_t;

/**
 * @brief Query the build configuration of the linked library.
 *
 * @param info  Filled with the build description; NULL is safely ignored.
 *              May be called at any time (initialization not required).
 */
HPULOGC_API void hpulogc_get_build_info(hpulogc_build_info_t* info);

/* ---- Statistics ---- */

/**
 * @brief Runtime counters snapshot (atomically read, thread-safe).
 */
typedef struct {
    unsigned long long accepted;    /*!< Records that passed filtering and entered the queue/pipeline */
    unsigned long long dropped;     /*!< discard-overflow drops + runtime write-failure drops */
    unsigned long long overwritten; /*!< Records removed by the overwrite policy */
    unsigned long long throttled;   /*!< Records dropped by rate limiting/sampling (always 0 when throttle is off) */
    unsigned long long written;     /*!< Records written out (counted per record, not per output) */
    size_t             buffer_used; /*!< Ring buffer bytes currently occupied */
    size_t             buffer_size; /*!< Ring buffer total size in bytes */
} hpulogc_stats_t;

/**
 * @brief Read a consistent snapshot of the runtime statistics.
 *
 * @param stats  Output snapshot; NULL returns HPULOGC_ERR_INVALID_ARG.
 * @return       HPULOGC_OK on success, HPULOGC_ERR_STATE when
 *               uninitialized.
 */
HPULOGC_API int hpulogc_get_stats(hpulogc_stats_t* stats);

/* ---- Log writing ---- */

/**
 * @brief Write one log record (printf-style).
 *
 * The call never fails to the caller: invalid level, uninitialized state,
 * configuration errors and queue overflow are silently handled according
 * to the configured policy and reflected in hpulogc_get_stats(). The
 * caller's errno is preserved.
 *
 * @param level     Severity of the record; HPULOGC_LEVEL_OFF or invalid
 *                  values are silently dropped.
 * @param category  Category name (any non-empty byte string, '.' is the
 *                  hierarchy separator); NULL/"" equals "*".
 * @param file      Source file name (may be NULL).
 * @param line      Source line number.
 * @param func      Function name (may be NULL).
 * @param fmt       printf-style format string; never pass user data as the
 *                  format string.
 */
HPULOGC_API void hpulogc_log(hpulogc_level_t level, const char* category,
                             const char* file, int line, const char* func,
                             const char* fmt, ...) HPULOGC_PRINTF(6, 7);

/**
 * @brief va_list variant of hpulogc_log().
 *
 * @param level  Severity; see hpulogc_log().
 * @param category  Category name; see hpulogc_log().
 * @param file   Source file name (may be NULL).
 * @param line   Source line number.
 * @param func   Function name (may be NULL).
 * @param fmt    printf-style format string.
 * @param ap     Argument list; consumed exactly once.
 */
HPULOGC_API void hpulogc_vlog(hpulogc_level_t level, const char* category,
                              const char* file, int line, const char* func,
                              const char* fmt, va_list ap);

/**
 * @brief Async-signal-safe restricted log write.
 *
 * This function is always async-signal-safe: no locks, no formatting, no
 * memory allocation; it appends a level tag and writes the message
 * directly to stderr via write(2). It saves and restores errno. When the
 * signal_safe config flag is false (default) the call is a silent no-op.
 *
 * @param level  Severity, used only for the tag.
 * @param msg    NUL-terminated message in a caller-provided static or
 *               stack buffer.
 */
HPULOGC_API void hpulogc_log_signal_safe(hpulogc_level_t level, const char* msg);

/* ---- Convenience macros (compile-time trimming point) ---- */

/**
 * @def HPULOGC_COMPILE_TIME_LEVEL
 * @brief Numeric compile-time trim threshold for the convenience macros.
 *
 * Records below this level make the corresponding HPULOGC_xxx macros expand
 * to empty statements (zero runtime cost). Allowed values are 0..6
 * (HPULOGC_LEVEL_TRACE..HPULOGC_LEVEL_OFF); default 0 (no trimming). This
 * only affects the convenience macros, not direct hpulogc_log() calls.
 */
#ifndef HPULOGC_COMPILE_TIME_LEVEL
#define HPULOGC_COMPILE_TIME_LEVEL 0 /* HPULOGC_LEVEL_TRACE: no trimming */
#endif

/* Variadic macro portability (see spec 7.3): prefer GNU ##__VA_ARGS__,
 * then MSVC traditional auto-comma swallowing, then __VA_OPT__, and
 * finally strict C99 (requires at least one variadic argument). */
#if defined(__GNUC__) || defined(__clang__)
#  define HPULOGC_VA_ARGS(fmt, ...) fmt, ##__VA_ARGS__
#  define HPULOGC_HAS_VA_ARGS 1
#elif defined(_MSC_VER) && (!defined(_MSVC_TRADITIONAL) || _MSVC_TRADITIONAL)
#  define HPULOGC_VA_ARGS(fmt, ...) fmt, __VA_ARGS__
#  define HPULOGC_HAS_VA_ARGS 1
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#  define HPULOGC_VA_ARGS(fmt, ...) fmt __VA_OPT__(,) __VA_ARGS__
#  define HPULOGC_HAS_VA_ARGS 1
#else
#  define HPULOGC_VA_ARGS(fmt, ...) fmt, __VA_ARGS__
#  define HPULOGC_HAS_VA_ARGS 0
#endif

/** @cond INTERNAL */
#define HPULOGC__LOG(lvl, cat, ...) \
    hpulogc_log(lvl, cat, __FILE__, __LINE__, __func__, HPULOGC_VA_ARGS(__VA_ARGS__))
/** @endcond */

#if HPULOGC_COMPILE_TIME_LEVEL <= 0 /* TRACE */
/** @brief Log at TRACE level to @p cat with printf-style @p fmt. */
#define HPULOGC_TRACE(cat, ...) HPULOGC__LOG(HPULOGC_LEVEL_TRACE, cat, __VA_ARGS__)
#else
#define HPULOGC_TRACE(cat, ...) ((void)0)
#endif

#if HPULOGC_COMPILE_TIME_LEVEL <= 1 /* DEBUG */
/** @brief Log at DEBUG level to @p cat with printf-style @p fmt. */
#define HPULOGC_DEBUG(cat, ...) HPULOGC__LOG(HPULOGC_LEVEL_DEBUG, cat, __VA_ARGS__)
#else
#define HPULOGC_DEBUG(cat, ...) ((void)0)
#endif

#if HPULOGC_COMPILE_TIME_LEVEL <= 2 /* INFO */
/** @brief Log at INFO level to @p cat with printf-style @p fmt. */
#define HPULOGC_INFO(cat, ...) HPULOGC__LOG(HPULOGC_LEVEL_INFO, cat, __VA_ARGS__)
#else
#define HPULOGC_INFO(cat, ...) ((void)0)
#endif

#if HPULOGC_COMPILE_TIME_LEVEL <= 3 /* WARN */
/** @brief Log at WARN level to @p cat with printf-style @p fmt. */
#define HPULOGC_WARN(cat, ...) HPULOGC__LOG(HPULOGC_LEVEL_WARN, cat, __VA_ARGS__)
#else
#define HPULOGC_WARN(cat, ...) ((void)0)
#endif

#if HPULOGC_COMPILE_TIME_LEVEL <= 4 /* ERROR */
/** @brief Log at ERROR level to @p cat with printf-style @p fmt. */
#define HPULOGC_ERROR(cat, ...) HPULOGC__LOG(HPULOGC_LEVEL_ERROR, cat, __VA_ARGS__)
#else
#define HPULOGC_ERROR(cat, ...) ((void)0)
#endif

#if HPULOGC_COMPILE_TIME_LEVEL <= 5 /* FATAL */
/** @brief Log at FATAL level to @p cat with printf-style @p fmt. */
#define HPULOGC_FATAL(cat, ...) HPULOGC__LOG(HPULOGC_LEVEL_FATAL, cat, __VA_ARGS__)
#else
#define HPULOGC_FATAL(cat, ...) ((void)0)
#endif

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* HPULOGC_H */
