/**
 * @file output_console.c
 * @brief Console sink (stdout/stderr, terminal-only ANSI color).
 *
 * Line buffering relies on stdio (full buffering when redirected), so no
 * extra batching buffer is needed here; the consumer flushes per batch.
 */

#include "output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"

#if HPULOGC_ENABLE_COLOR
/** @brief ANSI color codes per level (whole-line coloring). */
static const char* const g_level_colors[] = {
    "\x1b[90m",  /* TRACE: bright black */
    "\x1b[36m",  /* DEBUG: cyan */
    "\x1b[32m",  /* INFO: green */
    "\x1b[33m",  /* WARN: yellow */
    "\x1b[31m",  /* ERROR: red */
    "\x1b[1;31m" /* FATAL: bold red */
};

/** @brief Reset sequence closing any color span. */
static const char* const g_color_reset = "\x1b[0m";
#endif

/**
 * @brief Console sink private data (allocated by the core, zeroed).
 */
typedef struct hpu_console_priv {
    FILE* stream;           /*!< Resolved stream (init) */
    int   fd;               /*!< Descriptor for the isatty check */
    int   want_stderr;      /*!< Configured: stream = stderr */
    int   want_color;       /*!< Configured: color = true */
    int   color;            /*!< Resolved: color codes are emitted */
} hpu_console_priv_t;

static int console_configure(hpulogc_sink_t* sink, const char* key,
                             const char* val)
{
    hpu_console_priv_t* p = hpulogc_sink_priv(sink);

    if (strcmp(key, "stream") == 0) {
        if (val == NULL || strcmp(val, "stdout") == 0) {
            p->want_stderr = 0;
            return 0;
        }
        if (strcmp(val, "stderr") == 0) {
            p->want_stderr = 1;
            return 0;
        }
        return -1;
    }
    if (strcmp(key, "color") == 0) {
        if (val == NULL || strcmp(val, "true") == 0 ||
            strcmp(val, "false") == 0) {
            p->want_color = val != NULL && strcmp(val, "true") == 0;
            return 0;
        }
        return -1;
    }
    return -1; /* unknown key: strict handling by the core */
}

static int console_init(hpulogc_sink_t* sink)
{
    hpu_console_priv_t* p = hpulogc_sink_priv(sink);
    hpu_output_base_t* b = hpu_sink_base(sink);

    p->stream = p->want_stderr ? stderr : stdout;
    p->fd = p->want_stderr ? 2 : 1;
    if (b->name[0] == '\0') {
        snprintf(b->name, sizeof(b->name), "console_%s",
                 p->want_stderr ? "stderr" : "stdout");
    }
    p->color = 0;
#if HPULOGC_ENABLE_COLOR
    if (p->want_color && hpu_fs_isatty(p->fd)) {
        p->color = 1; /* redirected pipes/files stay plain (spec 4.7) */
    }
#endif
    return 0;
}

static void console_emit(hpulogc_sink_t* sink, const hpulogc_event_t* ev)
{
    hpu_console_priv_t* p = hpulogc_sink_priv(sink);

#if HPULOGC_ENABLE_COLOR
    if (p->color && ev->level >= 0 && ev->level <= 5) {
        fwrite(g_level_colors[ev->level], 1,
               strlen(g_level_colors[ev->level]), p->stream);
        fwrite(ev->line, 1, ev->line_len, p->stream);
        fwrite(g_color_reset, 1, strlen(g_color_reset), p->stream);
        return;
    }
#endif
    fwrite(ev->line, 1, ev->line_len, p->stream);
    if (ferror(p->stream)) {
        hpu_sink_account_failed(sink, 1);
    }
}

static int console_flush(hpulogc_sink_t* sink)
{
    hpu_console_priv_t* p = hpulogc_sink_priv(sink);

    return fflush(p->stream) == 0 ? 0 : -1;
}

/** @brief Console sink type (SYNC|ASYNC|LINE_ATOMIC; no fsync semantics). */
static const hpulogc_sink_ops_t g_console_ops = {
    "console",
    HPULOGC_SINK_ABI_VERSION,
    HPULOGC_CAP_SYNC | HPULOGC_CAP_ASYNC | HPULOGC_CAP_LINE_ATOMIC,
    sizeof(hpu_console_priv_t),
    console_configure,
    console_init,
    NULL, /* start: no external resource */
    console_emit,
    NULL, /* emit_batch: core loops emit */
    console_flush,
    NULL, /* sync: no fsync semantics */
    NULL, /* periodic: no fsync bookkeeping */
    NULL, /* destroy: priv lives in the instance allocation */
    { NULL, NULL, NULL, NULL }
};

const hpulogc_sink_ops_t* hpu_console_sink_ops(void)
{
    return &g_console_ops;
}
