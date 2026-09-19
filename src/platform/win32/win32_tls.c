/* -*- coding: utf-8 -*- */

/**
 * @file win32_tls.c
 * @brief Win32 implementation of TLS slots with destructors.
 *
 * Fiber-Local Storage (FlsAlloc family) is used because it is the only
 * Windows API that invokes a per-slot destructor at thread exit — the
 * per-thread render buffers rely on that (spec 9 memory rules). FLS
 * callbacks receive only the value, so each key is bound to one of a
 * fixed set of trampoline thunks carrying the user destructor.
 */

#include "platform/platform.h"

#include <errno.h>
#include <string.h>
#include <windows.h>

/** @brief Maximum simultaneous TLS keys (the library uses one). */
#define HPU_TLS_MAX_KEYS 8

/** @brief Slot binding a user destructor to a trampoline. */
typedef struct hpu_tls_slot {
    void (*dtor)(void*); /*!< User destructor (NULL = none) */
    DWORD fls;           /*!< FLS index bound to this slot */
    int   used;          /*!< Slot in use */
} hpu_tls_slot_t;

static hpu_tls_slot_t g_tls_slots[HPU_TLS_MAX_KEYS];

#define HPU_TLS_DEFINE_THUNK(i)                                              \
    static void WINAPI hpu_tls_thunk_##i(void* value)                        \
    {                                                                        \
        if (g_tls_slots[i].dtor != NULL) {                                   \
            g_tls_slots[i].dtor(value);                                      \
        }                                                                    \
    }

HPU_TLS_DEFINE_THUNK(0)
HPU_TLS_DEFINE_THUNK(1)
HPU_TLS_DEFINE_THUNK(2)
HPU_TLS_DEFINE_THUNK(3)
HPU_TLS_DEFINE_THUNK(4)
HPU_TLS_DEFINE_THUNK(5)
HPU_TLS_DEFINE_THUNK(6)
HPU_TLS_DEFINE_THUNK(7)

static const PFLS_CALLBACK_FUNCTION g_tls_thunks[HPU_TLS_MAX_KEYS] = {
    hpu_tls_thunk_0, hpu_tls_thunk_1, hpu_tls_thunk_2, hpu_tls_thunk_3,
    hpu_tls_thunk_4, hpu_tls_thunk_5, hpu_tls_thunk_6, hpu_tls_thunk_7
};

int hpu_tls_create(hpu_tls_key_t* key, void (*dtor)(void* value))
{
    int i;

    if (key == NULL) {
        return -EINVAL;
    }
    for (i = 0; i < HPU_TLS_MAX_KEYS; i++) {
        DWORD fls;

        if (g_tls_slots[i].used) {
            continue;
        }
        g_tls_slots[i].dtor = dtor;
        fls = FlsAlloc(g_tls_thunks[i]);
        if (fls == FLS_OUT_OF_INDEXES) {
            g_tls_slots[i].dtor = NULL;
            return -ENOMEM;
        }
        g_tls_slots[i].fls  = fls;
        g_tls_slots[i].used = 1;
        key->impl = (unsigned long)fls;
        return 0;
    }
    return -ENOMEM;
}

void hpu_tls_destroy(hpu_tls_key_t* key)
{
    int i;

    if (key == NULL || key->impl == 0) {
        return;
    }
    (void)FlsFree((DWORD)key->impl);
    for (i = 0; i < HPU_TLS_MAX_KEYS; i++) {
        if (g_tls_slots[i].used && g_tls_slots[i].fls == (DWORD)key->impl) {
            g_tls_slots[i].used = 0;
            g_tls_slots[i].dtor = NULL;
            break;
        }
    }
    key->impl = 0;
}

void* hpu_tls_get(const hpu_tls_key_t* key)
{
    if (key == NULL) {
        return NULL;
    }
    return FlsGetValue((DWORD)key->impl);
}

int hpu_tls_set(const hpu_tls_key_t* key, void* value)
{
    if (key == NULL) {
        return -EINVAL;
    }
    if (!FlsSetValue((DWORD)key->impl, value)) {
        return -EINVAL;
    }
    return 0;
}
