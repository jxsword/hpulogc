/* -*- coding: utf-8 -*- */

/**
 * @file hpulogc_chk_conf.c
 * @brief Configuration-file validation CLI (spec rd_v0.6 v0.6.1 §11).
 *
 * Thin shell over hpulogc_conf_validate(): parse the given INI file, run
 * the full semantic validation (dry-run, no log files are created) and
 * report the result via exit code:
 *   0 = configuration is valid
 *   1 = configuration is invalid (first diagnostic on stderr)
 *   2 = usage error / file not readable
 */

#include "hpulogc.h"

#include <stdio.h>
#include <string.h>

/** @brief Exit code: configuration valid. */
#define CHK_EXIT_OK 0
/** @brief Exit code: configuration invalid. */
#define CHK_EXIT_INVALID 1
/** @brief Exit code: usage error or unreadable file. */
#define CHK_EXIT_USAGE 2

/**
 * @brief Print usage text.
 *
 * @param prog  Program name (argv[0]).
 */
static void print_usage(const char* prog)
{
    fprintf(stderr,
            "usage: %s [--strict|--lenient] <config.ini>\n"
            "\n"
            "Validate an hpulogc INI configuration file (dry-run: no log\n"
            "files are created or opened).\n"
            "\n"
            "options:\n"
            "  --strict   treat unknown keys as errors (overrides the\n"
            "             file's own 'strict init' value)\n"
            "  --lenient  ignore unknown keys with a warning (overrides\n"
            "             the file's own 'strict init' value)\n"
            "  (default)  follow the file's own 'strict init' key\n"
            "\n"
            "exit codes: 0 valid, 1 invalid, 2 usage/IO error\n",
            prog);
}

int main(int argc, char** argv)
{
    const char* path = NULL;
    int strict = -1; /* -1: follow the file's own 'strict init' */
    char err[1024];
    int i;
    int rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--strict") == 0) {
            strict = 1;
        } else if (strcmp(argv[i], "--lenient") == 0) {
            strict = 0;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return CHK_EXIT_OK;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "hpulogc: %s: unknown option\n", argv[i]);
            print_usage(argv[0]);
            return CHK_EXIT_USAGE;
        } else if (path == NULL) {
            path = argv[i];
        } else {
            fprintf(stderr, "hpulogc: more than one config file given\n");
            return CHK_EXIT_USAGE;
        }
    }
    if (path == NULL) {
        print_usage(argv[0]);
        return CHK_EXIT_USAGE;
    }

    rc = hpulogc_conf_validate(path, strict, err, sizeof(err));
    if (rc == HPULOGC_OK) {
        printf("%s: OK\n", path);
        return CHK_EXIT_OK;
    }
    if (rc == HPULOGC_ERR_IO) {
        fputs(err, stderr);
        return CHK_EXIT_USAGE;
    }
    if (rc == HPULOGC_ERR_INVALID_ARG) {
        fprintf(stderr, "hpulogc: %s: invalid argument\n", path);
        return CHK_EXIT_USAGE;
    }
    /* HPULOGC_ERR_CONFIG: report the captured first diagnostic; fall back
     * to stderr replay when the caller asked for no capture buffer. */
    if (err[0] != '\0') {
        fputs(err, stderr);
    } else {
        /* Re-run without a buffer so the diagnostics reach stderr. */
        (void)hpulogc_conf_validate(path, strict, NULL, 0);
    }
    return CHK_EXIT_INVALID;
}
