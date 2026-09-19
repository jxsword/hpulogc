/**
 * @file test_ring.c
 * @brief Unit tests for the ring buffer (both implementations, SPSC/MPSC).
 *
 * Compiled four times with:
 *   HPU_RING_TEST_IMPL  0 = locked, 1 = lockfree
 *   HPU_RING_TEST_CONC  0 = SPSC,  1 = MPSC
 */

#include "test_util.h"
#include "atomic/hpulogc_atomic.h"
#include "ring/ringbuf.h"

#include <pthread.h>
#include <unistd.h>

#ifndef HPU_RING_TEST_IMPL
#define HPU_RING_TEST_IMPL 0
#endif
#ifndef HPU_RING_TEST_CONC
#define HPU_RING_TEST_CONC 0
#endif

/* The lock-free overwrite policy deliberately tears slow consumers via a
 * seqlock-style marker protocol (see docs/implementation_notes.md);
 * ThreadSanitizer cannot model that, so those cases are skipped under
 * TSan. The discard policy is fully TSan-clean. */
#if defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && __has_feature(thread_sanitizer))
#define HPU_RING_SKIP_OVERWRITE 1
#else
#define HPU_RING_SKIP_OVERWRITE 0
#endif

#define RING_TEST_CAPACITY (64U * 1024U)
#define STAGING_SIZE (8U * 1024U)

/* spsc flag for hpu_ring_create(), derived from the build combination. */
#if HPU_RING_TEST_CONC == 0
#define RING_TEST_SPSC 1
#else
#define RING_TEST_SPSC 0
#endif

/** @brief Overflow policy codes (mirrors hpulogc_overflow_policy_t). */
enum { POL_DISCARD = 0, POL_OVERWRITE = 1, POL_WAIT = 2 };

/**
 * @brief Build a record whose message encodes (producer, seq).
 */
static void make_msg(hpu_ring_msg_t* msg, uint8_t* msgbuf,
                     unsigned producer, unsigned long seq)
{
    int j;

    memset(msg, 0, sizeof(*msg));
    /* fixed-width encoding "Pdd Sdddddddddx" = 15 bytes; the trailing
     * non-digit terminator keeps sscanf from consuming stack garbage
     * beyond the encoded digits. */
    msgbuf[0] = 'P';
    msgbuf[1] = (uint8_t)('0' + (producer / 10) % 10);
    msgbuf[2] = (uint8_t)('0' + producer % 10);
    msgbuf[3] = ' ';
    msgbuf[4] = 'S';
    for (j = 0; j < 9; j++) {
        unsigned long div = 1;
        int t;

        for (t = 0; t < 8 - j; t++) {
            div *= 10;
        }
        msgbuf[5 + j] = (uint8_t)('0' + (seq / div) % 10);
    }
    msgbuf[14] = 'x';
    msg->level       = 2; /* INFO */
    msg->tid         = producer;
    msg->realtime_ns = (int64_t)seq;
    msg->mono_us     = (int64_t)seq;
    msg->line        = (uint32_t)seq;
    msg->category    = "cat";
    msg->category_len = 3;
    msg->msg         = (const char*)msgbuf;
    msg->msg_len     = 15;
}

/**
 * @brief Parse (producer, seq) back out of a retrieved record.
 */
static void parse_view(const hpu_ring_view_t* v, unsigned* producer,
                       unsigned long* seq)
{
    unsigned p = 0;
    unsigned long s = 0;
    char tmp[16];

    CHECK_EQ(v->meta.msg_len, 15);
    memcpy(tmp, v->msg, 15);
    tmp[15] = '\0';
    CHECK_EQ(sscanf(tmp, "P%u S%lu", &p, &s), 2);
    *producer = p;
    *seq = s;
}

/* ------------------------------------------------------------------ */

