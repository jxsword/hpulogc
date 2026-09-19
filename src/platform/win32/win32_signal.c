/* -*- coding: utf-8 -*- */

/**
 * @file win32_signal.c
 * @brief Win32 implementation of the SIGHUP contract stub.
 *
 * Windows has no SIGHUP: registration always fails per the contract
 * (hpu_signal.h), init warns once and hot reload relies on polling.
 */

#include "platform/platform.h"

int hpu_signal_install_hup(void)
{
    return -1; /* no SIGHUP concept on Windows */
}

int hpu_signal_take_hup(void)
{
    return 0; /* a signal can never arrive */
}
