/* -*- coding: utf-8 -*- */

/**
 * @file core_internal.h
 * @brief Internal runtime state, registry/throttle structures and the
 *        shared core API used by the api/pipeline/consumer/hotreload
 *        translation units.
 *
 * History note: this module was reconstructed during Phase 2. The Phase 1
 * baseline shipped without src/core/ (see docs/implementation_notes.md,
 * "Phase 2 (Windows)" section). It is written against the frozen public
 * contract and the Phase 1 decision record; concurrency primitives use the
 * platform contract types (hpu_mutex_t) as prescribed by the Phase 1
 * defect list entry A.
 */

#ifndef HPU_CORE_INTERNAL_H
#define HPU_CORE_INTERNAL_H

#include "hpulogc.h"

#include "../conf/conf_model.h"
#include "../format/format.h"
#include "../output/output.h"
#include "../platform/platform.h"
#include "../ring/ringbuf.h"

/** @brief Registered-category storage limit (spec 4.9). */
#ifndef HPU_REG_MAX
#define HPU_REG_MAX HPULOGC_MAX_CATEGORIES
#endif

/** @brief Registry entry name capacity. Categories are arbitrary byte
 *         strings; entries longer than this take the un-cached slow path
 *         (routing still works, only the per-level cache is unavailable). */
#define HPU_REG_NAME_LEN 128

/** @brief Sentinel for "no per-category level override" (decision 19). */
#define HPU_REG_NO_OVERRIDE 0xFFu

/**
 * @brief One registered category slot.
 *
 * Published with the count protocol (decision 14): the writer fills the
 * slot under the registry mutex and then releases-increments the count;
 * readers acquire-load the count and scan linearly. Routing caches are
 * keyed by the configuration generation and recomputed lazily under the
 * configuration lock.
 */
typedef struct hpu_reg_entry {
    char     name[HPU_REG_NAME_LEN]; /*!< Category name bytes */
    uint32_t name_len;               /*!< Category name length */
    uint32_t hash;                   /*!< FNV-1a hash of the name */
    uint8_t  level_override;         /*!< HPU_REG_NO_OVERRIDE or a level */
    int      rule_for_level[7];      /*!< Cached rule index per level, -1 */
    uint32_t gen;                    /*!< Configuration generation of the cache */
#if HPULOGC_ENABLE_THROTTLE
    double   tokens;                 /*!< Per-category token bucket level */
    int64_t  last_refill_ns;         /*!< Last bucket refill timestamp */
#endif
} hpu_reg_entry_t;

/**
 * @brief Category registry (separate BSS symbol so CATEGORY=OFF builds
 *        let the linker trim it; decision 12).
 */
typedef struct hpu_registry {
    hpu_mutex_t      mu;              /*!< Guards registration + caches */
    hpu_reg_entry_t  entries[HPU_REG_MAX]; /*!< Fixed slot array */
    hpu_atomic_u32   count;           /*!< Published slot count */
    uint32_t         warned_hash[HPU_REG_MAX]; /*!< One-warning-per-category hashes */
    size_t           warned_count;    /*!< Number of recorded warn hashes */
} hpu_registry_t;

/**
 * @brief Global runtime state (spec 4.9/7.5/9).
 *
 * hpu_runtime_t is deliberately small (~1 KB BSS); the registry lives in
 * its own symbol (decision 12).
 */
