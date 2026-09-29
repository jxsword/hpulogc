/**
 * @file sink_queue.c
 * @brief Second-level delivery queue for async sinks (rd_v0.6 §4.10.6).
 *
 * One bounded byte-ring queue per async sink instance plus one dedicated
 * worker thread. Producers (consumer threads in async builds, caller
 * threads in sync builds) pack the event and enqueue WITHOUT blocking:
 * a full queue discards the new event and counts it in the sink's
 * `dropped` statistic (decision D-S5). The worker drains the queue in
 * batches, calls emit_batch/emit, and flushes periodically.
 *
 * Packed record layout (little-endian-agnostic, host byte order):
 *   u32 rec_len (whole record incl. this field)
 *   u8  level; u8 pad[3]
 *   u16 line_len, body_len, cat_len, file_len, func_len, field_count,
 *       fields_len, src_line
 *   i64 realtime_ns; i64 mono_us; u64 tid
 *   payload: line, body, category, file, func, fields_wire
 */

#include "sink_queue.h"

#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"
#include "../core/core_internal.h" /* hpu_core_sleep_ms (bounded drain) */

/** @brief Packed record header size (u32 + 4 + 16 + 8 + 16 + 8). */
#define SQ_HDR 56U

/**
 * @brief Queue state (base->queue for async instances).
 */
struct hpu_sink_queue {
    hpu_mutex_t mu;        /*!< Guards all fields below */
    hpu_cond_t cv;         /*!< Worker wakeup (data / stop) */
    uint8_t* buf;          /*!< Byte ring (capacity = queue_size) */
    size_t cap;            /*!< Ring capacity in bytes */
    size_t head;           /*!< Worker read offset (monotonic bytes) */
    size_t tail;           /*!< Producer write offset (monotonic bytes) */
    int stop;              /*!< Worker stop request (drain first) */
    int exited;            /*!< Worker finished draining and observed stop */
    hpu_thread_t worker;   /*!< Worker handle */
    int worker_started;    /*!< Worker thread created successfully */
    struct sq_worker_ctx* ctx; /*!< Worker context (freed with the queue) */
};

/**
 * @brief Total packed size of an event.
 */
static size_t sq_packed_size(const hpulogc_event_t* ev)
{
    return SQ_HDR + ev->line_len + ev->msg_len + ev->category_len +
           ev->file_len + ev->func_len + ev->fields_len;
}

