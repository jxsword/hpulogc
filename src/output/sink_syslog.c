/**
 * @file sink_syslog.c
 * @brief syslog sink (POSIX only): openlog/syslog passthrough with a
 *        level-to-priority mapping (rd_v0.6 §4.7.1).
 *
 * openlog state is process-global, so the type enforces a single instance
 * per process (a second instance definition fails init, §10.4). The ident
 * is the sink instance name; LOG_PID is always included.
 */

#include "output.h"

#if !defined(_WIN32)

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "../atomic/hpulogc_atomic.h"

/**
 * @brief syslog sink private data (allocated by the core, zeroed).
 */
typedef struct hpu_syslog_priv {
    int facility;  /*!< LOG_* facility value */
    int opened;    /*!< openlog issued (closelog at destroy) */
} hpu_syslog_priv_t;

/** @brief Single-instance guard (process-global openlog state). */
static hpu_atomic_u32 g_syslog_instances;

/**
 * @brief Map a facility name to its LOG_* value.
 * @return Facility value, or -1 when unknown.
 */
static int facility_from_name(const char* val)
{
    static const struct {
        const char* name;
        int value;
    } table[] = {
        { "user", LOG_USER },     { "daemon", LOG_DAEMON },
        { "auth", LOG_AUTH },     { "cron", LOG_CRON },
        { "local0", LOG_LOCAL0 }, { "local1", LOG_LOCAL1 },
        { "local2", LOG_LOCAL2 }, { "local3", LOG_LOCAL3 },
        { "local4", LOG_LOCAL4 }, { "local5", LOG_LOCAL5 },
        { "local6", LOG_LOCAL6 }, { "local7", LOG_LOCAL7 },
    };
    size_t i;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(val, table[i].name) == 0) {
            return table[i].value;
        }
    }
    return -1;
}

static int syslog_configure(hpulogc_sink_t* sink, const char* key,
                            const char* val)
{
    hpu_syslog_priv_t* p = hpulogc_sink_priv(sink);

    if (strcmp(key, "facility") == 0) {
        int f;

        if (val == NULL || (f = facility_from_name(val)) < 0) {
            return -1;
        }
        p->facility = f;
        return 0;
    }
    return -1; /* unknown key */
}

static int syslog_start(hpulogc_sink_t* sink)
{
    hpu_syslog_priv_t* p = hpulogc_sink_priv(sink);
    hpu_output_base_t* b = hpu_sink_base(sink);

    /* Single-instance enforcement (§10.4). */
    if (hpu_at_fetch_add_u32(&g_syslog_instances, 1, HPU_MO_ACQ_REL) != 0) {
        fprintf(stderr,
                "hpulogc: syslog sink '%s': only one syslog instance is "
                "allowed per process\n",
                b->name);
        return -1;
    }
    if (b->name[0] != '\0') {
        openlog(b->name, LOG_PID, p->facility);
    } else {
        openlog("hpulogc", LOG_PID, p->facility);
    }
    p->opened = 1;
    return 0;
}

static void syslog_emit(hpulogc_sink_t* sink, const hpulogc_event_t* ev)
{
    static const int pri_by_level[] = {
        LOG_DEBUG, LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERR, LOG_CRIT
    };
    hpu_syslog_priv_t* p = hpulogc_sink_priv(sink);
    int pri;

    if (!p->opened) {
        return;
    }
    pri = (ev->level >= 0 && ev->level <= 5) ? pri_by_level[ev->level]
                                             : LOG_INFO;
    /* The message is data, never a format string. */
    syslog(pri, "%.*s", (int)ev->line_len, ev->line);
}

static void syslog_destroy(hpulogc_sink_t* sink)
{
    hpu_syslog_priv_t* p = hpulogc_sink_priv(sink);

    if (p->opened) {
        closelog();
        p->opened = 0;
    }
    hpu_at_fetch_sub_u32(&g_syslog_instances, 1, HPU_MO_ACQ_REL);
}

/** @brief syslog sink type (SYNC only; POSIX builds, single instance). */
static const hpulogc_sink_ops_t g_syslog_ops = {
    "syslog",
    HPULOGC_SINK_ABI_VERSION,
    HPULOGC_CAP_SYNC | HPULOGC_CAP_LINE_ATOMIC,
    sizeof(hpu_syslog_priv_t),
    syslog_configure,
    NULL, /* init: no defaults beyond the facility */
    syslog_start,
    syslog_emit,
    NULL, /* emit_batch */
    NULL, /* flush: syslog(3) is unbuffered from the caller's view */
    NULL, /* sync */
    NULL, /* periodic */
    syslog_destroy,
    { NULL, NULL, NULL, NULL }
};

const hpulogc_sink_ops_t* hpu_syslog_sink_ops(void)
{
    return &g_syslog_ops;
}

#endif /* !defined(_WIN32) */
