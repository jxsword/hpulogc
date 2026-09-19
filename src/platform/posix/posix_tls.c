/**
 * @file posix_tls.c
 * @brief POSIX implementation of thread-local storage slots.
 */

#include "platform/platform.h"

#include <errno.h>
#include <pthread.h>

int hpu_tls_create(hpu_tls_key_t* key, void (*dtor)(void* value))
{
    pthread_key_t k;
    int rc;

    if (key == NULL) {
        return -EINVAL;
    }
    rc = pthread_key_create(&k, dtor);
    if (rc != 0) {
        return -rc;
    }
    key->impl = (unsigned long)k;
    return 0;
}

void hpu_tls_destroy(hpu_tls_key_t* key)
{
    if (key != NULL && key->impl != 0) {
        pthread_key_delete((pthread_key_t)key->impl);
        key->impl = 0;
    }
}

void* hpu_tls_get(const hpu_tls_key_t* key)
{
    return pthread_getspecific((pthread_key_t)key->impl);
}

int hpu_tls_set(const hpu_tls_key_t* key, void* value)
{
    return -pthread_setspecific((pthread_key_t)key->impl, value);
}