int hpu_sink_queue_push(hpu_sink_queue_t* q, const hpulogc_event_t* ev)
{
    size_t need;
    size_t head;
    size_t contig;
    uint8_t pad_hdr[4] = { 0, 0, 0, 0 };
    uint16_t u16;
    uint32_t u32;
    size_t pos;

    if (q == NULL || ev == NULL) {
        return -1;
    }
    need = sq_packed_size(ev);

    hpu_mutex_lock(&q->mu);
    if (q->stop) {
        hpu_mutex_unlock(&q->mu);
        return -1; /* shutting down: stop accepting */
    }
    /* Padding records (4 bytes of zeros) bridge wrap boundaries; a record
     * larger than the whole queue is undeliverable (discard, D-S5). */
    head = q->head;
    contig = q->cap - (q->tail % q->cap);
    if (contig < need + 4U) {
        if (q->cap - (q->tail - head) < contig) {
            goto full; /* not even the pad fits */
        }
        /* tail pad to the boundary */
        if (contig >= 4U) {
            memcpy(q->buf + q->tail % q->cap, pad_hdr, 4);
            q->tail += 4U;
            contig -= 4U;
        }
    }
    if (q->cap - (q->tail - head) < need) {
        goto full; /* full: discard the new event (D-S5) */
    }
    if (q->tail % q->cap + need > q->cap) {
        /* wrap mid-record: drop instead of splitting (simple + safe) */
        goto full;
    }

    pos = q->tail % q->cap;
    u32 = (uint32_t)need;
    memcpy(q->buf + pos, &u32, 4);
    q->buf[pos + 4] = (uint8_t)ev->level;
    memset(q->buf + pos + 5, 0, 3);
    u16 = (uint16_t)ev->line_len;
    memcpy(q->buf + pos + 8, &u16, 2);
    u16 = (uint16_t)ev->msg_len;
    memcpy(q->buf + pos + 10, &u16, 2);
    u16 = (uint16_t)ev->category_len;
    memcpy(q->buf + pos + 12, &u16, 2);
    u16 = (uint16_t)ev->file_len;
    memcpy(q->buf + pos + 14, &u16, 2);
    u16 = (uint16_t)ev->func_len;
    memcpy(q->buf + pos + 16, &u16, 2);
    u16 = (uint16_t)ev->field_count;
    memcpy(q->buf + pos + 18, &u16, 2);
    u16 = (uint16_t)ev->fields_len;
    memcpy(q->buf + pos + 20, &u16, 2);
    u16 = (uint16_t)(ev->src_line < 0 ? 0 : ev->src_line);
    memcpy(q->buf + pos + 22, &u16, 2);
    memset(q->buf + pos + 24, 0, 8);
    memcpy(q->buf + pos + 32, &ev->realtime_ns, 8);
    memcpy(q->buf + pos + 40, &ev->mono_us, 8);
    memcpy(q->buf + pos + 48, &ev->tid, 8);
    {
        uint8_t* p = q->buf + pos + SQ_HDR;

        memcpy(p, ev->line, ev->line_len);
        p += ev->line_len;
        memcpy(p, ev->msg, ev->msg_len);
        p += ev->msg_len;
        if (ev->category_len > 0) {
            memcpy(p, ev->category, ev->category_len);
            p += ev->category_len;
        }
        if (ev->file_len > 0) {
            memcpy(p, ev->file, ev->file_len);
            p += ev->file_len;
        }
        if (ev->func_len > 0) {
            memcpy(p, ev->func, ev->func_len);
            p += ev->func_len;
        }
        if (ev->fields_len > 0) {
            memcpy(p, ev->fields_wire, ev->fields_len);
        }
    }
    q->tail += need;
    hpu_cond_signal(&q->cv);
    hpu_mutex_unlock(&q->mu);
    return 0;

full:
    hpu_mutex_unlock(&q->mu);
    return 1; /* queue full: caller counts the drop */
}

/**
 * @brief Worker context: decodes queued records into linear scratch.
 */
typedef struct sq_worker_ctx {
    hpu_sink_queue_t* q;
    hpulogc_sink_t* sink;
    const hpulogc_sink_ops_t* ops;
    uint8_t* sweep;        /*!< Linear drain area (queue capacity) */
    const hpulogc_event_t** batch; /*!< Decoded event pointers */
    hpulogc_event_t* evs;  /*!< Decoded event storage */
    hpulogc_field_t* flds; /*!< Per-event field storage (batch_max rows) */
    size_t batch_max;      /*!< Event capacity of the arrays */
    size_t flush_interval; /*!< Worker flush cadence in ms */
    uint64_t last_flush_ms;
} sq_worker_ctx_t;

/**
 * @brief Decode one packed record at @p off into @p ev (pointers into the
 *        linear sweep area).
 * @return Record total length, or 0 when the record is incomplete/corrupt.
 */