typedef struct hpu_runtime {
    int              state;            /*!< hpu_runtime_state_t value */
    hpu_mutex_t      conf_lock;        /*!< Serializes conf readers/writer */
    hpu_conf_t*      conf;             /*!< Active snapshot (guarded) */
    hpu_atomic_u32   conf_gen;         /*!< Bumped on every snapshot swap */
    /* Producer fast-path copies of hot config values (updated under
     * conf_lock at init/reload; read lock-free). */
    hpu_atomic_u32   level_atomic;     /*!< Global filter threshold */
    hpu_atomic_u32   ts_source;        /*!< hpulogc_timestamp_source_t */
    hpu_atomic_u32   escape_injection; /*!< Injection escaping enabled */
    hpu_atomic_u32   max_log_len_atomic; /*!< Producer render cap (bytes) */
    /* Async pipeline (HPULOGC_ENABLE_ASYNC builds only) */
    hpu_ring_t*      ring;             /*!< Ring buffer, NULL in sync builds */
    hpu_thread_t     consumer_thread;  /*!< Consumer handle */
    hpu_mutex_t      consumer_mu;      /*!< Exit-wait mutex */
    hpu_cond_t       consumer_cond;    /*!< Exit-wait cond */
    hpu_atomic_u32   consumer_exit;    /*!< Consumer stop request */
    hpu_atomic_u32   consumer_alive;   /*!< Consumer running flag */
    hpu_atomic_u64   flush_req;        /*!< Flush request counter */
    hpu_atomic_u64   flush_done;       /*!< Last serviced flush counter */
    /* Hot reload (HPULOGC_ENABLE_HOT_RELOAD builds only) */
    hpu_watcher_t    watcher;          /*!< Platform watcher state */
    hpu_thread_t     watcher_thread;   /*!< Watcher thread handle */
    hpu_atomic_u32   watcher_exit;     /*!< Watcher stop request */
    int              watcher_started;  /*!< Non-zero after successful start */
    int              reload_in_progress; /*!< Serializes reload triggers */
    /* Signal-triggered reload */
    int              signal_reload_installed; /*!< hpu_signal handler owner */
    /* Init source: "" = code config, else config file path (reinit) */
    char             source_path[HPULOGC_MAX_PATH_LEN];
    /* Fork handling (POSIX only; accepted-but-ignored on Windows) */
    int              fork_behavior;    /*!< HPU_FORK_* value */
    int              fork_child;       /*!< Set by the atfork child handler */
    hpu_atomic_u32   fork_dirty;       /*!< Lazy-rebuild request (child) */
    int              fork_disabled;    /*!< disable behavior: drop logs */
    /* Statistics (written side; accepted counted in the pipeline) */
    hpu_atomic_u64   st_accepted;      /*!< Records passing the filter */
    hpu_atomic_u64   st_written;       /*!< Records dispatched to outputs */
    hpu_atomic_u64   st_throttled;     /*!< Throttle/sampling drops */
    hpu_atomic_u64   st_retired_lost;  /*!< Lost lines of closed outputs */
    /* Periodic stats report bookkeeping (async builds) */
    int64_t          stats_last_ns;    /*!< Last stats report timestamp */
} hpu_runtime_t;

/** @brief Runtime state values. */
enum hpu_runtime_state {
    HPU_RT_UNINITIALIZED = 0, /*!< Before init / after shutdown */
    HPU_RT_INITIALIZING  = 1, /*!< Inside init (owner is the init thread) */
    HPU_RT_RUNNING       = 2, /*!< Fully initialized */
    HPU_RT_SHUTDOWN      = 3  /*!< Shutdown in progress */
};

/** @brief Global runtime instance (core.c). */
extern hpu_runtime_t g_rt;

/** @brief Global registry instance (registry.c). */
extern hpu_registry_t g_reg;

/* ---- core.c: lifecycle and controls --------------------------------- */

/**
 * @brief Bring a finalized configuration snapshot live (ring, consumer,
 *        watcher, registry reset). Called with state = INITIALIZING.
 * @param c  Finalized snapshot (ownership transfers to the runtime).
 * @return   0 on success, negative error code (runtime torn down).
 */
int hpu_core_start(hpu_conf_t* c);

/**
 * @brief Tear everything down; state must not be RUNNING.
 */
void hpu_core_stop(void);

/**
 * @brief Map the configured overflow policy to the ring policy code,
 *        enforcing build availability (spec 4.3; also enforced earlier by
 *        hpu_conf_finalize for config-file paths).
 * @return Ring policy value.
 */
int hpu_core_ring_policy(void);

/**
 * @brief Fold open outputs' lost-line counters into the retired total
 *        (decision 16; called before closing outputs).
 */
void hpu_core_retire_output_lost(void);

/**
 * @brief Fork child gate (reinit/disable behavior, spec 9). Called by the
 *        producer path; returns non-zero when the record must be dropped.
 *        Always 0 on Windows (no fork).
 */
int hpu_core_fork_check(void);

/* ---- registry.c ------------------------------------------------------ */

/**
 * @brief Look up (and register on first sight) a category slot.
 *
 * Registration may print a one-time stderr warning when the table is
 * full; the caller keeps operating without a slot in that case.
 *
 * @param category  Category bytes (NULL/"" handled as "*").
 * @param len       Category byte count.
 * @return          Slot pointer or NULL when unregistered/full/too long.
 */
hpu_reg_entry_t* hpu_registry_lookup(const char* category, size_t len);

/**
 * @brief Find a registered slot without registering (fast path).
 * @return Slot pointer or NULL.
 */
hpu_reg_entry_t* hpu_registry_find(const char* category, size_t len);

