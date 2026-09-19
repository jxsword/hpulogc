/**
 * @file ini.c
 * @brief zlog-style line lexer: comments, quoting, continuation lines.
 */

#include "ini.h"

#include <string.h>

#include "../platform/platform.h"

/** @brief Upper bound for one logical (continuation-joined) line. */
#define HPU_INI_MAX_LOGICAL (4 * HPU_INI_MAX_LINE)

/**
 * @brief Lexer state shared by the file and buffer entry points.
 */
typedef struct ini_lexer {
    char logical[HPU_INI_MAX_LOGICAL]; /*!< Joined line under construction */
    size_t logical_len;                /*!< Current joined length */
    int logical_start;                 /*!< First physical line number */
    hpu_ini_line_fn cb;                /*!< Line callback */
    void* userdata;                    /*!< Callback context */
    int* err_line;                     /*!< Error line output */
} ini_lexer_t;

/**
 * @brief Strip comments and trailing whitespace, respecting quotes.
 *
 * A '#' outside double quotes starts a comment running to the end of the
 * physical line. Quote state does not carry across continuation lines
 * (each physical line is lexed independently before joining).
 *
 * @param line  Physical line (modified in place).
 * @return      Resulting length after stripping.
 */
static size_t strip_comment_and_trim(char* line)
{
    char* p = line;
    int in_quotes = 0;
    char* comment = NULL;
    size_t len;

    while (*p != '\0') {
        if (*p == '"') {
            in_quotes = !in_quotes;
        } else if (*p == '#' && !in_quotes) {
            comment = p;
            break;
        }
        p++;
    }
    if (comment != NULL) {
        *comment = '\0';
    }

    len = strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' ||
                       line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }
    return len;
}

/**
 * @brief Deliver one finished logical line to the callback.
 * @return 0 to continue, callback value to abort.
 */
static int deliver(ini_lexer_t* lx)
{
    size_t len;
    int rc;

    if (lx->logical_len == 0) {
        return 0;
    }
    lx->logical[lx->logical_len] = '\0';
    len = strip_comment_and_trim(lx->logical);
    if (len == 0) {
        lx->logical_len = 0;
        lx->logical_start = 0;
        return 0; /* comment-only line */
    }
    rc = lx->cb(lx->userdata, lx->logical, lx->logical_start);
    lx->logical_len = 0;
    lx->logical_start = 0;
    return rc;
}

/**
 * @brief Process one physical line.
 * @return 0 to continue, callback value to abort, HPULOGC_ERR_CONFIG on
 *         lexical errors.
 */
static int process_line(ini_lexer_t* lx, char* phys, int line_no,
                        int is_last)
{
    size_t plen = strlen(phys);
    size_t bs = 0;
    int is_continuation;

    if (plen >= HPU_INI_MAX_LINE) {
        if (lx->err_line != NULL) {
            *lx->err_line = line_no;
        }
        return HPULOGC_ERR_CONFIG; /* physical line too long */
    }

    /* Comment stripping happens per physical line; a '#' inside quotes
     * survives (quote pairing is per physical line). */
    {
        char* q = phys;
        int in_quotes = 0;
        char* comment = NULL;

        while (*q != '\0') {
            if (*q == '"') {
                in_quotes = !in_quotes;
            } else if (*q == '#' && !in_quotes) {
                comment = q;
                break;
            }
            q++;
        }
        if (comment != NULL) {
            *comment = '\0';
        }
        plen = strlen(phys);
        while (plen > 0 && (phys[plen - 1] == ' ' || phys[plen - 1] == '\t' ||
                            phys[plen - 1] == '\r')) {
            phys[--plen] = '\0';
        }
    }

    /* Odd number of trailing backslashes = continuation. */
    while (bs < plen && phys[plen - 1 - bs] == '\\') {
        bs++;
    }
    is_continuation = (bs % 2 == 1) && !is_last;
    if (bs % 2 == 1) {
        phys[plen - 1] = '\0'; /* drop the continuation backslash */
        plen--; /* interior whitespace stays part of the value */
    } else if (bs >= 2) {
        /* an escaped backslash: the pair denotes one literal backslash */
        phys[plen - 1] = '\0';
        plen--;
    }

    if (lx->logical_len + plen + 1 >= sizeof(lx->logical)) {
        if (lx->err_line != NULL) {
            *lx->err_line = lx->logical_start ? lx->logical_start : line_no;
        }
        return HPULOGC_ERR_CONFIG;
    }
    if (plen > 0 && lx->logical_len == 0 && lx->logical_start == 0) {
        lx->logical_start = line_no;
    }
    memcpy(lx->logical + lx->logical_len, phys, plen);
    lx->logical_len += plen;

    if (!is_continuation) {
        return deliver(lx);
    }
    return 0;
}

int hpu_ini_parse_buffer(const char* text, hpu_ini_line_fn cb,
                         void* userdata, int* err_line)
{
    ini_lexer_t lx;
    const char* p = text;
    int line_no = 0;

    memset(&lx, 0, sizeof(lx));
    lx.cb = cb;
    lx.userdata = userdata;
    lx.err_line = err_line;

    while (*p != '\0') {
        char phys[HPU_INI_MAX_LINE];
        size_t n = 0;
        int is_last;

        line_no++;
        while (*p != '\0' && *p != '\n') {
            if (n + 1 >= sizeof(phys)) {
                if (lx.err_line != NULL) {
                    *lx.err_line = line_no;
                }
                return HPULOGC_ERR_CONFIG; /* physical line too long */
            }
            phys[n++] = *p++;
        }
        if (*p == '\n') {
            p++;
        }
        phys[n] = '\0';
        is_last = (*p == '\0');
        {
            int rc = process_line(&lx, phys, line_no, is_last);

            if (rc != 0) {
                return rc;
            }
        }
    }
    return deliver(&lx);
}

int hpu_ini_parse_file(const char* path, hpu_ini_line_fn cb, void* userdata,
                       int* err_line)
{
    FILE* fp;
    char line[HPU_INI_MAX_LINE];
    ini_lexer_t lx;
    int line_no = 0;

    if (path == NULL) {
        return HPULOGC_ERR_INVALID_ARG;
    }
    if (hpu_fs_stat_kind(path) != HPU_FS_REGULAR) {
        /* Missing files, directories, FIFOs and devices all reject the
         * startup (spec 10.4). */
        return HPULOGC_ERR_CONFIG;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return HPULOGC_ERR_CONFIG;
    }

    memset(&lx, 0, sizeof(lx));
    lx.cb = cb;
    lx.userdata = userdata;
    lx.err_line = err_line;

    for (;;) {
        int rc;

        if (fgets(line, (int)sizeof(line), fp) == NULL) {
            break; /* EOF or read error: flush what we have */
        }
        line_no++;
        {
            size_t len = strlen(line);
            int is_last = (feof(fp) != 0);

            if (len > 0 && line[len - 1] != '\n' && !is_last) {
                /* no newline and not EOF: the line is longer than the
                 * physical limit */
                fclose(fp);
                if (err_line != NULL) {
                    *err_line = line_no;
                }
                return HPULOGC_ERR_CONFIG;
            }
            rc = process_line(&lx, line, line_no, is_last);
            if (rc != 0) {
                fclose(fp);
                return rc;
            }
        }
    }
    fclose(fp);
    return deliver(&lx);
}