static size_t sq_decode(const sq_worker_ctx_t* ctx, size_t off,
                        hpulogc_event_t* ev, hpulogc_field_t* fld_out)
{
    hpu_sink_queue_t* q = ctx->q;
    const uint8_t* base = q->buf;
    const uint8_t* rd;
    uint32_t rec_len;
    uint16_t line_len, body_len, cat_len, file_len, func_len;
    uint16_t field_count, fields_len, src_line;

    if (off + SQ_HDR > q->cap) {
        return 0;
    }
    memcpy(&rec_len, base + off, 4);
    if (rec_len < SQ_HDR || off + rec_len > q->cap) {
        return 0;
    }
    rd = base + off + 4;
    ev->level = *(const int8_t*)(const void*)rd;
    rd += 4;
    memcpy(&line_len, rd, 2); rd += 2;
    memcpy(&body_len, rd, 2); rd += 2;
    memcpy(&cat_len, rd, 2);  rd += 2;
    memcpy(&file_len, rd, 2); rd += 2;
    memcpy(&func_len, rd, 2); rd += 2;
    memcpy(&field_count, rd, 2); rd += 2;
    memcpy(&fields_len, rd, 2); rd += 2;
    memcpy(&src_line, rd, 2); rd += 2;
    rd += 8; /* pad */
    memcpy(&ev->realtime_ns, rd, 8); rd += 8;
    memcpy(&ev->mono_us, rd, 8); rd += 8;
    memcpy(&ev->tid, rd, 8); rd += 8;

    ev->line = (const char*)rd; rd += line_len;
    ev->line_len = line_len;
    ev->msg = (const char*)rd; rd += body_len;
    ev->msg_len = body_len;
    ev->category = cat_len > 0 ? (const char*)rd : NULL;
    rd += cat_len;
    ev->category_len = cat_len;
    ev->file = file_len > 0 ? (const char*)rd : NULL;
    rd += file_len;
    ev->file_len = file_len;
    ev->func = func_len > 0 ? (const char*)rd : NULL;
    rd += func_len;
    ev->func_len = func_len;
    ev->src_line = src_line;
    ev->fields_wire = fields_len > 0 ? rd : NULL;
    ev->fields_len = fields_len;
    ev->field_count = field_count;
    ev->field_count =
        hpu_fields_unpack(ev->fields_wire, fields_len, field_count, fld_out,
                          HPULOGC_MAX_FIELDS);
    ev->fields = ev->field_count > 0 ? fld_out : NULL;
    (void)ctx;
    return rec_len;
}

/**
 * @brief Worker main loop: sweep available records, emit in batches,
 *        flush periodically.
 */
static void sq_worker_main(void* arg)
{
    sq_worker_ctx_t* ctx = arg;
    hpu_sink_queue_t* q = ctx->q;

    for (;;) {
        size_t avail;
        size_t off;
        size_t n = 0;
        uint64_t now;

        hpu_mutex_lock(&q->mu);
        while (!q->stop && q->tail == q->head) {
            (void)hpu_cond_timedwait_ms(&q->cv, &q->mu, 20);
        }
        avail = q->tail - q->head;
        if (avail > q->cap) {
            avail = q->cap; /* defensive */
        }
        /* The swept span can wrap the ring (producers advance while this
         * worker is blocked in a slow emit_batch): copy in two parts at
         * the ring boundary (defect P-7: a single memcpy read past the
         * end of q->buf). Records themselves never straddle the wrap
         * (push pads/drops), so the linearized copy preserves the record
         * stream, including the 4-byte wrap-bridge pads. */
        if (avail > 0) {
            size_t start = q->head % q->cap;

            if (start + avail > q->cap) {
                size_t first = q->cap - start;

                memcpy(ctx->sweep, q->buf + start, first);
                memcpy(ctx->sweep + first, q->buf, avail - first);
            } else {
                memcpy(ctx->sweep, q->buf + start, avail);
            }
        }
        q->head += avail;
        if (q->stop && q->tail == q->head) {
            q->exited = 1;
            hpu_mutex_unlock(&q->mu);
            break;
        }
        hpu_mutex_unlock(&q->mu);

        /* Decode the swept records and emit them batch_size at a time. */
        off = 0;
        while (off + SQ_HDR <= avail) {
            size_t rec_len;

            n = 0;
            while (n < ctx->batch_max && off + SQ_HDR <= avail) {
                uint32_t rec_raw;

                /* A zero rec_len is a wrap-bridge pad written by the
                 * producer: skip it (defect P-7: breaking here used to
                 * silently discard every already-swept record after the
                 * bridge). */
                memcpy(&rec_raw, ctx->sweep + off, 4);
                if (rec_raw == 0) {
                    off += 4;
                    continue;
                }
                rec_len = sq_decode(ctx, off, &ctx->evs[n],
                                    &ctx->flds[n * HPULOGC_MAX_FIELDS]);
                if (rec_len == 0) {
                    break; /* incomplete/corrupt tail: stop this sweep */
                }
                ctx->batch[n] = &ctx->evs[n];
                n++;
                off += rec_len;
            }
            if (n == 0) {
                break;
            }
            if (ctx->ops->emit_batch != NULL) {
                int done = ctx->ops->emit_batch(ctx->sink, ctx->batch, n);

                if (done < 0) {
                    hpu_sink_account_failed(ctx->sink, (unsigned long long)n);
                } else if ((size_t)done < n) {
                    hpu_sink_account_failed(ctx->sink,
                                            (unsigned long long)(n -
                                                                 (size_t)done));
                }
            } else if (ctx->ops->emit != NULL) {
                size_t k;

                for (k = 0; k < n; k++) {
                    ctx->ops->emit(ctx->sink, ctx->batch[k]);
                }
            }
        }
        now = (uint64_t)(hpu_now_ns() / 1000000ULL);
        if (now - ctx->last_flush_ms >= ctx->flush_interval) {
            (void)hpu_output_flush((hpu_output_t*)(void*)ctx->sink);
            ctx->last_flush_ms = now;
        }
    }
}

