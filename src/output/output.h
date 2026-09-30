/**
 * @file output.h
 * @brief Internal sink contract (rd_v0.6 §4.10): type registry, instance
 *        creation lifecycle and dispatch over hpulogc_sink_ops_t vtables.
 *
 * The instance layout is [hpu_output_base_t][priv_size bytes]; private data
 * is accessed via hpulogc_sink_priv(). Built-in sinks (console, rollingfile,
 * null, syslog) implement the same vtable as custom sinks.
 */

#ifndef HPU_OUTPUT_H
#define HPU_OUTPUT_H

#include <stddef.h>
#include <stdint.h>

#include "hpulogc.h"
#include "../atomic/hpulogc_atomic.h"
#include "../platform/platform.h"

/* File-scope forward declaration: the config definition struct lives in
 * conf_model.h; without this, the tag in a parameter list would get
 * prototype scope and conflict with the completed definition. */
struct hpu_conf_output;

/** @brief Maximum bytes pending in a file sink's write buffer before a
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

/** @brief Rotation configuration (parsed from the sink config keys). */
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
 * @brief Common sink header at offset 0 of every instance.
 *
 * Carries the type vtable, instance metadata mirrored from the definition
 * (enabled/async/queue size, timezone) and the per-sink statistics.
 */
typedef struct hpu_output_base {
    const hpulogc_sink_ops_t* ops;   /*!< Type implementation (vtable) */
    char name[HPULOGC_MAX_NAME_LEN]; /*!< Instance name (routing key) */
    int  fsync_sev;                  /*!< Effective fsync severity */
    int  use_utc;                    /*!< Timezone mode (global.timezone) */
    int  enabled;                    /*!< Instance enabled (routing visibility) */
    int  async;                      /*!< Async delivery (second-level queue) */
    size_t queue_size;               /*!< Second-level queue capacity in bytes */
    void* queue;                     /*!< Second-level queue state (async instances) */
    hpu_atomic_u64 st_written;       /*!< Delivered records */
    hpu_atomic_u64 st_dropped;       /*!< Queue-full drops */
    hpu_atomic_u64 st_failed;        /*!< Write failures (after retry) */
    hpu_atomic_u64 st_fields;        /*!< Fields removed by the filter whitelist */
    hpu_atomic_u64 st_bytes;         /*!< Delivered line bytes */
    /* `filter keys` whitelist (§4.7.3, D-R14): parsed at create time;
     * filter_count == 0 means no filtering (zero-cost pass-through). */
    char* filter_pool;               /*!< Concatenated whitelist key bytes */
    uint16_t filter_off[HPULOGC_MAX_SINK_FILTER_KEYS]; /*!< Key byte offsets into filter_pool */
    uint8_t filter_len[HPULOGC_MAX_SINK_FILTER_KEYS];  /*!< Key byte counts */
    uint8_t filter_count;            /*!< Whitelist key count (0 = no filter) */
    uint8_t* filter_wire;            /*!< Filtered wire scratch (sync path;
                                          guarded by filter_mu) */
    size_t filter_wire_cap;          /*!< Wire scratch capacity */
    hpu_mutex_t filter_mu;           /*!< Serializes the sync-path scratch:
                                          async-build consumer threads
                                          dispatch to sync sinks
                                          concurrently (§4.10.4) */
} hpu_output_base_t;

/** @brief Opaque sink handle (hpu_output_base_t at offset 0). */
typedef struct hpu_output hpu_output_t;

/** @brief One private key/value pair passed to ops->configure. */
typedef struct hpu_kv {
    const char* key; /*!< Key bytes */
    const char* val; /*!< Value bytes (may be NULL for flag-style keys) */
} hpu_kv_t;

/* ---- Type registry (sink_registry.c) ---- */

/**
 * @brief Register the built-in sink types (idempotent, init path).
 */
void hpu_sink_registry_init(void);

/**
 * @brief Find a registered type by name.
 * @param type  Type name.
 * @return      Ops table or NULL when not registered.
 */
