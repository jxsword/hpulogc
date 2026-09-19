/**
 * @file fuzz_ini.c
 * @brief Fuzz harness for the INI configuration parser.
 *
 * LLVMFuzzerTestOneInput-compatible entry: works with libFuzzer
 * (-fsanitize=fuzzer) and with the deterministic driver in fuzz_main.c.
 * Every input is parsed through hpu_ini_parse_buffer(); the harness must
 * never crash, hang or leak.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "conf/ini.h"

/**
 * @brief Silent line callback (never fails).
 */
static int fuzz_line_cb(void* ud, const char* line, int line_no)
{
    (void)ud;
    (void)line;
    (void)line_no;
    return 0;
}

/**
 * @brief libFuzzer entry point.
 *
 * @param data  Arbitrary bytes.
 * @param size  Byte count.
 * @return      Always 0.
 */
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    char* text;
    int err_line = 0;

    if (size > 65536) {
        return 0;
    }
    text = malloc(size + 1);
    if (text == NULL) {
        return 0;
    }
    memcpy(text, data, size);
    text[size] = '\0';

    /* The parser must never crash regardless of input. */
    (void)hpu_ini_parse_buffer(text, fuzz_line_cb, NULL, &err_line);

    free(text);
    return 0;
}