TEST(ring_roundtrip_single_thread)
{
    hpu_ring_t* r = hpu_ring_create(RING_TEST_CAPACITY, POL_DISCARD, 0);
    hpu_ring_msg_t msg;
    uint8_t buf[64];
    hpu_ring_view_t v;
    char staging[STAGING_SIZE];
    unsigned i;

    CHECK(r != NULL);
    CHECK_EQ(hpu_ring_capacity(r), RING_TEST_CAPACITY);

    for (i = 0; i < 500; i++) {
        make_msg(&msg, buf, 1, i);
        CHECK_EQ(hpu_ring_put(r, &msg), HPU_RING_OK);
    }

    for (i = 0; i < 500; i++) {
        CHECK_EQ(hpu_ring_get(r, &v, staging, sizeof(staging), 0),
                 HPU_RING_OK);
        CHECK_EQ(v.meta.msg_len, 15);
        CHECK_EQ(v.meta.tid, 1);
        CHECK(v.category != NULL && v.meta.category_len == 3 &&
              memcmp(v.category, "cat", 3) == 0);
    }
    CHECK_EQ(hpu_ring_get(r, &v, staging, sizeof(staging), 0),
             HPU_RING_EMPTY);
    CHECK_EQ(hpu_ring_used(r), 0);
    hpu_ring_destroy(r);
}

TEST(ring_wrap_and_pad)
{
    /* Small ring: forces boundary pads and repeated wrap-around. */
    hpu_ring_t* r = hpu_ring_create(512, POL_DISCARD, 0);
    hpu_ring_msg_t msg;
    uint8_t buf[64];
    hpu_ring_view_t v;
    char staging[STAGING_SIZE];
    unsigned i;

    CHECK(r != NULL);
    for (i = 0; i < 500; i++) {
        make_msg(&msg, buf, 1, i);
        CHECK_EQ(hpu_ring_put(r, &msg), HPU_RING_OK);
        CHECK_EQ(hpu_ring_get(r, &v, staging, sizeof(staging), 100),
                 HPU_RING_OK);
        {
            unsigned p;
            unsigned long s;
            parse_view(&v, &p, &s);
            CHECK_EQ(p, 1);
            CHECK_EQ((int)s, (int)i); /* strict order, no loss/dup */
        }
    }
    CHECK_EQ(hpu_ring_get(r, &v, staging, sizeof(staging), 0),
             HPU_RING_EMPTY);
    hpu_ring_destroy(r);
}

TEST(ring_discard_full)
{
    hpu_ring_t* r = hpu_ring_create(1024, POL_DISCARD, 0);
    hpu_ring_msg_t msg;
    uint8_t buf[64];
    hpu_ring_view_t v;
    char staging[STAGING_SIZE];
    unsigned long long dropped = 999, overwritten = 999;
    int ok = 0, dropped_ret = 0;
    unsigned i;

    CHECK(r != NULL);
    for (i = 0; i < 200; i++) {
        make_msg(&msg, buf, 1, i);
        if (hpu_ring_put(r, &msg) == HPU_RING_OK) {
            ok++;
        } else {
            dropped_ret++;
        }
    }
    CHECK(ok > 0);
    CHECK(dropped_ret > 0);
    CHECK_EQ(ok + dropped_ret, 200);

    hpu_ring_counters(r, &dropped, &overwritten);
    CHECK_EQ((int)dropped, dropped_ret);
    CHECK_EQ((int)overwritten, 0);

    /* Draining yields exactly the accepted records in order. */
    for (i = 0; i < (unsigned)ok; i++) {
        CHECK_EQ(hpu_ring_get(r, &v, staging, sizeof(staging), 0),
                 HPU_RING_OK);
        {
            unsigned p;
            unsigned long s;
            parse_view(&v, &p, &s);
            CHECK_EQ((int)s, (int)i); /* prefix preserved */
        }
    }
    CHECK_EQ(hpu_ring_get(r, &v, staging, sizeof(staging), 0),
             HPU_RING_EMPTY);
    hpu_ring_destroy(r);
}

#if !(HPU_RING_TEST_IMPL == 1 && HPU_RING_TEST_CONC == 1) && \
    !HPU_RING_SKIP_OVERWRITE
