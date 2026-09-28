/**
 * @file format.h
 * @brief Internal format template precompilation and line rendering.
 *
 * Templates are compiled once (init / hot reload) into an action sequence
 * (spec 4.9: no string re-parsing on the hot path). Rendering appends into
 * a caller-provided line buffer with the max_log_length truncation rule
 * (full line including newline, truncation marker, spec 9).
 */

#ifndef HPU_FORMAT_H
#define HPU_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#include "hpulogc.h"

/**
 * @brief Rendered action codes for the precompiled sequence.
 */
typedef enum {
    HPU_FA_LITERAL = 0,  /*!< Literal bytes from the pool */
    HPU_FA_LEVEL,        /*!< %level - uppercase level tag */
    HPU_FA_TIME,         /*!< %time - timestamp (cached per second) */
    HPU_FA_PID,          /*!< %pid */
    HPU_FA_TID,          /*!< %tid */
    HPU_FA_FILE,         /*!< %file */
    HPU_FA_LINE,         /*!< %line */
    HPU_FA_FUNC,         /*!< %func */
    HPU_FA_MSG,          /*!< %msg (injection / JSON escaping applies) */
    HPU_FA_CATEGORY,     /*!< %category (JSON escaping applies) */
    HPU_FA_NEWLINE,      /*!< %n */
    HPU_FA_FIELDS        /*!< %v - structured fields (empty when none); json
                              *   formats splice in a typed "fields" member */
} hpu_fmt_action_type_t;

/**
 * @brief One precompiled action.
 */
typedef struct hpu_fmt_action {
    uint8_t  type;     /*!< hpu_fmt_action_type_t */
    uint32_t lit_len;  /*!< LITERAL: byte count in the pool */
    uint32_t lit_off;  /*!< LITERAL: offset into the pool */
} hpu_fmt_action_t;

/**
 * @brief A compiled format template.
 */
typedef struct hpu_format {
    char name[HPULOGC_MAX_NAME_LEN]; /*!< Template name */
    int  is_json;                    /*!< JSON semantics (name "json") */
    hpu_fmt_action_t* actions;       /*!< Action sequence */
    size_t action_count;             /*!< Number of actions */
    char* pool;                      /*!< Literal bytes */
    size_t pool_len;                 /*!< Used pool bytes */
    size_t pool_cap;                 /*!< Pool capacity */
} hpu_format_t;

/**
 * @brief Per-thread cache for time rendering (avoids strftime per log).
 */
typedef struct hpu_fmt_cache {
    int64_t cached_sec;               /*!< Epoch second of the cached text */
    char    prefix[HPULOGC_MAX_FMT_LEN]; /*!< Text before the fraction */
    size_t  prefix_len;               /*!< Cached prefix length */
    char    suffix[HPULOGC_MAX_FMT_LEN]; /*!< Text after the fraction */
    size_t  suffix_len;               /*!< Cached suffix length */
    int     valid;                    /*!< Cache holds a rendered second */
} hpu_fmt_cache_t;

/**
 * @brief Everything the renderer needs besides the format and record.
 *
 * Filled by the configuration layer; immutable while a config snapshot is
 * active.
 */
typedef struct hpu_fmt_env {
    const char* time_format;     /*!< Global time format (strftime style) */
    int         use_utc;         /*!< Non-zero: UTC, else local timezone */
    int         timestamp_source; /*!< hpulogc_timestamp_source_t value */
    int         newline_style;   /*!< hpulogc_newline_t value */
    int         pid_fmt;         /*!< 0=decimal, 1=hex, 2=none */
    int         tid_fmt;         /*!< 0=decimal, 1=hex, 2=none */
    int         source_loc_enabled; /*!< Runtime capture source loc flag */
    int64_t     mono_base_us;    /*!< Monotonic microseconds at init */
    long        pid;             /*!< Cached getpid() */
} hpu_fmt_env_t;

/**
 * @brief Log record passed to the renderer.
 */
typedef struct hpu_log_record {
    int         level;         /*!< Severity (hpulogc_level_t) */
    const char* category;      /*!< Category bytes (may be NULL) */
    size_t      category_len;  /*!< Category byte count */
    const char* file;          /*!< File bytes (may be NULL) */
    size_t      file_len;      /*!< File byte count */
    const char* func;          /*!< Function bytes (may be NULL) */
    size_t      func_len;      /*!< Function byte count */
    int         line;          /*!< Source line */
    const char* msg;           /*!< Rendered message bytes */
    size_t      msg_len;       /*!< Message byte count */
    int64_t     realtime_ns;   /*!< Realtime capture timestamp */
    int64_t     mono_us;       /*!< Monotonic capture timestamp */
    uint64_t    tid;           /*!< Producer thread id */
    /* Structured fields in wire format (§4.11.4); pointers stay valid for
     * the whole processing of this record (TLS buffer / ring staging). */
    const uint8_t* fields_wire;  /*!< Wire-encoded field region (may be NULL) */
    uint32_t       fields_len;   /*!< Wire region byte count (8-aligned) */
    uint16_t       field_count;  /*!< Number of encoded fields */
} hpu_log_record_t;

