/**
 * @file ringbuf.h
 * @brief Internal ring buffer contract shared by both implementations.
 *
 * Exactly one of ringbuf_locked.c / ringbuf_lockfree.c is compiled per
 * build (spec 3.3); both provide the same API over a common variable-size
 * record layout. Records are kept contiguous via explicit pad records so
 * the consumer reads them in place without copies.
 */

#ifndef HPU_RINGBUF_H
#define HPU_RINGBUF_H

#include <stddef.h>
#include <stdint.h>

#include "../atomic/hpulogc_atomic.h"
#include "../platform/platform.h"

/** @brief Opaque ring handle type; layout is implementation-private. */
typedef struct hpu_ring hpu_ring_t;

/** @brief Record flags (hpu_ring_meta_t::level_flags high byte). */
#define HPU_REC_FLAG_PAD 0x01 /*!< Padding-only record, skip it */

/**
 * @brief Record header placed at the start of every ring record.
 *
 * @p commit carries the lock-free commit marker (logical position + 1;
 * 0 = reserved/not committed). The locked implementation ignores it.
 * Strings follow the header in order: category, file, func, msg; lengths
 * are byte counts without NUL terminators.
 */
typedef struct hpu_ring_meta {
    uint64_t commit;       /*!< Lock-free commit marker (pos + 1) */
    uint64_t tid;          /*!< Producer OS thread id */
    int64_t  realtime_ns;  /*!< CLOCK_REALTIME nanoseconds at capture */
    int64_t  mono_us;      /*!< CLOCK_MONOTONIC microseconds since init */
    uint32_t total_len;    /*!< Whole record length incl. header and pad */
    uint32_t msg_len;      /*!< Rendered message byte count */
    uint32_t line;         /*!< Source line (0 when absent) */
    uint32_t level_flags;  /*!< level:8 | flags:8 | reserved:16 */
    uint16_t category_len; /*!< Category byte count */
    uint16_t file_len;     /*!< File name byte count */
    uint16_t func_len;     /*!< Function name byte count */
    uint16_t reserved;     /*!< Padding to 8-byte alignment */
} hpu_ring_meta_t;         /* 48 bytes */

/** @brief Extract the severity level from level_flags. */
#define HPU_REC_LEVEL(lf)  ((int)((lf) & 0xFFU))
/** @brief Extract the record flags from level_flags. */
#define HPU_REC_FLAGS(lf)  ((int)(((lf) >> 8) & 0xFFU))

/** @brief Producer-side descriptor of one record. */
typedef struct hpu_ring_msg {
    uint8_t     level;         /*!< Severity */
    uint8_t     rec_flags;     /*!< Record flags (normal records: 0) */
    const char* category;      /*!< Category bytes (may be NULL) */
    uint32_t    category_len;  /*!< Category byte count */
    const char* file;          /*!< File bytes (may be NULL) */
    uint32_t    file_len;      /*!< File byte count */
    const char* func;          /*!< Function bytes (may be NULL) */
    uint32_t    func_len;      /*!< Function byte count */
    uint32_t    line;          /*!< Source line */
    uint64_t    tid;           /*!< Producer thread id */
    int64_t     realtime_ns;   /*!< Realtime capture timestamp */
    int64_t     mono_us;       /*!< Monotonic capture timestamp */
    const char* msg;           /*!< Rendered message bytes */
    uint32_t    msg_len;       /*!< Message byte count */
} hpu_ring_msg_t;

/** @brief Consumer-side view of one record (pointers into ring memory,
 *         valid until the next hpu_ring_get() call). */
typedef struct hpu_ring_view {
    hpu_ring_meta_t meta;      /*!< Copy of the record header */
    const char*     category;  /*!< Category bytes (in ring) */
    const char*     file;      /*!< File bytes (in ring) */
    const char*     func;      /*!< Function bytes (in ring) */
    const char*     msg;       /*!< Message bytes (in ring) */
} hpu_ring_view_t;

