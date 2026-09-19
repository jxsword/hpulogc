/**
 * @file ringbuf_locked.c
 * @brief Locked ring buffer (HPULOGC_LOCKFREE=OFF, default).
 *
 * Platform mutex + condition variables (contract in src/platform/) protect
 * a byte ring of contiguous records. Supports SPSC and MPSC (the mutex
 * serializes producers) and all three overflow policies: discard,
 * overwrite (oldest unconsumed record removed by the producer) and wait.
 */

#include "ringbuf.h"

#include <stdlib.h>
#include <string.h>

/** @brief Round up to the 8-byte record alignment. */
static size_t align8(size_t n)
{
    return (n + 7U) & ~(size_t)7U;
}

/** @brief Overflow policy codes (mirrors hpulogc_overflow_policy_t). */
enum {
    RB_POL_DISCARD   = 0,
    RB_POL_OVERWRITE = 1,
    RB_POL_WAIT      = 2
};

struct hpu_ring {
    size_t      capacity;     /*!< Ring size in bytes (multiple of 8) */
    char*       buf;          /*!< Ring storage */
    int         policy;       /*!< One of RB_POL_* */
    int         spsc;         /*!< Build concurrency (unused by this impl) */
    hpu_mutex_t mu;           /*!< Protects all fields below */
    hpu_cond_t  not_empty;    /*!< Signaled after a record is appended */
    hpu_cond_t  not_full;     /*!< Signaled after space is freed */
    size_t      head;         /*!< Monotonic read cursor */
    size_t      tail;         /*!< Monotonic write cursor */
    unsigned long long dropped;     /*!< discard-policy drops */
    unsigned long long overwritten; /*!< overwrite-policy removals */
    int         closed;       /*!< Set by hpu_ring_close() */
};

/**
 * @brief Fill a record header from the producer descriptor.
 */
static void fill_meta(hpu_ring_meta_t* meta, const hpu_ring_msg_t* msg,
                      uint32_t total_len)
{
    memset(meta, 0, sizeof(*meta));
    meta->commit       = 0; /* unused by the locked ring */
    meta->tid          = msg->tid;
    meta->realtime_ns  = msg->realtime_ns;
    meta->mono_us      = msg->mono_us;
    meta->total_len    = total_len;
    meta->msg_len      = msg->msg_len;
    meta->line         = msg->line;
    meta->level_flags  = (uint32_t)msg->level |
                         ((uint32_t)msg->rec_flags << 8);
    meta->category_len = (uint16_t)msg->category_len;
    meta->file_len     = (uint16_t)msg->file_len;
    meta->func_len     = (uint16_t)msg->func_len;
    meta->reserved     = 0;
}

/**
 * @brief Total byte length a record with this payload occupies.
 */
static uint32_t record_len(const hpu_ring_msg_t* msg)
{
    size_t n = sizeof(hpu_ring_meta_t) + msg->category_len + msg->file_len +
               msg->func_len + msg->msg_len;
    return (uint32_t)align8(n);
}

/**
 * @brief Write one record at a contiguous position.
 * @param r      Ring.
 * @param pos    Monotonic write position (physical: pos % capacity).
 * @param msg    Record descriptor.
 * @param total  Length to encode (may include tail padding).
 * @return       Written length.
 */
static uint32_t write_record(hpu_ring_t* r, size_t pos,
                             const hpu_ring_msg_t* msg, uint32_t total)
{
    size_t off = pos % r->capacity;
    char* base = r->buf + off;
    hpu_ring_meta_t* meta = (hpu_ring_meta_t*)(void*)base;
    char* p = base + sizeof(*meta);

    fill_meta(meta, msg, total);

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
    return total;
}

/**
 * @brief Write a padding record that spans to the ring boundary.
 * @return Padding length.
 */