int hpu_sink_queue_create(hpu_sink_queue_t** out, hpulogc_sink_t* sink,
                          const hpulogc_sink_ops_t* ops, size_t capacity,
                          uint32_t flush_interval_ms, size_t batch_max)
{
    hpu_sink_queue_t* q;
    sq_worker_ctx_t* ctx;

    q = calloc(1, sizeof(*q));
    if (q == NULL) {
        return -1;
    }
    q->cap = capacity;
    q->buf = malloc(capacity);
    if (q->buf == NULL) {
        free(q);
        return -1;
    }
    if (hpu_mutex_init(&q->mu) != 0 || hpu_cond_init(&q->cv) != 0) {
        free(q->buf);
        free(q);
        return -1;
    }
    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        free(q->buf);
        free(q);
        return -1;
    }
    ctx->q = q;
    ctx->sink = sink;
    ctx->ops = ops;
    ctx->flush_interval = flush_interval_ms > 0 ? flush_interval_ms : 100;
    ctx->last_flush_ms = (uint64_t)(hpu_now_ns() / 1000000ULL);
    ctx->batch_max = batch_max > 0 ? batch_max : capacity / SQ_HDR + 1;
    ctx->sweep = malloc(capacity);
    ctx->batch = calloc(ctx->batch_max, sizeof(*ctx->batch));
    ctx->evs = calloc(ctx->batch_max, sizeof(*ctx->evs));
    ctx->flds = calloc(ctx->batch_max * HPULOGC_MAX_FIELDS,
                       sizeof(*ctx->flds));
    if (ctx->sweep == NULL || ctx->batch == NULL || ctx->evs == NULL ||
        ctx->flds == NULL) {
        free(ctx->sweep);
        free(ctx->batch);
        free(ctx->evs);
        free(ctx->flds);
        free(ctx);
        free(q->buf);
        free(q);
        return -1;
    }
    if (hpu_thread_create(&q->worker, sq_worker_main, ctx) != 0) {
        free(ctx->sweep);
        free(ctx->batch);
        free(ctx->evs);
        free(ctx);
        free(q->buf);
        free(q);
        return -1;
    }
    q->worker_started = 1;
    q->ctx = ctx; /* kept for the shutdown free */
    *out = q;
    return 0;
}

void hpu_sink_queue_flush_wait(hpu_sink_queue_t* q, uint32_t timeout_ms)
{
    uint64_t deadline;

    if (q == NULL) {
        return;
    }
    deadline = hpu_now_ns() / 1000000ULL +
               (timeout_ms == 0 ? ~0ULL : (uint64_t)timeout_ms);
    for (;;) {
        size_t used;

        hpu_mutex_lock(&q->mu);
        used = q->tail - q->head;
        hpu_mutex_unlock(&q->mu);
        if (used == 0) {
            return; /* drained; the worker flushed at its cadence */
        }
        if (hpu_now_ns() / 1000000ULL >= deadline) {
            return; /* best effort */
        }
        hpu_core_sleep_ms(1);
    }
}

void hpu_sink_queue_destroy(hpu_sink_queue_t* q)
{
    if (q == NULL) {
        return;
    }
    hpu_mutex_lock(&q->mu);
    q->stop = 1;
    hpu_cond_broadcast(&q->cv);
    hpu_mutex_unlock(&q->mu);
    if (q->worker_started) {
        hpu_thread_join(&q->worker);
    }
    hpu_cond_destroy(&q->cv);
    hpu_mutex_destroy(&q->mu);
    free(q->buf);
    free(q->ctx->sweep);
    free(q->ctx->batch);
    free(q->ctx->evs);
    free(q->ctx->flds);
    free(q->ctx);
    free(q);
}
