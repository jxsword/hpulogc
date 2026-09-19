/**
 * @file output_console.c
 * @brief Console output backend (stdout/stderr, terminal-only ANSI color).
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
 * @brief Console output instance.
 */
typedef struct hpu_console_out {
    hpu_output_base_t base; /*!< Common header (offset 0) */
    FILE* stream;           /*!< stdout or stderr */
    int   fd;               /*!< Descriptor for the isatty check */
    int   color;            /*!< Non-zero when color codes are emitted */
} hpu_console_out_t;

hpu_output_t* hpu_console_output_open(const hpulogc_output_t* cfg,
                                      int effective_fsync)
{
    hpu_console_out_t* o = calloc(1, sizeof(*o));

    if (o == NULL) {
        return NULL;
    }
    o->base.type = HPULOGC_OUT_CONSOLE;
    o->base.fsync_sev = effective_fsync;
    snprintf(o->base.name, sizeof(o->base.name), "console_%s",
             cfg->stream == 1 ? "stderr" : "stdout");

    o->stream = (cfg->stream == 1) ? stderr : stdout;
    o->fd = (cfg->stream == 1) ? 2 : 1;
    o->color = 0;
#if HPULOGC_ENABLE_COLOR
    if (cfg->color && hpu_fs_isatty(o->fd)) {
        o->color = 1; /* redirected pipes/files stay plain (spec 4.7) */
    }
#else
    (void)cfg;
#endif
    return (hpu_output_t*)(void*)o;
}

int hpu_console_output_write_line(hpu_output_t* o, const char* line,
                                  size_t len, int level)
{
    hpu_console_out_t* c = (hpu_console_out_t*)(void*)o;

#if HPULOGC_ENABLE_COLOR
    if (c->color && level >= 0 && level <= 5) {
        fwrite(g_level_colors[level], 1, strlen(g_level_colors[level]),
               c->stream);
        fwrite(line, 1, len, c->stream);
        fwrite(g_color_reset, 1, strlen(g_color_reset), c->stream);
        return 0;
    }
#else
    (void)level;
#endif
    fwrite(line, 1, len, c->stream);
    return ferror(c->stream) != 0 ? -1 : 0;
}

int hpu_console_output_flush(hpu_output_t* o)
{
    hpu_console_out_t* c = (hpu_console_out_t*)(void*)o;

    return fflush(c->stream) == 0 ? 0 : -1;
}

int hpu_output_write_direct(hpu_output_t* o, const char* bytes, size_t len)
{
    hpu_output_base_t* b = (hpu_output_base_t*)(void*)o;

    if (b->type == HPULOGC_OUT_CONSOLE) {
        hpu_console_out_t* c = (hpu_console_out_t*)(void*)o;

        fwrite(bytes, 1, len, c->stream);
        return ferror(c->stream) != 0 ? -1 : 0;
    }
    return hpu_file_output_write_line(o, bytes, len);
}

void hpu_output_close_impl(hpu_output_t* o)
{
    hpu_output_base_t* b = (hpu_output_base_t*)(void*)o;

    if (b->type == HPULOGC_OUT_CONSOLE) {
        free(o);
    } else {
        hpu_file_output_close(o);
    }
}