TEST(ring_overwrite_full)
{
    hpu_ring_t* r = hpu_ring_create(1024, POL_OVERWRITE, RING_TEST_SPSC);
    hpu_ring_msg_t msg;
    uint8_t buf[64];
    hpu_ring_view_t v;
    char staging[STAGING_SIZE];
    unsigned long long dropped = 0, overwritten = 0;
    unsigned i, delivered = 0;
    unsigned first_seq = 0;

    CHECK(r != NULL);
    for (i = 0; i < 200; i++) {
        make_msg(&msg, buf, 1, i);
        CHECK_EQ(hpu_ring_put(r, &msg), HPU_RING_OK);
    }
    while (hpu_ring_get(r, &v, staging, sizeof(staging), 0) == HPU_RING_OK) {
        unsigned p;
        unsigned long s;
        if (delivered == 0) {
            parse_view(&v, &p, &s);
            first_seq = (unsigned)s;
        } else {
            parse_view(&v, &p, &s);
            CHECK_EQ((int)s, (int)(first_seq + delivered)); /* contiguous */
        }
        delivered++;
    }

    hpu_ring_counters(r, &dropped, &overwritten);
    CHECK_EQ((int)dropped, 0);
    /* Every record is either delivered or counted as overwritten. */
    CHECK_EQ((int)(delivered + overwritten), 200);
    /* The oldest records were the ones overwritten. */
    CHECK(first_seq > 0 || overwritten == 0);
    hpu_ring_destroy(r);
}
#endif

#if HPU_RING_TEST_IMPL == 0
/** @brief Producer context for the wait-policy test. */
typedef struct wait_ctx {
    hpu_ring_t* ring;   /*!< Ring under test */
    int produced;       /*!< Successful puts */
} wait_ctx_t;

static void* wait_producer_thread(void* raw)
{
    wait_ctx_t* c = raw;
    int i;

    for (i = 0; i < 500; i++) {
        hpu_ring_msg_t msg;
        uint8_t buf[64];

        make_msg(&msg, buf, 1, (unsigned long)i);
        if (hpu_ring_put(c->ring, &msg) == HPU_RING_OK) {
            c->produced++;
        }
    }
    return NULL;
}

TEST(ring_wait_policy)
{
    /* Producer blocks (wait policy) on a tiny ring while this thread
     * drains concurrently: nothing may be lost. */
    hpu_ring_t* r = hpu_ring_create(512, POL_WAIT, 0);
    wait_ctx_t wctx;
    hpu_ring_view_t v;
    char staging[STAGING_SIZE];
    pthread_t producer;
    unsigned delivered = 0;

    CHECK(r != NULL);
    memset(&wctx, 0, sizeof(wctx));
    wctx.ring = r;
    CHECK_EQ(pthread_create(&producer, NULL, wait_producer_thread, &wctx), 0);

    while (delivered < 500) {
        if (hpu_ring_get(r, &v, staging, sizeof(staging), 100) ==
            HPU_RING_OK) {
            delivered++;
        }
    }
    pthread_join(producer, NULL);
    CHECK_EQ(wctx.produced, 500);
    CHECK_EQ(delivered, 500);
    hpu_ring_destroy(r);
}
#endif

TEST(ring_close_then_put_drops)
{
    hpu_ring_t* r = hpu_ring_create(1024, POL_DISCARD, 0);
    hpu_ring_msg_t msg;
    uint8_t buf[64];
    hpu_ring_view_t v;
    char staging[STAGING_SIZE];
    unsigned long long dropped = 0;

    CHECK(r != NULL);
    make_msg(&msg, buf, 1, 1);
    CHECK_EQ(hpu_ring_put(r, &msg), HPU_RING_OK);
    hpu_ring_close(r);
    CHECK_EQ(hpu_ring_put(r, &msg), HPU_RING_DROPPED);
    /* record written before close is still drained */
    CHECK_EQ(hpu_ring_get(r, &v, staging, sizeof(staging), 0), HPU_RING_OK);
    CHECK_EQ(hpu_ring_get(r, &v, staging, sizeof(staging), 0),
             HPU_RING_EMPTY);
    hpu_ring_counters(r, &dropped, NULL);
    CHECK_EQ((int)dropped, 1);
    hpu_ring_destroy(r);
}