/**
 * @brief Initialize a render cache (call once per thread before rendering).
 * @param cache  Cache to initialize.
 */
void hpu_fmt_cache_init(hpu_fmt_cache_t* cache);

/**
 * @brief Compile a format template into an action sequence.
 *
 * @param fmt       Format object to fill (name copied).
 * @param name      Format name; the name "json" enables JSON semantics.
 * @param template_ Format template string (NUL-terminated).
 * @return          0 on success, HPULOGC_ERR_CONFIG when the template
 *                  contains an unknown placeholder, HPULOGC_ERR_NO_MEM on
 *                  allocation failure.
 */
int hpu_format_compile(hpu_format_t* fmt, const char* name,
                       const char* template_);

/**
 * @brief Release the allocations owned by a compiled format.
 * @param fmt  Format object; must be compiled or zeroed.
 */
void hpu_format_free(hpu_format_t* fmt);

/**
 * @brief Render one complete log line.
 *
 * Applies JSON escaping for the json format (msg/file/category/func;
 * pid/tid forced decimal, spec 12) or the configurable injection escaping
 * to %msg only (newline -> "\\n" literal text, ESC -> "\\x1b" literal
 * text). Enforces the full-line truncation rule: the returned line
 * including its newline never exceeds @p max_line bytes; when truncated,
 * @p marker is appended before the newline.
 *
 * @param fmt              Compiled format.
 * @param rec              Record to render.
 * @param env              Render environment (config-derived).
 * @param cache            Per-thread time cache.
 * @param escape_injection Non-zero to apply injection escaping (json
 *                         formats ignore this).
 * @param max_line         Maximum full line length in bytes.
 * @param marker           Truncation marker (may be NULL/empty).
 * @param out              Output buffer.
 * @param out_size         Output buffer size (>= max_line + 1).
 * @param out_len          Filled with the rendered length.
 * @return                 0 on success, HPULOGC_ERR_INVALID_ARG when the
 *                         buffer is too small for max_line.
 */
int hpu_format_render(const hpu_format_t* fmt, const hpu_log_record_t* rec,
                      const hpu_fmt_env_t* env, hpu_fmt_cache_t* cache,
                      int escape_injection, size_t max_line,
                      const char* marker, char* out, size_t out_size,
                      size_t* out_len);

/**
 * @brief The five built-in format templates (spec 10.3).
 */
extern const char* const hpu_builtin_format_names[5];
extern const char* const hpu_builtin_format_templates[5];

/* ---- Structured-field wire format (§4.11.4) ---- */

/**
 * @brief Serialize structured fields into the wire format.
 *
 * Wire encoding per field: {u16 key_len; u8 type; u8 pad; u16 val_len;
 * u8 val[val_len]} (host byte order, scalars via memcpy); the whole region
 * is padded to 8 bytes. Budget rules (§4.11.2): fields beyond
 * HPULOGC_MAX_FIELDS, keys longer than HPULOGC_MAX_FIELD_KEY_LEN and the
 * tail beyond @p budget bytes are dropped (each counts +1 into
 * @p dropped); STRING values longer than HPULOGC_MAX_FIELD_STR_LEN are
 * truncated (+1).
 *
 * @param fields   Caller-held field array (may be NULL when n == 0).
 * @param n        Number of input fields.
 * @param budget   Maximum serialized region size in bytes (max_log_length).
 * @param out      Output buffer (wire region).
 * @param cap      Capacity of @p out.
 * @param out_len  Filled with the padded wire length.
 * @param out_cnt  Filled with the number of encoded fields.
 * @param dropped  Accumulates dropped/truncated field count (may be NULL).
 * @return         0 on success, HPULOGC_ERR_NO_MEM when @p cap is too
 *                 small for the first field (caller sizes it with the
 *                 max_record_size formula).
 */
int hpu_fields_serialize(const hpulogc_field_t* fields, size_t n,
                         size_t budget, uint8_t* out, size_t cap,
                         size_t* out_len, uint16_t* out_cnt,
                         unsigned long long* dropped);

/**
 * @brief Wire-format field visitor (§4.11.4).
 *
 * @param ud       Opaque user data.
 * @param key      Field key bytes (not NUL-terminated).
 * @param key_len  Key byte count.
 * @param type     hpulogc_field_type_t value.
 * @param val      Value bytes: scalars in host order (8/8/8/1 bytes),
 *                 STR raw bytes.
 * @param val_len  Value byte count.
 * @return         0 to continue iteration, non-zero to stop.
 */
typedef int (*hpu_fields_iter_cb)(void* ud, const char* key, size_t key_len,
                                  int type, const void* val, size_t val_len);

/**
 * @brief Iterate the fields of a wire region.
 *
 * @param wire   Wire region (may be NULL when len == 0).
 * @param len    Region byte count (8-aligned padded length).
 * @param count  Field count from the record header.
 * @param cb     Visitor called once per field.
 * @param ud     Opaque user data for @p cb.
 */
void hpu_fields_iterate(const uint8_t* wire, size_t len, uint16_t count,
                        hpu_fields_iter_cb cb, void* ud);

#endif /* HPU_FORMAT_H */