/** @brief Ring operation results. */
#define HPU_RING_OK      0 /*!< Record transferred */
#define HPU_RING_EMPTY   1 /*!< Nothing available within the timeout */
#define HPU_RING_DROPPED 2 /*!< Record dropped per overflow policy */

/**
 * @brief Create a ring buffer.
 *
 * @param capacity_bytes  Ring size in bytes (rounded up to 8; must be at
 *                        least twice the largest record, guaranteed by the
 *                        caller).
 * @param policy          hpulogc_overflow_policy_t value.
 * @param spsc            Non-zero when the build concurrency is SPSC.
 * @return                Ring handle or NULL on allocation failure.
 */
hpu_ring_t* hpu_ring_create(size_t capacity_bytes, int policy, int spsc);

/**
 * @brief Destroy a ring buffer and free its memory.
 * @param r  Ring handle (NULL is safe).
 */
void hpu_ring_destroy(hpu_ring_t* r);

/**
 * @brief Free a ring buffer WITHOUT destroying its sync primitives.
 *
 * For the fork(2) child path only: inherited condvars may still have
 * waiters registered in the parent, and destroying them would block.
 * The memory is released; the primitives are intentionally abandoned.
 *
 * @param r  Ring handle (NULL is safe).
 */
void hpu_ring_discard(hpu_ring_t* r);

/**
 * @brief Enqueue one record (blocks under the wait policy).
 *
 * @param r    Ring handle.
 * @param msg  Record descriptor; strings are copied into the ring.
 * @return     HPU_RING_OK when enqueued, HPU_RING_DROPPED when the record
 *             was dropped (discard policy, full ring, or ring closed).
 */
int hpu_ring_put(hpu_ring_t* r, const hpu_ring_msg_t* msg);

/**
 * @brief Retrieve the next record (consumer side).
 *
 * The record bytes are copied into @p staging (which must be at least as
 * large as the biggest possible record) before the internal read cursor
 * advances; the returned view points into @p staging and stays valid until
 * the next hpu_ring_get(). Copying makes the view immune to the overwrite
 * policy reusing ring memory while the consumer formats.
 *
 * @param r             Ring handle.
 * @param out           Filled with a view into @p staging.
 * @param staging       Consumer-provided buffer receiving the record.
 * @param staging_len   Size of @p staging in bytes.
 * @param timeout_ms    Maximum wait when the ring is empty (0 = poll).
 * @return              HPU_RING_OK when a record was retrieved (padding
 *                      records are skipped internally), HPU_RING_EMPTY on
 *                      timeout or when closed and drained.
 */
int hpu_ring_get(hpu_ring_t* r, hpu_ring_view_t* out, void* staging,
                 size_t staging_len, uint32_t timeout_ms);

/**
 * @brief Wake up the consumer immediately (used by flush/shutdown).
 * @param r  Ring handle.
 */
void hpu_ring_kick_consumer(hpu_ring_t* r);

/**
 * @brief Mark the ring closed: producers must stop, consumer may drain.
 * @param r  Ring handle.
 */
void hpu_ring_close(hpu_ring_t* r);

/**
 * @brief Current occupied byte count (including unconsumed records).
 * @param r  Ring handle.
 * @return   Occupied bytes.
 */
size_t hpu_ring_used(const hpu_ring_t* r);

/**
 * @brief Ring capacity in bytes.
 * @param r  Ring handle.
 * @return   Capacity.
 */
size_t hpu_ring_capacity(const hpu_ring_t* r);

/**
 * @brief Read the ring-side drop counters.
 *
 * @param r            Ring handle.
 * @param dropped      Filled with discard-policy drops (may be NULL).
 * @param overwritten  Filled with overwrite-policy removals (may be NULL).
 */
void hpu_ring_counters(const hpu_ring_t* r, unsigned long long* dropped,
                       unsigned long long* overwritten);

#endif /* HPU_RINGBUF_H */
