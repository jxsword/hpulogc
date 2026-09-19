/**
 * @file custom_format.c
 * @brief Custom format example: named format templates from a config
 *        file, rotation and fsync policy.
 */

#include <stdio.h>

#include <hpulogc.h>

/** @brief Example configuration with custom named formats. */
static const char* const g_config =
    "[global]\n"
    "level = trace\n"
    "default format = standard\n"
    "default outputs = out\n"
    "\n"
    "[formats]\n"
    "compact  = \"%time [%level] %msg%n\"\n"
    "verbose  = \"%time [%level] [%category] [pid:%pid tid:%tid] %msg%n\"\n"
    "\n"
    "[outputs]\n"
    "out = file, path=hpulogc_custom_example.log, rotate=size, "
    "max size=64kb, max files=3, fsync=false, symlink latest=true\n"
    "\n"
    "[rules]\n"
    "net.* = verbose, out\n"
    "*.* = compact, out\n";

int main(void)
{
    int i;
    char path[64];

    /* Write the configuration to a temp file and load it. */
    {
        FILE* fp = fopen("hpulogc_custom_example.ini", "w");

        if (fp == NULL) {
            return 1;
        }
        fputs(g_config, fp);
        fclose(fp);
    }

    if (hpulogc_init_from_file("hpulogc_custom_example.ini") !=
        HPULOGC_OK) {
        return 1;
    }

    for (i = 0; i < 200; i++) {
        snprintf(path, sizeof(path), "conn-%d", i % 7);
        HPULOGC_INFO(path, "message %d with a longer payload", i);
    }

    hpulogc_shutdown();
    printf("done: see hpulogc_custom_example.log\n");
    return 0;
}