const hpulogc_sink_ops_t* hpu_sink_registry_find(const char* type);

/**
 * @brief Number of registered types (built-in + custom).
 */
size_t hpu_sink_registry_count(void);

/**
 * @brief Forbid further registrations (init path; unfrozen at shutdown).
 */
void hpu_sink_registry_freeze(void);

/**
 * @brief Allow registrations again (shutdown path; re-init window).
 */
void hpu_sink_registry_unfreeze(void);

/**
 * @brief Size-suffix parser shared by sink configuration (zlog semantics:
 *        1k=1000, 1kb=1024, 1m=10^6, 1mb=2^20, 1g=10^9, 1gb=2^30).
 * @return 0 on success, -1 on a malformed value.
 */
int hpu_parse_size_str(const char* s, unsigned long long* out);

/* ---- Instance creation / dispatch (output.c) ---- */

/**
 * @brief Create and start one sink instance (full lifecycle §4.10.2).
 *
 * Runs create -> configure* -> init -> start. Common keys (enabled /
 * async / queue size) are consumed here; unknown or invalid private keys
 * are reported through @p err (the caller applies `strict init`).
 *
 * @param type       Type name (must be registered).
 * @param kvs        Private key/value pairs (may be NULL when nkv == 0).
 * @param nkv        Number of key/value pairs.
 * @param name       Instance name.
 * @param fsync_sev  Effective global fsync severity.
 * @param use_utc    Timezone mode for time-aware sinks.
 * @param err        Filled with HPULOGC_OK / ERR_CONFIG / ERR_IO / ERR_NO_MEM.
 * @return           Instance handle or NULL on failure.
 */
hpu_output_t* hpu_output_create(const char* type, const hpu_kv_t* kvs,
                                size_t nkv, const char* name, int fsync_sev,
                                int use_utc, uint32_t flush_interval_ms,
                                size_t batch_max, int* err);

/**
 * @brief Compatibility shim: create one sink instance from the deprecated
 *        flat hpulogc_output_t descriptor (mapped onto console/rollingfile
 *        keys; behavior identical to v0.2).
 */
/**
 * @brief Create one sink instance from a configuration definition.
 *
 * Handles both shapes (flat shim mapping and generic declaration) plus
 * the common keys; @p err carries ERR_CONFIG (unknown key/type, invalid
 * value) / ERR_IO (open failure) / ERR_NO_MEM.
 */
hpu_output_t* hpu_output_open_from_conf(const struct hpu_conf_output* def,
                                        int effective_fsync, int use_utc,
                                        uint32_t flush_interval_ms,
                                        size_t batch_max, int* err);

/**
 * @brief Wait for the async queue to drain (bounded), flush, run the
 *        destroy callback and free the instance (idempotent on NULL).
 * @param drain_timeout_ms  Bound for the async-queue drain wait (ms).
 */
void hpu_output_close(hpu_output_t* o, uint32_t drain_timeout_ms);

/**
 * @brief Deliver one event to the sink (emit inline or enqueue, §4.10.6).
 * @return 0 delivered (emitted or enqueued), -1 dropped by a full async
 *         queue (counted in the sink's `dropped` statistic).
 */
int hpu_output_deliver(hpu_output_t* o, const hpulogc_event_t* ev);

/**
 * @brief Flush buffered bytes (periodic fsync throttling included).
 * @return 0 on success; non-zero when data remains or the write failed.
 */
int hpu_output_flush(hpu_output_t* o);

/**
 * @brief flush + fsync (FSYNC-capable types).
 * @return 0 on success; non-zero on failure.
 */
int hpu_output_sync(hpu_output_t* o);

/**
 * @brief Periodic fsync tick (PERIODIC severity; throttled internally).
 */
int hpu_output_periodic(hpu_output_t* o, uint64_t now_ns,
                        uint64_t interval_ns);

const char* hpu_output_name(const hpu_output_t* o);

