/**
 * @file output_console.c
 * @brief Console sink (stdout/stderr, terminal-only ANSI color).
 *
 * The async batch path coalesces whole lines into a preallocated buffer so
 * one batch costs a single fwrite (a single stdio lock round-trip) instead
 * of one per line; the sync path writes per event and relies on stdio
 * buffering. Whole lines are never split across buffer boundaries
 * (HPULOGC_CAP_LINE_ATOMIC).
 */

#include "output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"

/** @brief Coalesce buffer capacity for the batch path (bytes). */
#define HPU_CONSOLE_COALESCE_CAP (64U * 1024U)

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
    char  coalesce[HPU_CONSOLE_COALESCE_CAP]; /*!< Batch merge buffer */
    size_t coalesce_len;    /*!< Bytes pending in the merge buffer */
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

/* Write one event's bytes straight to the stream (rare oversized-line
 * fallback of the batch path); no accounting here — emit_batch reports
 * failure through its return value (§4.10.6). */
static int console_write_direct(const hpu_console_priv_t* p,
                                const hpulogc_event_t* ev)
{
#if HPULOGC_ENABLE_COLOR
    if (p->color && ev->level >= 0 && ev->level <= 5) {
        fwrite(g_level_colors[ev->level], 1,
               strlen(g_level_colors[ev->level]), p->stream);
        fwrite(ev->line, 1, ev->line_len, p->stream);
        fwrite(g_color_reset, 1, strlen(g_color_reset), p->stream);
        return ferror(p->stream) ? -1 : 0;
    }
#else
    (void)p;
#endif
    fwrite(ev->line, 1, ev->line_len, p->stream);
    return ferror(p->stream) ? -1 : 0;
}

/* Drain the merge buffer with one fwrite; clears the pending length
 * regardless of the outcome. */
static int console_coalesce_flush(hpu_console_priv_t* p)
{
    if (p->coalesce_len != 0) {
        fwrite(p->coalesce, 1, p->coalesce_len, p->stream);
        p->coalesce_len = 0;
        if (ferror(p->stream)) {
            return -1;
        }
    }
    return 0;
}

/* Append one event (whole-line color span included) to the merge buffer;
 * the caller guarantees size <= HPU_CONSOLE_COALESCE_CAP - coalesce_len. */
static void console_coalesce_append(hpu_console_priv_t* p,
                                    const hpulogc_event_t* ev)
{
#if HPULOGC_ENABLE_COLOR
    if (p->color && ev->level >= 0 && ev->level <= 5) {
        size_t cs_len = strlen(g_level_colors[ev->level]);

        memcpy(p->coalesce + p->coalesce_len, g_level_colors[ev->level],
               cs_len);
        p->coalesce_len += cs_len;
        memcpy(p->coalesce + p->coalesce_len, ev->line, ev->line_len);
        p->coalesce_len += ev->line_len;
        memcpy(p->coalesce + p->coalesce_len, g_color_reset,
               strlen(g_color_reset));
        p->coalesce_len += strlen(g_color_reset);
        return;
    }
#endif
    memcpy(p->coalesce + p->coalesce_len, ev->line, ev->line_len);
    p->coalesce_len += ev->line_len;
}

/* Size in bytes one event occupies in the merge buffer. */
static size_t console_event_size(const hpu_console_priv_t* p,
                                 const hpulogc_event_t* ev)
{
#if HPULOGC_ENABLE_COLOR
    if (p->color && ev->level >= 0 && ev->level <= 5) {
        return strlen(g_level_colors[ev->level]) + ev->line_len +
               strlen(g_color_reset);
    }
#else
    (void)p;
#endif
    return ev->line_len;
}

/* Async batch path: merge whole lines and emit them with as few fwrite
 * calls (stdio lock round-trips) as possible. Errors are batch-granular:
 * stdio reports no per-event progress within one fwrite, so any error
 * fails the whole batch (return 0; the core books the rest as failed). */
static int console_emit_batch(hpulogc_sink_t* sink,
                              const hpulogc_event_t* const* evs, size_t n)
{
    hpu_console_priv_t* p = hpulogc_sink_priv(sink);
    size_t i;
    int err = 0;

    for (i = 0; i < n && err == 0; i++) {
        const hpulogc_event_t* ev = evs[i];
        size_t need = console_event_size(p, ev);

        if (need > HPU_CONSOLE_COALESCE_CAP) {
            /* Oversized line: drain the buffer first, then write it
             * directly (never split a line, LINE_ATOMIC contract). */
            err = console_coalesce_flush(p);
            if (err == 0) {
                err = console_write_direct(p, ev);
            }
        } else {
            if (p->coalesce_len + need > HPU_CONSOLE_COALESCE_CAP) {
                err = console_coalesce_flush(p);
            }
            if (err == 0) {
                console_coalesce_append(p, ev);
            }
        }
    }
    if (err == 0) {
        err = console_coalesce_flush(p);
    }
    return err != 0 ? 0 : (int)n;
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
    console_emit_batch,
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
