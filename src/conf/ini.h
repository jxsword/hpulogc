/**
 * @file ini.h
 * @brief Internal INI-style configuration lexer/parser (zlog-compatible
 *        lexing per spec 10.1).
 */

#ifndef HPU_INI_H
#define HPU_INI_H

#include <stdio.h>

#include "../../include/hpulogc.h"

/** @brief Maximum physical line length (spec 10.1: 1024 bytes). */
#define HPU_INI_MAX_LINE 1024

/** @brief Line callback: invoked per logical (continuation-joined) line.
 *
 * @param userdata  Opaque caller context.
 * @param line      Logical line text (no trailing newline).
 * @param line_no   Physical line number where the logical line starts.
 * @return          0 to continue, non-zero to abort parsing with that
 *                  value as the result.
 */
typedef int (*hpu_ini_line_fn)(void* userdata, const char* line,
                               int line_no);

/**
 * @brief Parse a configuration file and stream logical lines.
 *
 * Implements the zlog lexing rules (spec 10.1): '#' comments (outside
 * double quotes), trailing-comment stripping, backslash continuation
 * (error line = first physical line), 1024-byte physical line limit.
 * Blank lines and comment-only lines are NOT delivered.
 *
 * @param path      Config file path; must be a regular file.
 * @param cb        Line callback.
 * @param userdata  Passed through to @p cb.
 * @param err_line  Filled with the offending physical line number on
 *                  error (may be NULL).
 * @return          0 on success, HPULOGC_ERR_CONFIG on lexical errors
 *                  (missing file, non-regular file, line too long).
 */
int hpu_ini_parse_file(const char* path, hpu_ini_line_fn cb, void* userdata,
                       int* err_line);

/**
 * @brief Parse an in-memory configuration text (tests and fuzzing).
 *
 * Same lexing rules as hpu_ini_parse_file().
 *
 * @param text      NUL-terminated configuration text.
 * @param cb        Line callback.
 * @param userdata  Passed through to @p cb.
 * @param err_line  Filled with the offending line number on error.
 * @return          0 on success, HPULOGC_ERR_CONFIG on lexical errors.
 */
int hpu_ini_parse_buffer(const char* text, hpu_ini_line_fn cb,
                         void* userdata, int* err_line);

#endif /* HPU_INI_H */