/* ------------------------------------------------------------------ */

#define STRESS_RECORDS 2000U
#define STRESS_PRODUCERS 4U

/** @brief Stress consumer context. */
typedef struct stress_ctx {
    hpu_ring_t* ring;                     /*!< Ring under test */
    unsigned long next[STRESS_PRODUCERS]; /*!< Expected next seq/producer */
    unsigned long count[STRESS_PRODUCERS];/*!< Received per producer */
    unsigned long total;                  /*!< Total received */
    unsigned long long dropped;           /*!< discard drops reported */
    unsigned long long overwritten;       /*!< overwritten reported */
    int strict;                           /*!< Expect gap-free sequences */
} stress_ctx_t;

__attribute__((unused)) static void stress_producer(hpu_ring_t* r, unsigned id, unsigned count,
                            unsigned long long* dropped_out)
{
    unsigned i;
    unsigned long long drops = 0;

    for (i = 0; i < count; i++) {
        hpu_ring_msg_t msg;
        uint8_t buf[64];

        make_msg(&msg, buf, id, i);
        if (hpu_ring_put(r, &msg) != HPU_RING_OK) {
            drops++;
        }
    }
    *dropped_out = drops;
}

/**
 * @brief Verify one consumed record preserves per-producer ordering.
 */
unsigned long g_skip_len;   /* records skipped: bad msg_len */
unsigned long g_skip_sscan; /* records skipped: bad sscanf */
unsigned long g_skip_other;

static void stress_verify(stress_ctx_t* ctx, const hpu_ring_view_t* v)
{
    unsigned p = 0;
    unsigned long s = 0;

    if (v->meta.msg_len != 15) {
        g_skip_len++;
        return;
    }
    {
        char tmp[16];

        memcpy(tmp, v->msg, 15);
        tmp[15] = '\0';
        if (sscanf(tmp, "P%u S%lu", &p, &s) != 2) {
            g_skip_sscan++;
            return;
        }
    }
    parse_view(v, &p, &s);
    CHECK(p < STRESS_PRODUCERS);
    if (ctx->strict) {
        /* No drops possible: sequences must be gap-free and in order. */
        CHECK_EQ(s, ctx->next[p]);
    } else {
        /* Discard pressure: per-producer order must hold and records
         * must be unique, but gaps are legitimate drops. */
        CHECK(s >= ctx->next[p]);
    }
    if (ctx->strict || s >= ctx->next[p]) {
        ctx->next[p] = s + 1;
    }
    ctx->count[p]++;
    ctx->total++;
}

#if HPU_RING_TEST_CONC == 0
TEST(ring_stress_spsc_no_loss)
{
    hpu_ring_t* r = hpu_ring_create(RING_TEST_CAPACITY, POL_DISCARD, 1);
    hpu_ring_msg_t msg;
    uint8_t buf[64];
    hpu_ring_view_t v;
    char staging[STAGING_SIZE];
    stress_ctx_t ctx;
    unsigned i;

    CHECK(r != NULL);
    memset(&ctx, 0, sizeof(ctx));
    ctx.ring = r;
    ctx.strict = 1;

    for (i = 0; i < 500; i++) {
        make_msg(&msg, buf, 0, i);
        CHECK_EQ(hpu_ring_put(r, &msg), HPU_RING_OK);
    }
    hpu_ring_close(r);
    while (hpu_ring_get(r, &v, staging, sizeof(staging), 100) ==
           HPU_RING_OK) {
        stress_verify(&ctx, &v);
    }
    CHECK_EQ(ctx.total, 500);
    hpu_ring_destroy(r);
}
#endif

#if HPU_RING_TEST_CONC == 1
/**
 * @brief MPSC producer thread trampoline.
 */