/**
 * @brief Cumulative write failures of an instance (the v0.2 lost-line
 *        counter, folded into retired_lost at close/reload, decision 16).
 */
unsigned long long hpu_output_lost_total(hpu_output_t* o);

/**
 * @brief Snapshot one instance's statistics (§4.10.3).
 */
void hpu_output_get_stats(hpu_output_t* o, hpulogc_sink_stats_t* out);

/* ---- Per-sink field filtering (§4.7.3 `filter keys`, D-R14) ---- */

/**
 * @brief Apply an instance's field whitelist to an event view, in place.
 *
 * Matches the event's wire-encoded field keys against the whitelist and,
 * when any field is removed, rewrites @p view with the surviving subset:
 * a filtered wire region rebuilt into @p scratch (grown on demand) and a
 * decoded field array in @p out_fields (borrowed views into @p scratch).
 * The shared producer staging is never touched: @p view must be a
 * caller-owned mutable copy (sync dispatch) or the worker's private
 * decode staging (async). @p scratch must be serialized by the caller:
 * the sync dispatch path guards it with the base filter_mu (async-build
 * consumer threads dispatch to sync sinks concurrently, §4.10.4) and the
 * async worker is per-instance single-thread.
 *
 * @param b           Instance base (whitelist + scratch owner).
 * @param view        Mutable event view to rewrite.
 * @param out_fields  Decode target for the surviving fields.
 * @param out_max     Capacity of @p out_fields.
 * @param scratch     Wire scratch buffer (owned by the caller's context).
 * @param scratch_cap Current capacity of @p scratch.
 * @return            Number of removed fields (0 = view left unchanged).
 */
size_t hpu_output_filter_apply(const hpu_output_base_t* b,
                               hpulogc_event_t* view,
                               hpulogc_field_t* out_fields, size_t out_max,
                               uint8_t** scratch, size_t* scratch_cap);

/* ---- Internal helpers for built-in sinks (§4.10.4) ---- */

/**
 * @brief Access the common header of a sink handle (built-in sinks only;
 *         custom sinks must use hpulogc_sink_priv()).
 */
hpu_output_base_t* hpu_sink_base(hpulogc_sink_t* sink);

/**
 * @brief Account @p n records as write failures (per-sink `failed`).
 *
 * Replaces the v0.2 per-output lost-line counter; the core folds failed
 * counts into the retired-lost total at close/reload (decision 16).
 */
void hpu_sink_account_failed(hpulogc_sink_t* sink, unsigned long long n);

/* ---- Built-in sink type accessors (registered by the registry) ---- */

const hpulogc_sink_ops_t* hpu_console_sink_ops(void);
const hpulogc_sink_ops_t* hpu_rollingfile_sink_ops(void);
#if defined(HPULOGC_SINK_NULL)
const hpulogc_sink_ops_t* hpu_null_sink_ops(void);
#endif
#if defined(HPULOGC_SINK_SYSLOG)
const hpulogc_sink_ops_t* hpu_syslog_sink_ops(void);
#endif
#if defined(HPULOGC_SINK_TCP)
const hpulogc_sink_ops_t* hpu_tcp_sink_ops(void);
#endif
#if defined(HPULOGC_SINK_UDP)
const hpulogc_sink_ops_t* hpu_udp_sink_ops(void);
#endif
#if defined(HPULOGC_SINK_UNIX)
const hpulogc_sink_ops_t* hpu_unix_sink_ops(void);
#endif
#if defined(HPULOGC_SINK_FIFO)
const hpulogc_sink_ops_t* hpu_fifo_sink_ops(void);
#endif

/* Test-only I/O failure injection (see docs/implementation_notes.md).
 * op: 0 = open, 1 = write, 2 = fsync; return non-zero to force the
 * operation to fail. NULL (default) = never fail. */
extern int (*hpu_io_fail_hook)(int op, const char* path);

#endif /* HPU_OUTPUT_H */
