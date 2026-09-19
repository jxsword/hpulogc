/**
 * @file ringbuf_lockfree.c
 * @brief Lock-free ring buffer (HPULOGC_LOCKFREE=ON).
 *
 * Algorithm (variable-size records over a byte ring):
 *  - Producers reserve a byte range with a fetch-and-CAS on a monotonic
 *    reservation cursor (`enq`); the space check compares against a stale
 *    read of the consume cursor, which is always conservative because the
 *    cursor is monotonic.
 *  - Records are kept contiguous via explicit pad records at the ring
 *    boundary, so consumers read headers and payloads without split copies.
 *  - Publication uses a commit marker equal to (logical position + 1),
 *    stored with release; consumers validate it with acquire loads before
 *    and after copying (seqlock-style), which also detects the overwrite
 *    policy racing the consumer.
 *  - MPSC builds support discard only; SPSC builds additionally support
 *    overwrite (the producer CAS-advances the consume cursor over the
 *    oldest records). Wait is rejected at init by the configuration layer
 *    (spec 4.3); if it ever reaches this file it degrades to discard.
 *
 * The overwrite path performs a deliberate seqlock read that TSan flags by
 * design when producers lap a slow consumer; the ring unit tests for that
 * combination are therefore excluded from TSan runs (documented in
 * docs/implementation_notes.md).
 */

#include "ringbuf.h"

#include <stdlib.h>
#include <string.h>

/** @brief Record alignment in bytes. */
#define LF_ALIGN 8U

/**
 * @brief Lock-free header view: layout-compatible with hpu_ring_meta_t,
 *        but with the commit word accessed atomically.
 */
typedef struct hpu_lf_head {
    hpu_atomic_u64 commit; /*!< Commit marker (logical pos + 1) */
    uint64_t       tid;
    int64_t        realtime_ns;
    int64_t        mono_us;
    uint32_t       total_len;
    uint32_t       msg_len;
    uint32_t       line;
    uint32_t       level_flags;
    uint16_t       category_len;
    uint16_t       file_len;
    uint16_t       func_len;
    uint16_t       reserved;
} hpu_lf_head_t;

/** @brief Compile-time layout checks against hpu_ring_meta_t. */
typedef char hpu_lf_size_assert[(sizeof(hpu_lf_head_t) ==
                                 sizeof(hpu_ring_meta_t)) ? 1 : -1];
typedef char hpu_lf_total_assert[(offsetof(hpu_lf_head_t, total_len) ==
                                  offsetof(hpu_ring_meta_t, total_len)) ? 1
                                                                         : -1];

/** @brief Overflow policy codes (mirrors hpulogc_overflow_policy_t). */
enum {
    RB_POL_DISCARD   = 0,
    RB_POL_OVERWRITE = 1,
    RB_POL_WAIT      = 2
};

struct hpu_ring {
    size_t      capacity;        /*!< Ring size in bytes (multiple of 8) */
    char*       buf;             /*!< Ring storage */
    int         policy;          /*!< One of RB_POL_* */
    int         spsc;            /*!< Non-zero for SPSC builds */
    hpu_atomic_u64 enq;          /*!< Monotonic reservation cursor */
    hpu_atomic_u64 deq;          /*!< Monotonic consume cursor */
    hpu_atomic_u64 dropped;      /*!< discard-policy drops */
    hpu_atomic_u64 overwritten;  /*!< overwrite-policy removals */
    hpu_atomic_u32 closed;       /*!< Set by hpu_ring_close() */
    hpu_atomic_u32 consumer_waiting; /*!< Consumer parked in lf_wait() */
    hpu_mutex_t kick_mu;         /*!< Guards the consumer wakeup */
    hpu_cond_t  kick;            /*!< Consumer wakeup signal */
};

/**
 * @brief Round up to the record alignment.
 */
static size_t lf_align8(size_t n)
{
    return (n + (LF_ALIGN - 1U)) & ~(size_t)(LF_ALIGN - 1U);
}

/**
 * @brief Total byte length a record with this payload occupies.
 */
static uint32_t lf_record_len(const hpu_ring_msg_t* msg)
{
    size_t n = sizeof(hpu_ring_meta_t) + msg->category_len + msg->file_len +
               msg->func_len + msg->msg_len;
    return (uint32_t)lf_align8(n);
}