typedef struct mpsc_arg {
    hpu_ring_t* ring;              /*!< Target ring */
    unsigned id;                   /*!< Producer id */
    unsigned long long dropped;    /*!< Put-level drops */
    hpu_atomic_u32 finished;       /*!< Set right before thread exit */
} mpsc_arg_t;

static void* mpsc_producer_thread(void* raw)
{
    mpsc_arg_t* a = raw;

    stress_producer(a->ring, a->id, STRESS_RECORDS, &a->dropped);
    hpu_at_store_u32(&a->finished, 1, HPU_MO_RELEASE);
    return NULL;
}

TEST(ring_stress_mpsc_no_loss)
{
    /* 4 MB ring and 4x500 records: drops are impossible, so the strict
     * gap-free verification is valid. */
    hpu_ring_t* r = hpu_ring_create(4U * 1024U * 1024U, POL_DISCARD, 0);
    hpu_ring_view_t v;
    char staging[STAGING_SIZE];
    stress_ctx_t ctx;
    mpsc_arg_t args[STRESS_PRODUCERS];
    pthread_t threads[STRESS_PRODUCERS];
    unsigned i;
    int all_delivered;

    CHECK(r != NULL);
    memset(&ctx, 0, sizeof(ctx));
    ctx.ring = r;
    ctx.strict = 1;

    for (i = 0; i < STRESS_PRODUCERS; i++) {
        args[i].ring = r;
        args[i].id = i;
        args[i].dropped = 0;
        CHECK_EQ(pthread_create(&threads[i], NULL, mpsc_producer_thread,
                                &args[i]), 0);
    }

    /* Consume while producing; exit once every producer finished and the
     * ring is drained (any shortfall shows up as a drop-count failure). */
    for (;;) {
        int finished = 1;

        for (i = 0; i < STRESS_PRODUCERS; i++) {
            if (hpu_at_load_u32(&args[i].finished, HPU_MO_ACQUIRE) == 0) {
                finished = 0;
            }
        }
        if (hpu_ring_get(r, &v, staging, sizeof(staging), 20) ==
            HPU_RING_OK) {
            stress_verify(&ctx, &v);
            continue;
        }
        if (finished) {
            break;
        }
    }

    for (i = 0; i < STRESS_PRODUCERS; i++) {
        pthread_join(threads[i], NULL);
        CHECK_EQ(args[i].dropped, 0);
    }
    hpu_ring_close(r);
    while (hpu_ring_get(r, &v, staging, sizeof(staging), 50) ==
           HPU_RING_OK) {
        stress_verify(&ctx, &v);
    }
    (void)all_delivered;
    fprintf(stderr, "stress: counts=%lu/%lu/%lu/%lu skip_len=%lu "
            "skip_sscan=%lu total=%lu\n",
            ctx.count[0], ctx.count[1], ctx.count[2], ctx.count[3],
            g_skip_len, g_skip_sscan, ctx.total);
    for (i = 0; i < STRESS_PRODUCERS; i++) {
        CHECK_EQ(ctx.count[i], STRESS_RECORDS);
    }
    CHECK_EQ(ctx.total, STRESS_RECORDS * STRESS_PRODUCERS);
    hpu_ring_destroy(r);
}
#endif

#if HPU_RING_TEST_IMPL == 1 && HPU_RING_TEST_CONC == 1
TEST(ring_lockfree_mpsc_overwrite_maps_to_discard)
{
    /* Overwrite is rejected for lockfree MPSC at init by the config layer;
     * the ring itself degrades to discard so no memory is corrupted. */
    hpu_ring_t* r = hpu_ring_create(1024, POL_OVERWRITE, 0);
    hpu_ring_msg_t msg;
    uint8_t buf[64];
    unsigned long long dropped = 0, overwritten = 0;

    unsigned i;

    CHECK(r != NULL);
    for (i = 0; i < 20; i++) {
        make_msg(&msg, buf, 1, i);
        (void)hpu_ring_put(r, &msg); /* 20x80B into 1KB: some must drop */
    }
    hpu_ring_counters(r, &dropped, &overwritten);
    CHECK_EQ((int)overwritten, 0);
    CHECK(dropped > 0);
    hpu_ring_destroy(r);
}
#endif

