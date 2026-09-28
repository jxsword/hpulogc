/**
 * @file sink_queue.h
 * @brief Second-level delivery queue for async sinks (rd_v0.6 §4.10.6):
 *        bounded byte-ring, non-blocking discard producers (decision
 *        D-S5), one worker thread per instance emitting in batches.
 */

#ifndef HPU_SINK_QUEUE_H
#define HPU_SINK_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#include "hpulogc.h"
#include "output.h"

/** @brief Opaque second-level queue state (hpu_output_base_t::queue). */
typedef struct hpu_sink_queue hpu_sink_queue_t;

/**
 * @brief Enqueue one event (non-blocking; full queue discards, D-S5).
 * @return 0 enqueued, 1 discarded (full/shutting down), -1 bad args.
 */
int hpu_sink_queue_push(hpu_sink_queue_t* q, const hpulogc_event_t* ev);

/**
 * @brief Wait until the queue is drained (bounded, best effort).
 * @param timeout_ms  Wait bound in ms (0 = wait forever).
 */
void hpu_sink_queue_flush_wait(hpu_sink_queue_t* q, uint32_t timeout_ms);

/**
 * @brief Create the queue and start the worker thread.
 *
 * @param out                Filled with the queue handle.
 * @param sink               Owning sink handle (callbacks target).
 * @param ops                Type ops table.
 * @param capacity           Ring capacity in bytes (queue size key).
 * @param flush_interval_ms  Worker flush cadence in ms.
 * @param batch_max          Worker emit_batch bound (records per call).
 * @return                   0 on success, -1 on failure (resources freed).
 */
int hpu_sink_queue_create(hpu_sink_queue_t** out, hpulogc_sink_t* sink,
                          const hpulogc_sink_ops_t* ops, size_t capacity,
                          uint32_t flush_interval_ms, size_t batch_max);

/**
 * @brief Stop the worker (drain-then-exit), join it and free everything.
 */
void hpu_sink_queue_destroy(hpu_sink_queue_t* q);

#endif /* HPU_SINK_QUEUE_H */