/**
 * @brief Write a complete record (or pad) and publish it.
 *
 * @param r       Ring.
 * @param pos     Logical position of the record.
 * @param msg     Record descriptor (ignored for pads).
 * @param total   Record length.
 * @param is_pad  Non-zero to write a padding record.
 */
static void lf_write_record(hpu_ring_t* r, size_t pos,
                            const hpu_ring_msg_t* msg, uint32_t total,
                            int is_pad)
{
    hpu_lf_head_t* h = (hpu_lf_head_t*)(void*)(r->buf + pos % r->capacity);
    char* p = (char*)h + sizeof(*h);

    /* Invalidate the slot first so resync walks never observe a torn
     * header under a stale marker. */
    hpu_at_store_u64(&h->commit, 0, HPU_MO_RELAXED);

    if (is_pad) {
        h->tid         = 0;
        h->realtime_ns = 0;
        h->mono_us     = 0;
        h->msg_len     = 0;
        h->line        = 0;
        h->level_flags = (uint32_t)HPU_REC_FLAG_PAD << 8;
        h->category_len = 0;
        h->file_len    = 0;
        h->func_len    = 0;
        h->reserved    = 0;
        h->total_len   = total;
    } else {
        h->tid          = msg->tid;
        h->realtime_ns  = msg->realtime_ns;
        h->mono_us      = msg->mono_us;
        h->msg_len      = msg->msg_len;
        h->line         = msg->line;
        h->level_flags  = (uint32_t)msg->level |
                          ((uint32_t)msg->rec_flags << 8);
        h->category_len = (uint16_t)msg->category_len;
        h->file_len     = (uint16_t)msg->file_len;
        h->func_len     = (uint16_t)msg->func_len;
        h->reserved     = 0;
        h->total_len    = total;

        if (msg->category_len > 0) {
            memcpy(p, msg->category, msg->category_len);
            p += msg->category_len;
        }
        if (msg->file_len > 0) {
            memcpy(p, msg->file, msg->file_len);
            p += msg->file_len;
        }
        if (msg->func_len > 0) {
            memcpy(p, msg->func, msg->func_len);
            p += msg->func_len;
        }
        if (msg->msg_len > 0) {
            memcpy(p, msg->msg, msg->msg_len);
        }
    }

    /* Publish: all fields above are visible before the marker. */
    hpu_at_store_u64(&h->commit, (uint64_t)pos + 1, HPU_MO_RELEASE);
}

/**
 * @brief Conservative space check against the (monotonic) consume cursor.
 * @return Non-zero when [pos, pos+len) fits without lapping unconsumed data.
 */