static uint32_t write_pad(hpu_ring_t* r, size_t pos)
{
    size_t off = pos % r->capacity;
    char* base = r->buf + off;
    hpu_ring_meta_t* meta = (hpu_ring_meta_t*)(void*)base;
    uint32_t pad = (uint32_t)(r->capacity - off);

    memset(meta, 0, sizeof(*meta));
    meta->total_len   = pad;
    meta->level_flags = (uint32_t)HPU_REC_FLAG_PAD << 8;
    return pad;
}

/**
 * @brief Parse the header at the read cursor and expose a consumer view.
 * @return Record total length.
 */
static uint32_t read_record(const hpu_ring_t* r, size_t pos,
                            hpu_ring_view_t* out)
{
    size_t off = pos % r->capacity;
    const char* base = r->buf + off;
    const hpu_ring_meta_t* meta = (const hpu_ring_meta_t*)(const void*)base;
    const char* p = base + sizeof(*meta);

    out->meta = *meta;
    if (meta->category_len > 0) {
        out->category = p;
        p += meta->category_len;
    } else {
        out->category = NULL;
    }
    if (meta->file_len > 0) {
        out->file = p;
        p += meta->file_len;
    } else {
        out->file = NULL;
    }
    if (meta->func_len > 0) {
        out->func = p;
        p += meta->func_len;
    } else {
        out->func = NULL;
    }
    out->msg = p;
    return meta->total_len;
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
    r->capacity = align8(capacity_bytes);
    r->policy   = policy;
    r->spsc     = spsc;
    r->buf      = malloc(r->capacity);
    if (r->buf == NULL) {
        free(r);
        return NULL;
    }
    if (hpu_mutex_init(&r->mu) != 0 || hpu_cond_init(&r->not_empty) != 0 ||
        hpu_cond_init(&r->not_full) != 0) {
        hpu_mutex_destroy(&r->mu);
        hpu_cond_destroy(&r->not_empty);
        hpu_cond_destroy(&r->not_full);
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
    hpu_cond_destroy(&r->not_empty);
    hpu_cond_destroy(&r->not_full);
    hpu_mutex_destroy(&r->mu);
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
    uint32_t need;
    int rc = HPU_RING_OK;

    need = record_len(msg);

    hpu_mutex_lock(&r->mu);
    for (;;) {
        size_t used = r->tail - r->head;
        size_t free_bytes = r->capacity - used;
        size_t contig = r->capacity - (r->tail % r->capacity);
        size_t min_pad = align8(sizeof(hpu_ring_meta_t));

        if (r->closed) {
            r->dropped++;
            rc = HPU_RING_DROPPED;
            break;
        }

        /* Keep every record contiguous: pad to the boundary when needed. */
        if (contig < need) {
            /* Padding is always at least min_pad: writers extend their
             * record to the boundary instead of leaving a tiny gap. */
            if (free_bytes >= contig) {
                r->tail += write_pad(r, r->tail);
                hpu_cond_signal(&r->not_empty);
                continue;
            }
            /* not enough space even for the pad: fall through to policy */
        } else if (free_bytes >= need) {
            uint32_t total = (uint32_t)need;

            if (contig - need < min_pad) {
                /* Extend this record to the boundary; a gap smaller than
                 * the header could not hold a pad record. */
                total = (uint32_t)contig;
            }
            r->tail += write_record(r, r->tail, msg, total);
            hpu_cond_signal(&r->not_empty);
            rc = HPU_RING_OK;
            break;
        }

        /* Ring full for this record: apply the overflow policy. */
        if (r->policy == RB_POL_OVERWRITE) {
            /* Drop the oldest unconsumed records until it fits. Padding
             * records are removed but not counted as overwritten. */
            while (r->capacity - (r->tail - r->head) < need) {
                hpu_ring_meta_t* oldest =
                    (hpu_ring_meta_t*)(void*)(r->buf + r->head % r->capacity);
                int is_pad =
                    (HPU_REC_FLAGS(oldest->level_flags) & HPU_REC_FLAG_PAD);
                r->head += oldest->total_len;
                if (!is_pad) {
                    r->overwritten++;
                }
            }
            hpu_cond_signal(&r->not_empty);
            hpu_cond_broadcast(&r->not_full);
            continue;
        }
        if (r->policy == RB_POL_WAIT) {
            /* Buffer >= 2 x max record guarantees eventual progress. */
            hpu_cond_wait(&r->not_full, &r->mu);
            continue;
        }
        /* discard */
        r->dropped++;
        rc = HPU_RING_DROPPED;
        break;
    }
    hpu_mutex_unlock(&r->mu);
    return rc;
}

int hpu_ring_get(hpu_ring_t* r, hpu_ring_view_t* out, void* staging,
                 size_t staging_len, uint32_t timeout_ms)
{
    uint64_t deadline = 0;
    int rc = HPU_RING_EMPTY;

    if (timeout_ms > 0) {
        deadline = hpu_now_ns() / 1000000ULL + timeout_ms;
    }

    hpu_mutex_lock(&r->mu);
    for (;;) {
        if (r->head != r->tail) {
            uint32_t total;
            hpu_ring_view_t view;

            total = read_record(r, r->head, &view);

            if (HPU_REC_FLAGS(view.meta.level_flags) & HPU_REC_FLAG_PAD) {
                r->head += total;
                hpu_cond_broadcast(&r->not_full);
                continue; /* skip padding, keep looking */
            }

            /* Copy the record out before advancing so the overwrite
             * policy cannot reuse the memory under the consumer. */
            if (staging != NULL && staging_len >= total) {
                hpu_ring_meta_t* sm = (hpu_ring_meta_t*)(void*)staging;
                char* p = (char*)staging + sizeof(hpu_ring_meta_t);

                memcpy(staging, r->buf + r->head % r->capacity, total);
                out->meta = *sm;
                out->category = sm->category_len > 0 ? p : NULL;
                p += sm->category_len;
                out->file = sm->file_len > 0 ? p : NULL;
                p += sm->file_len;
                out->func = sm->func_len > 0 ? p : NULL;
                p += sm->func_len;
                out->msg = p;
            } else {
                *out = view; /* no staging: view into ring (caller beware) */
            }

            r->head += total;
            hpu_cond_broadcast(&r->not_full);
            rc = HPU_RING_OK;
            break;
        }
        if (r->closed || timeout_ms == 0) {
            rc = HPU_RING_EMPTY;
            break;
        }
        {
            uint64_t now = hpu_now_ns() / 1000000ULL;
            if (now >= deadline) {
                rc = HPU_RING_EMPTY;
                break;
            }
            hpu_cond_timedwait_ms(&r->not_empty, &r->mu,
                                  (uint32_t)(deadline - now));
        }
    }
    hpu_mutex_unlock(&r->mu);
    return rc;
}

void hpu_ring_kick_consumer(hpu_ring_t* r)
{
    hpu_mutex_lock(&r->mu);
    hpu_cond_broadcast(&r->not_empty);
    hpu_mutex_unlock(&r->mu);
}

void hpu_ring_close(hpu_ring_t* r)
{
    hpu_mutex_lock(&r->mu);
    r->closed = 1;
    hpu_cond_broadcast(&r->not_empty);
    hpu_cond_broadcast(&r->not_full);
    hpu_mutex_unlock(&r->mu);
}

size_t hpu_ring_used(const hpu_ring_t* r)
{
    size_t used;

    hpu_mutex_lock((hpu_mutex_t*)&r->mu);
    used = r->tail - r->head;
    hpu_mutex_unlock((hpu_mutex_t*)&r->mu);
    return used;
}

size_t hpu_ring_capacity(const hpu_ring_t* r)
{
    return r->capacity;
}

void hpu_ring_counters(const hpu_ring_t* r, unsigned long long* dropped,
                       unsigned long long* overwritten)
{
    hpu_mutex_lock((hpu_mutex_t*)&r->mu);
    if (dropped != NULL) {
        *dropped = r->dropped;
    }
    if (overwritten != NULL) {
        *overwritten = r->overwritten;
    }
    hpu_mutex_unlock((hpu_mutex_t*)&r->mu);
}
