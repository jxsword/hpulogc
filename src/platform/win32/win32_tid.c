/* -*- coding: utf-8 -*- */

/**
 * @file win32_tid.c
 * @brief Win32 implementation of the thread id contract.
 *
 * GetCurrentThreadId reads the TEB directly (no syscall), so no caching
 * is needed.
 */

#include "platform/platform.h"

uint64_t hpu_thread_id(void)
{
    return (uint64_t)GetCurrentThreadId();
}
