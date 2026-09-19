/**
 * @file minimal.c
 * @brief Minimal hpulogc example: defaults, one-line setup.
 */

#include <hpulogc.h>

int main(void)
{
    /* Built-in defaults: level=INFO, standard format, stderr console. */
    if (hpulogc_init_default() != HPULOGC_OK) {
        return 1;
    }

    HPULOGC_INFO("main", "hello from hpulogc %s", HPULOGC_VERSION_STRING);
    HPULOGC_WARN("main", "this is a warning");
    HPULOGC_ERROR("main", "and an error with value %d", 42);

    hpulogc_shutdown();
    return 0;
}
