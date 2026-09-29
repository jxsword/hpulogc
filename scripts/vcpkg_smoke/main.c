/* -*- coding: utf-8 -*- */

/**
 * @file main.c
 * @brief vcpkg smoke consumer: verifies the installed package resolves
 *        through find_package(hpulogc) and that the exported
 *        hpulogc::hpulogc CMake target links and runs (D-R9 CI gate).
 */

#include <hpulogc.h>

#include <stdio.h>

int main(void)
{
    if (hpulogc_init_default() != 0) {
        fprintf(stderr, "hpulogc_init_default failed\n");
        return 1;
    }
    HPULOGC_INFO("smoke", "vcpkg smoke consumer is alive");
    hpulogc_shutdown();
    printf("hpulogc vcpkg smoke: OK\n");
    return 0;
}