/**
 * @brief Slow-consumer pressure context (overwrite accounting).
 */
typedef struct slow_ctx {
    hpu_ring_t* ring;        /*!< Ring under test */
    unsigned long delivered; /*!< Records consumed */
    hpu_atomic_u32 stop;     /*!< Consumer stop flag */
} slow_ctx_t;

static void* slow_consumer_thread(void* raw)
{
    slow_ctx_t* c = raw;
    hpu_ring_view_t v;
    char staging[STAGING_SIZE];

    while (hpu_at_load_u32(&c->stop, HPU_MO_ACQUIRE) == 0) {
        if (hpu_ring_get(c->ring, &v, staging, sizeof(staging), 20) ==
            HPU_RING_OK) {
            c->delivered++;
        }
    }
    /* drain */
    while (hpu_ring_get(c->ring, &v, staging, sizeof(staging), 50) ==
           HPU_RING_OK) {
        c->delivered++;
    }
    return NULL;
}

#if !(HPU_RING_TEST_IMPL == 1 && HPU_RING_TEST_CONC == 1)
#if !HPU_RING_SKIP_OVERWRITE
TEST(ring_pressure_overwrite_accounting)
{
    hpu_ring_t* r = hpu_ring_create(2048, POL_OVERWRITE, RING_TEST_SPSC);
    slow_ctx_t ctx;
    pthread_t consumer;
    unsigned long long total = 0;
    unsigned long long dropped = 0, overwritten = 0;
    unsigned i;

    CHECK(r != NULL);
    memset(&ctx, 0, sizeof(ctx));
    ctx.ring = r;
    CHECK_EQ(pthread_create(&consumer, NULL, slow_consumer_thread, &ctx), 0);

    for (i = 0; i < 20000; i++) {
        hpu_ring_msg_t msg;
        uint8_t buf[64];

        make_msg(&msg, buf, 1, i);
        CHECK_EQ(hpu_ring_put(r, &msg), HPU_RING_OK);
        total++;
        if (i % 100 == 0) {
            usleep(100); /* let the consumer fall behind sometimes */
        }
    }
    hpu_at_store_u32(&ctx.stop, 1, HPU_MO_RELEASE);
    hpu_ring_close(r);
    pthread_join(consumer, NULL);
    hpu_ring_counters(r, &dropped, &overwritten);
    /* Exact accounting: produced == delivered + overwritten */
    CHECK_EQ(ctx.delivered + overwritten, total);
    CHECK_EQ(dropped, 0);
    hpu_ring_destroy(r);
}
#endif /* !HPU_RING_SKIP_OVERWRITE */
#endif

TEST(ring_pressure_discard_accounting)
{
    hpu_ring_t* r = hpu_ring_create(2048, POL_DISCARD, 0);
    slow_ctx_t ctx;
    pthread_t consumer;
    unsigned long long total = 0;
    unsigned long long producer_dropped = 0;
    unsigned long long dropped = 0, overwritten = 0;
    unsigned i;

    CHECK(r != NULL);
    memset(&ctx, 0, sizeof(ctx));
    ctx.ring = r;
    CHECK_EQ(pthread_create(&consumer, NULL, slow_consumer_thread, &ctx), 0);

    for (i = 0; i < 20000; i++) {
        hpu_ring_msg_t msg;
        uint8_t buf[64];

        make_msg(&msg, buf, 1, i);
        if (hpu_ring_put(r, &msg) != HPU_RING_OK) {
            producer_dropped++;
        }
        total++;
    }
    hpu_at_store_u32(&ctx.stop, 1, HPU_MO_RELEASE);
    hpu_ring_close(r);
    pthread_join(consumer, NULL);
    hpu_ring_counters(r, &dropped, &overwritten);
    CHECK_EQ(ctx.delivered + producer_dropped, total);
    CHECK_EQ((int)overwritten, 0);
    CHECK_EQ((int)dropped, (int)producer_dropped);
    hpu_ring_destroy(r);
}