/**
 * @brief Recompute the per-level rule cache of one slot (must hold the
 *        conf lock; @p conf is the active snapshot).
 */
void hpu_registry_refresh_route(hpu_reg_entry_t* slot,
                                const hpu_conf_t* conf, uint32_t gen);

/**
 * @brief Reset the registry to empty (init path, single-threaded).
 */
void hpu_registry_reset(void);

/**
 * @brief Release registry sync primitives (shutdown path).
 */
void hpu_registry_shutdown(void);

/**
 * @brief Initialize registry sync primitives (init path).
 * @return 0 on success, negative on failure.
 */
int hpu_registry_init(void);

/**
 * @brief Set the per-category level override (set_level_for_category).
 * @param category  Category name (must be registered or registrable).
 * @param level     New level or (hpulogc_level_t)-1 to clear.
 * @return          0 on success, negative error code.
 */
int hpu_registry_set_override(const char* category, hpulogc_level_t level);

/* ---- pipeline.c ------------------------------------------------------ */

/**
 * @brief Producer entry: steps 2/3/4 of the pipeline (spec 4.9).
 *
 * Level filtering, throttling, message rendering into the TLS buffer and
 * enqueue (async) or immediate route+write (sync). Never fails to the
 * caller; drops are reflected in the statistics.
 *
 * @param level        Severity.
 * @param category     Category bytes (NULL allowed).
 * @param file         Source file (NULL allowed).
 * @param line         Source line.
 * @param func         Function name (NULL allowed).
 * @param fmt          printf-style format.
 * @param ap           Argument list (consumed).
 */
void hpu_pipeline_submit(hpulogc_level_t level, const char* category,
                         const char* file, int line, const char* func,
                         const char* fmt, va_list ap);

/**
 * @brief Create the TLS key for the per-thread render buffers (init).
 * @return 0 on success, -1 on failure.
 */
int hpu_pipeline_tls_init(void);

/**
 * @brief Free the calling thread's render buffer and destroy the TLS key
 *        (shutdown).
 */
void hpu_pipeline_tls_shutdown(void);

/**
 * @brief Route + render + write one record to the active configuration.
 *
 * Shared by the sync path and the consumer thread. The caller must hold
 * the conf lock (conf readers hold it; see decision 17).
 *
 * @param rec  Record to process (msg bytes are borrowed).
 * @return     0 when the record was written to at least one output,
 *             -1 when it was dropped (no route or all writes failed).
 */
int hpu_pipeline_process(const hpu_log_record_t* rec);

/**
 * @brief Match a routing selector against a category (spec 10.3 rules).
 * @param rule_sel  Selector: "*", exact name, or "name.*".
 * @param cat       Category bytes.
 * @param cat_len   Category byte count.
 * @return          Non-zero when the selector matches.
 */
int hpu_pipeline_selector_match(const char* rule_sel, const char* cat,
                                size_t cat_len);

/* ---- consumer.c (HPULOGC_ENABLE_ASYNC builds) ------------------------ */

/**
 * @brief Start the consumer thread for the current runtime.
 * @return 0 on success, negative on failure.
 */
int hpu_consumer_start(void);

/**
 * @brief Request the consumer to stop; joins it (bounded by the shutdown
 *        timeout when the timeout is non-zero).
 */
void hpu_consumer_stop(void);

/**
 * @brief Flush handshake: wait until the consumer submitted everything
 *        already enqueued.
 * @return 0 on success, -1 when not serviced in time.
 */
int hpu_consumer_flush(void);

/* ---- hotreload.c (HPULOGC_ENABLE_HOT_RELOAD builds) ------------------ */

/**
 * @brief Start the watcher thread for g_rt.source_path.
 * @return 0 on success, negative on failure.
 */
int hpu_hotreload_start(void);

/**
 * @brief Stop the watcher thread.
 */
void hpu_hotreload_stop(void);

/**
 * @brief Run one reload cycle (parse new config; swap on success, keep
 *        the old configuration on failure). Serialized internally; also
 *        used by the tests via the public-internal name.
 * @return 0 when the reload succeeded, -1 when it failed and rolled back.
 */
int hpu_core_trigger_reload(void);

/* ---- signal_safe.c --------------------------------------------------- */

/**
 * @brief Enable the async-signal-safe channel (init, signal_safe = true).
 */
void hpu_signal_safe_enable(void);

/**
 * @brief Disable the channel (shutdown).
 */
void hpu_signal_safe_disable(void);

#endif /* HPU_CORE_INTERNAL_H */
