/**
 * @file fuzz_main.c
 * @brief Deterministic random-mutation fuzz driver (no libFuzzer needed).
 *
 * Runs a fixed number of mutations of seed configuration texts through
 * LLVMFuzzerTestOneInput() with a deterministic PRNG, so the same run is
 * reproducible. Used as the CTest fuzz test when libFuzzer is not
 * invoked separately.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief libFuzzer-compatible entry (fuzz_ini.c). */
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

/** @brief Seed configurations covering the grammar surface. */
static const char* const g_seeds[] = {
    "[global]\nlevel = info\nstrict init = true\n",
    "[formats]\nmine = \"%level: %msg%n\"\n",
    "[outputs]\nf = file, path=/tmp/x.log, rotate=size, max size=1mb\n"
    "c = console, stream=stderr, color=true\n",
    "[buffer]\nbuffer size = 1mb\noverflow policy = discard\n",
    "[rules]\napp.debug.* = detailed, f\n*.* = standard\n",
    "[async]\nbatch size = 64\nflush interval = 100\n",
    "[throttle]\nglobal rate limit = 10\nsampling rate = 0.5\n",
    "[advanced]\nmax log length = 4096\nescape injection = true\n",
    "[build]\nbuild version = full\nconcurrency = mpsc\n",
    "key = one \\\ntwo # comment\n",
};

/** @brief Deterministic xorshift PRNG. */
static uint64_t prng_state = 0x9e3779b97f4a7c15ULL;

/**
 * @brief Next pseudo-random value.
 */
static uint64_t prng_next(void)
{
    uint64_t x = prng_state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    prng_state = x;
    return x;
}

int main(int argc, char** argv)
{
    long iterations = 20000;
    uint8_t buf[4096];
    long i;
    size_t nseeds = sizeof(g_seeds) / sizeof(g_seeds[0]);

    if (argc > 1) {
        iterations = atol(argv[1]);
        if (iterations <= 0) {
            iterations = 20000;
        }
    }
    if (argc > 2) {
        prng_state = strtoull(argv[2], NULL, 16); /* reproducible seed */
    }

    for (i = 0; i < iterations; i++) {
        size_t seed = (size_t)(prng_next() % nseeds);
        size_t len = strlen(g_seeds[seed]);
        size_t mutations = prng_next() % 24;
        size_t m;

        if (len >= sizeof(buf)) {
            len = sizeof(buf) - 1;
        }
        memcpy(buf, g_seeds[seed], len);

        for (m = 0; m < mutations; m++) {
            uint64_t r = prng_next();

            switch (r % 5) {
            case 0: /* flip a byte */
                if (len > 0) {
                    buf[r % len] = (uint8_t)(r >> 8);
                }
                break;
            case 1: /* insert a byte */
                if (len + 1 < sizeof(buf)) {
                    size_t at = (size_t)(r % (len + 1));

                    memmove(buf + at + 1, buf + at, len - at);
                    buf[at] = (uint8_t)(r >> 16);
                    len++;
                }
                break;
            case 2: /* delete a byte */
                if (len > 1) {
                    size_t at = (size_t)(r % len);

                    memmove(buf + at, buf + at + 1, len - at - 1);
                    len--;
                }
                break;
            case 3: /* truncate */
                if (len > 0) {
                    len = (size_t)(r >> 8) % len;
                }
                break;
            default: /* append a chunk */
            {
                if (len + 4 < sizeof(buf)) {
                    buf[len++] = (uint8_t)(r >> 8);
                    buf[len++] = (uint8_t)(r >> 16);
                    buf[len++] = (uint8_t)(r >> 24);
                    buf[len++] = '\\';
                }
                break;
            }
            }
        }
        LLVMFuzzerTestOneInput(buf, len);
    }

    printf("fuzz: %ld iterations done (no crash)\n", iterations);
    return 0;
}
