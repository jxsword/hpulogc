/**
 * @file hpu_tls.h
 * @brief Platform contract: thread-local storage slots with destructor.
 *
 * Used for the per-thread render buffer (allocated lazily on first use,
 * freed by the destructor at thread exit; spec 9 memory rules).
 */

#ifndef HPU_TLS_H
#define HPU_TLS_H

/**
 * @brief Thread-local key handle.
 */
typedef struct hpu_tls_key {
    unsigned long impl; /*!< Platform key storage (pthread_key_t) */
} hpu_tls_key_t;

/**
 * @brief Create a thread-local key.
 *
 * @param key   Handle filled on success.
 * @param dtor  Destructor invoked at thread exit on the stored pointer
 *              (may be NULL).
 * @return      0 on success, negative errno-style value on failure.
 */
int hpu_tls_create(hpu_tls_key_t* key, void (*dtor)(void* value));

/**
 * @brief Destroy a key created by hpu_tls_create().
 *
 * Values in other threads are NOT freed by this call.
 *
 * @param key  Key handle.
 */
void hpu_tls_destroy(hpu_tls_key_t* key);

/**
 * @brief Get the value stored for the calling thread.
 * @param key  Key handle.
 * @return     Stored value or NULL.
 */
void* hpu_tls_get(const hpu_tls_key_t* key);

/**
 * @brief Set the value for the calling thread.
 * @param key    Key handle.
 * @param value  Value to store.
 * @return       0 on success, negative errno-style value on failure.
 */
int hpu_tls_set(const hpu_tls_key_t* key, void* value);

#endif /* HPU_TLS_H */