static int lf_space_ok(const hpu_ring_t* r, uint64_t pos, uint64_t len)
{
    uint64_t deq_v = hpu_at_load_u64(&r->deq, HPU_MO_ACQUIRE);

    if (pos + len > deq_v + r->capacity) {
        deq_v = hpu_at_load_u64(&r->deq, HPU_MO_ACQUIRE); /* refresh */
        if (pos + len > deq_v + r->capacity) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Wake the consumer when it is parked in lf_wait().
 */
static void lf_kick(hpu_ring_t* r)
{
    if (hpu_at_load_u32(&r->consumer_waiting, HPU_MO_ACQUIRE) != 0) {
        hpu_mutex_lock(&r->kick_mu);
        hpu_cond_broadcast(&r->kick);
        hpu_mutex_unlock(&r->kick_mu);
    }
}

/**
 * @brief Park the consumer until kicked or the deadline passes.
 *
 * Must only be called when the ring was observed empty; the mutex
 * handshake with lf_kick() closes the missed-wakeup window.
 */
static void lf_wait(hpu_ring_t* r, uint64_t deadline_ms)
{
    uint64_t now = hpu_now_ns() / 1000000ULL;

    hpu_mutex_lock(&r->kick_mu);
    hpu_at_store_u32(&r->consumer_waiting, 1, HPU_MO_RELEASE);

    /* Re-check under the mutex so a producer that already saw waiting=0
     * cannot slip a wakeup past us. */
    if (hpu_at_load_u64(&r->deq, HPU_MO_RELAXED) >=
        hpu_at_load_u64(&r->enq, HPU_MO_ACQUIRE)) {
        if (now < deadline_ms) {
            (void)hpu_cond_timedwait_ms(&r->kick, &r->kick_mu,
                                        (uint32_t)(deadline_ms - now));
        }
    }
    hpu_at_store_u32(&r->consumer_waiting, 0, HPU_MO_RELAXED);
    hpu_mutex_unlock(&r->kick_mu);
}

/**
 * @brief Wait for the record at @p d to be committed.
 *
 * @return 1 committed, 0 deadline passed, -1 slot was overwritten (resync
 *         needed).
 */
static int lf_wait_commit(hpu_ring_t* r, uint64_t d, uint64_t deadline_ms)
{
    for (;;) {
        uint64_t m = hpu_at_load_u64(
            (const hpu_atomic_u64*)(const void*)(r->buf + d % r->capacity),
            HPU_MO_ACQUIRE);
        if (m == d + 1) {
            return 1;
        }
        if (m > d + 1) {
            return -1; /* our cursor is stale: reload deq */
        }
        /* m == 0 (being written, being invalidated, or stale older
         * marker): keep waiting, but bail out when deq moved. */
        if (hpu_at_load_u64(&r->deq, HPU_MO_ACQUIRE) != d) {
            return -1;
        }
        if (hpu_now_ns() / 1000000ULL >= deadline_ms) {
            return 0;
        }
        hpu_cpu_relax();
    }
}

/**
 * @brief Free space for a new record (SPSC overwrite policy only).
 *
 * CAS-advances the consume cursor over the oldest records until
 * [pos, pos+len) fits; each freed record counts as overwritten.
 *
 * @return Non-zero when enough space is available.
 */
static int lf_overwrite_make_space(hpu_ring_t* r, uint64_t pos, uint64_t len)
{
    if (r->policy != RB_POL_OVERWRITE || !r->spsc) {
        return 0;
    }
    for (;;) {
        uint64_t d = hpu_at_load_u64(&r->deq, HPU_MO_ACQUIRE);
        hpu_lf_head_t* h;
        uint32_t old_total;
        uint64_t expected;

        if (pos + len <= d + r->capacity) {
            return 1;
        }
        h = (hpu_lf_head_t*)(void*)(r->buf + d % r->capacity);
        old_total = h->total_len;
        if (old_total < sizeof(hpu_ring_meta_t) || old_total > r->capacity) {
            /* Should not happen (record at deq is committed); give up. */
            return 0;
        }
        /* Invalidate the slot BEFORE advancing deq: a consumer that is
         * mid-copy detects the torn read via its marker revalidation. */
        hpu_at_store_u64(&h->commit, 0, HPU_MO_RELAXED);
        expected = d;
        if (hpu_at_cas_u64(&r->deq, &expected, d + old_total,
                           HPU_MO_RELEASE, HPU_MO_ACQUIRE) != 0) {
            if (!(HPU_REC_FLAGS(h->level_flags) & HPU_REC_FLAG_PAD)) {
                hpu_at_fetch_add_u64(&r->overwritten, 1, HPU_MO_ACQ_REL);
            }
        }
        /* CAS failure: the consumer consumed the record first; the
         * invalidation only made it re-validate and retry. */
    }
}

/**
 * @brief Advance the consume cursor over the record at @p d.
 *
 * Uses CAS in overwrite builds where the producer also advances the
 * cursor; a plain store otherwise. Returns 0 when the CAS lost (another
 * writer moved deq first, meaning the record was freed by the overwrite
 * policy).
 */
static int lf_advance_deq(hpu_ring_t* r, uint64_t d, uint32_t total)
{
    if (r->policy == RB_POL_OVERWRITE && r->spsc) {
        uint64_t expected = d;
        return hpu_at_cas_u64(&r->deq, &expected, d + total, HPU_MO_RELEASE,
                              HPU_MO_ACQUIRE);
    }
    hpu_at_store_u64(&r->deq, d + total, HPU_MO_RELEASE);
    return 1;
}

/**
 * @brief Build the consumer view from the staging copy.
 */
static void lf_view_from_staging(hpu_ring_view_t* out, void* staging)
{
    hpu_ring_meta_t* sm = (hpu_ring_meta_t*)(void*)staging;
    char* p = (char*)staging + sizeof(*sm);

    out->meta = *sm;
    out->category = sm->category_len > 0 ? p : NULL;
    p += sm->category_len;
    out->file = sm->file_len > 0 ? p : NULL;
    p += sm->file_len;
    out->func = sm->func_len > 0 ? p : NULL;
    p += sm->func_len;
    out->msg = p;
}

/**
 * @brief Build the consumer view directly from ring memory (no staging
 *        was supplied; only safe when the overwrite policy cannot reuse
 *        the record while the consumer formats).
 */
static void lf_view_from_ring(hpu_ring_view_t* out, hpu_lf_head_t* h)
{
    char* base = (char*)h + sizeof(*h);

    out->meta.commit      = hpu_at_load_u64(&h->commit, HPU_MO_RELAXED);
    out->meta.tid         = h->tid;
    out->meta.realtime_ns = h->realtime_ns;
    out->meta.mono_us     = h->mono_us;
    out->meta.total_len   = h->total_len;
    out->meta.msg_len     = h->msg_len;
    out->meta.line        = h->line;
    out->meta.level_flags = h->level_flags;
    out->meta.category_len = h->category_len;
    out->meta.file_len    = h->file_len;
    out->meta.func_len    = h->func_len;
    out->meta.reserved    = h->reserved;

    out->category = h->category_len > 0 ? base : NULL;
    out->file     = base + h->category_len;
    out->func     = out->file + h->file_len;
    out->msg      = out->func + h->func_len;
}

hpu_ring_t* hpu_ring_create(size_t capacity_bytes, int policy, int spsc)
{
    hpu_ring_t* r;

    if (capacity_bytes < sizeof(hpu_ring_meta_t) * 2) {
        return NULL;
    }

    r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->capacity = lf_align8(capacity_bytes);
    r->policy   = (policy == RB_POL_OVERWRITE && !spsc) ? RB_POL_DISCARD
                                                        : policy;
    r->spsc     = spsc;
    r->buf      = malloc(r->capacity);
    if (r->buf == NULL) {
        free(r);
        return NULL;
    }
    if (hpu_mutex_init(&r->kick_mu) != 0 || hpu_cond_init(&r->kick) != 0) {
        hpu_mutex_destroy(&r->kick_mu);
        hpu_cond_destroy(&r->kick);
        free(r->buf);
        free(r);
        return NULL;
    }
    return r;
}

void hpu_ring_destroy(hpu_ring_t* r)
{
    if (r == NULL) {
        return;
    }
    hpu_cond_destroy(&r->kick);
    hpu_mutex_destroy(&r->kick_mu);
    free(r->buf);
    free(r);
}

void hpu_ring_discard(hpu_ring_t* r)
{
    if (r == NULL) {
        return;
    }
    /* fork-child path: skip primitive destruction (may have waiters) */
    free(r->buf);
    free(r);
}

int hpu_ring_put(hpu_ring_t* r, const hpu_ring_msg_t* msg)
{
    uint32_t total = lf_record_len(msg);

    if (hpu_at_load_u32(&r->closed, HPU_MO_ACQUIRE) != 0) {
        hpu_at_fetch_add_u64(&r->dropped, 1, HPU_MO_RELAXED);
        return HPU_RING_DROPPED;
    }

    for (;;) {
        uint64_t pos = hpu_at_load_u64(&r->enq, HPU_MO_RELAXED);
        size_t off_mod = (size_t)(pos % r->capacity);
        size_t min_pad = lf_align8(sizeof(hpu_ring_meta_t));

        if (off_mod + total > r->capacity) {
            /* Not enough contiguous space: publish a pad to the boundary.
             * Padding is always >= min_pad because writers extend their
             * record to the boundary instead of leaving a tiny gap. */
            uint32_t pad = (uint32_t)(r->capacity - off_mod);

            if (!lf_space_ok(r, pos, pad) &&
                !lf_overwrite_make_space(r, pos, pad)) {
                hpu_at_fetch_add_u64(&r->dropped, 1, HPU_MO_RELAXED);
                return HPU_RING_DROPPED;
            }
            if (r->spsc) {
                hpu_at_store_u64(&r->enq, pos + pad, HPU_MO_RELEASE);
            } else {
                uint64_t expected = pos;
                if (hpu_at_cas_u64(&r->enq, &expected, pos + pad,
                                   HPU_MO_ACQ_REL,
                                   HPU_MO_ACQUIRE) == 0) {
                    continue;
                }
            }
            lf_write_record(r, pos, msg, pad, 1);
            continue;
        }

        if (off_mod + total + min_pad > r->capacity) {
            /* Extend the record to the boundary: a gap smaller than the
             * header could not hold a pad record. */
            total = (uint32_t)(r->capacity - off_mod);
        }

        if (!lf_space_ok(r, pos, total)) {
            if (lf_overwrite_make_space(r, pos, total)) {
                continue; /* re-run the reservation with fresh space */
            }
            hpu_at_fetch_add_u64(&r->dropped, 1, HPU_MO_RELAXED);
            return HPU_RING_DROPPED;
        }

        /* Reserve the byte range. */
        if (r->spsc) {
            hpu_at_store_u64(&r->enq, pos + total, HPU_MO_RELEASE);
        } else {
            uint64_t expected = pos;
            if (hpu_at_cas_u64(&r->enq, &expected, pos + total,
                               HPU_MO_ACQ_REL, HPU_MO_ACQUIRE) == 0) {
                continue;
            }
        }

        lf_write_record(r, pos, msg, total, 0);
        lf_kick(r);
        return HPU_RING_OK;
    }
}

int hpu_ring_get(hpu_ring_t* r, hpu_ring_view_t* out, void* staging,
                 size_t staging_len, uint32_t timeout_ms)
{
    uint64_t now_ms = hpu_now_ns() / 1000000ULL;
    uint64_t deadline = timeout_ms > 0 ? now_ms + timeout_ms : now_ms;

    for (;;) {
        uint64_t d = hpu_at_load_u64(&r->deq, HPU_MO_RELAXED);
        uint64_t e = hpu_at_load_u64(&r->enq, HPU_MO_ACQUIRE);

        if (d < e) {
            int state = lf_wait_commit(r, d, deadline);

            if (state == 0) {
                return HPU_RING_EMPTY; /* producer still publishing */
            }
            if (state < 0) {
                continue; /* cursor stale: retry from the current deq */
            }

            {
                size_t off = (size_t)(d % r->capacity);
                hpu_lf_head_t* h = (hpu_lf_head_t*)(void*)(r->buf + off);
                uint32_t total = h->total_len;

                if (total < sizeof(hpu_ring_meta_t) ||
                    total > r->capacity) {
                    continue; /* torn header: revalidate from deq */
                }

                if (HPU_REC_FLAGS(h->level_flags) & HPU_REC_FLAG_PAD) {
                    (void)lf_advance_deq(r, d, total);
                    continue; /* padding skipped either way */
                }

                if (staging != NULL && staging_len < total) {
                    continue;
                }

                if (staging != NULL) {
                    memcpy(staging, r->buf + off, total);
                    /* seqlock revalidation after the copy */
                    if (hpu_at_load_u64(&h->commit, HPU_MO_ACQUIRE) !=
                        d + 1) {
                        continue; /* overwritten mid-copy: retry */
                    }
                    lf_view_from_staging(out, staging);
                } else {
                    lf_view_from_ring(out, h);
                }

                if (lf_advance_deq(r, d, total) == 0) {
                    /* Producer freed this record (overwrite): drop the
                     * copy without counting it written. */
                    continue;
                }
                return HPU_RING_OK;
            }
        }

        if (hpu_at_load_u32(&r->closed, HPU_MO_ACQUIRE) != 0 ||
            timeout_ms == 0) {
            return HPU_RING_EMPTY;
        }
        {
            uint64_t now = hpu_now_ns() / 1000000ULL;
            if (now >= deadline) {
                return HPU_RING_EMPTY;
            }
            lf_wait(r, deadline);
        }
    }
}

void hpu_ring_kick_consumer(hpu_ring_t* r)
{
    lf_kick(r);
}

void hpu_ring_close(hpu_ring_t* r)
{
    hpu_at_store_u32(&r->closed, 1, HPU_MO_RELEASE);
    lf_kick(r);
}

size_t hpu_ring_used(const hpu_ring_t* r)
{
    uint64_t enq_v = hpu_at_load_u64(&r->enq, HPU_MO_ACQUIRE);
    uint64_t deq_v = hpu_at_load_u64(&r->deq, HPU_MO_ACQUIRE);

    return (size_t)(enq_v - deq_v);
}

size_t hpu_ring_capacity(const hpu_ring_t* r)
{
    return r->capacity;
}

void hpu_ring_counters(const hpu_ring_t* r, unsigned long long* dropped,
                       unsigned long long* overwritten)
{
    if (dropped != NULL) {
        *dropped = hpu_at_load_u64(&r->dropped, HPU_MO_ACQUIRE);
    }
    if (overwritten != NULL) {
        *overwritten = hpu_at_load_u64(&r->overwritten, HPU_MO_ACQUIRE);
    }
}
