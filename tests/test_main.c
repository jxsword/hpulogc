/**
 * @file test_main.c
 * @brief Shared test runner implementation linked into every test binary.
 */

#include "test_util.h"

#include <stdarg.h>

/** @brief Registered case slot. */
typedef struct hpu_test_case {
    void (*fn)(void);   /*!< Case entry point */
    const char* name;   /*!< Case display name */
} hpu_test_case_t;

static hpu_test_case_t hpu_test_cases[HPU_TEST_MAX];

int hpu_test_count = 0;
int hpu_test_failed = 0;
const char* hpu_test_current = NULL;
int hpu_test_verbose = 0;

void hpu_test_register(void (*fn)(void), const char* name)
{
    if (hpu_test_count < HPU_TEST_MAX) {
        hpu_test_cases[hpu_test_count].fn = fn;
        hpu_test_cases[hpu_test_count].name = name;
        hpu_test_count++;
    } else {
        fprintf(stderr, "test registry full, dropping case %s\n", name);
    }
}

void hpu_test_fail(const char* file, int line, const char* fmt, ...)
{
    va_list ap;

    fprintf(stderr, "    FAIL [%s] %s:%d: ", hpu_test_current, file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    hpu_test_failed++;
}

int hpu_test_run_all(void)
{
    int i;
    int cases_failed = 0;

    for (i = 0; i < hpu_test_count; i++) {
        int before = hpu_test_failed;

        hpu_test_current = hpu_test_cases[i].name;
        hpu_test_cases[i].fn();
        if (hpu_test_failed == before) {
            printf("[ PASS ] %s\n", hpu_test_cases[i].name);
        } else {
            printf("[ FAIL ] %s\n", hpu_test_cases[i].name);
            cases_failed++;
        }
    }

    printf("%d cases, %d failed, %d failed assertions\n", hpu_test_count,
           cases_failed, hpu_test_failed);
    return hpu_test_failed;
}

int main(int argc, char** argv)
{
    int rc;

    if (argc > 1 && (strcmp(argv[1], "-v") == 0 ||
                     strcmp(argv[1], "--verbose") == 0)) {
        hpu_test_verbose = 1;
    }
    rc = hpu_test_run_all();
    return rc != 0 ? 1 : 0;
}
