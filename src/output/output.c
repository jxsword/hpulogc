/**
 * @file output.c
 * @brief Sink type registry, instance creation lifecycle (§4.10.2) and
 *        dispatch over the hpulogc_sink_ops_t vtable.
 */

#include "output.h"
#include "sink_queue.h"
#include "../conf/conf_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hpu_fsync_severity(int cs)
{
    switch (cs) {
    case HPULOGC_CRASH_NONE:
        return HPU_FSYNC_SEV_NONE;
    case HPULOGC_CRASH_SHUTDOWN:
        return HPU_FSYNC_SEV_SHUTDOWN;
    case HPULOGC_CRASH_PERIODIC:
        return HPU_FSYNC_SEV_PERIODIC;
    case HPULOGC_CRASH_ENTRY:
        return HPU_FSYNC_SEV_ENTRY;
    default:
        return HPU_FSYNC_SEV_NONE;
    }
}

/* ------------------------------------------------------------------ */
/* Type registry (built-ins + hpulogc_sink_register)                   */
/* ------------------------------------------------------------------ */

static const hpulogc_sink_ops_t* g_types[HPULOGC_MAX_SINK_TYPES];
static size_t g_type_count;
static int g_registry_ready;
static int g_registry_frozen; /*!< Set while an instance is initialized:
                                   *   registration requires the pre-init
                                   *   single-threaded window (§4.10.5) */

/**
 * @brief Size-suffix parser (zlog semantics: 1k=1000, 1kb=1024, 1m=10^6,
 *        1mb=2^20, 1g=10^9, 1gb=2^30; case-insensitive; bare digits bytes).
 * @return 0 on success, -1 on a malformed value.
 */
int hpu_parse_size_str(const char* s, unsigned long long* out)
{
    unsigned long long v = 0;
    unsigned long long mult = 1;
    int digits = 0;
    char c0;
    char c1;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10U + (unsigned long long)(*s - '0');
        digits++;
        s++;
    }
    if (digits == 0) {
        return -1;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '\0') {
        *out = v;
        return 0;
    }
    c0 = (char)(*s | 0x20);
    c1 = (char)(s[1] | 0x20);
    if (c0 == 'k') {
        mult = (s[1] == '\0') ? 1000ULL : (c1 == 'b' ? 1024ULL : 0);
        s += (s[1] == '\0') ? 1 : 2;
    } else if (c0 == 'm') {
        mult = (s[1] == '\0') ? 1000000ULL : (c1 == 'b' ? 1048576ULL : 0);
        s += (s[1] == '\0') ? 1 : 2;
    } else if (c0 == 'g') {
        mult = (s[1] == '\0') ? 1000000000ULL
                               : (c1 == 'b' ? 1073741824ULL : 0);
        s += (s[1] == '\0') ? 1 : 2;
    } else {
        return -1;
    }
    if (mult == 0 || *s != '\0') {
        return -1;
    }
    *out = v * mult;
    return 0;
}

void hpu_sink_registry_init(void)
{
    if (g_registry_ready) {
        return;
    }
    g_registry_ready = 1;

    g_types[g_type_count++] = hpu_console_sink_ops();
    g_types[g_type_count++] = hpu_rollingfile_sink_ops();
#if defined(HPULOGC_SINK_NULL)
    g_types[g_type_count++] = hpu_null_sink_ops();
#endif
#if defined(HPULOGC_SINK_SYSLOG)
    g_types[g_type_count++] = hpu_syslog_sink_ops();
#endif
#if defined(HPULOGC_SINK_TCP)
    g_types[g_type_count++] = hpu_tcp_sink_ops();
#endif
#if defined(HPULOGC_SINK_UDP)
    g_types[g_type_count++] = hpu_udp_sink_ops();
#endif
}

void hpu_sink_registry_freeze(void)
{
    g_registry_frozen = 1;
}

void hpu_sink_registry_unfreeze(void)
{
    g_registry_frozen = 0;
}

const hpulogc_sink_ops_t* hpu_sink_registry_find(const char* type)
{
    size_t i;

    if (type == NULL) {
        return NULL;
    }
    if (!g_registry_ready) {
        hpu_sink_registry_init(); /* parse-time lookups precede create */
    }
    for (i = 0; i < g_type_count; i++) {
        if (strcmp(g_types[i]->type, type) == 0) {
            return g_types[i];
        }
    }
    return NULL;
}

size_t hpu_sink_registry_count(void)
{
    return g_type_count;
}

int hpulogc_sink_register(const hpulogc_sink_ops_t* ops)
{
    size_t i;

    if (g_registry_frozen) {
        return HPULOGC_ERR_STATE;
    }
    if (ops == NULL || ops->type == NULL || ops->type[0] == '\0' ||
        strlen(ops->type) >= HPULOGC_MAX_NAME_LEN ||
        ops->abi_version != HPULOGC_SINK_ABI_VERSION ||
        (ops->emit == NULL && ops->emit_batch == NULL)) {
        return HPULOGC_ERR_INVALID_ARG;
    }
    for (i = 0; i < 4; i++) {
        if (ops->reserved[i] != NULL) {
            return HPULOGC_ERR_INVALID_ARG;
        }
    }
    if (hpu_sink_registry_find(ops->type) != NULL ||
        g_type_count >= HPULOGC_MAX_SINK_TYPES) {
        return HPULOGC_ERR_CONFIG;
    }
    if (!g_registry_ready) {
        hpu_sink_registry_init();
    }
    g_types[g_type_count++] = ops;
    return HPULOGC_OK;
}

/* ------------------------------------------------------------------ */
/* Instance lifecycle (§4.10.2)                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Common-key predicate: consumed by the core, never forwarded.
 */
static int is_common_key(const char* key)
{
    return strcmp(key, "enabled") == 0 || strcmp(key, "async") == 0 ||
           strcmp(key, "queue size") == 0;
}

/**
 * @brief Parse a boolean value (strict: only true/false, case-insensitive).
 * @return 0/1, or -1 when malformed.
 */
static int parse_bool_str(const char* v)
{
    if (v == NULL) {
        return -1;
    }
    if (strcmp(v, "true") == 0 || strcmp(v, "TRUE") == 0 ||
        strcmp(v, "True") == 0 || strcmp(v, "on") == 0 ||
        strcmp(v, "1") == 0) {
        return 1;
    }
    if (strcmp(v, "false") == 0 || strcmp(v, "FALSE") == 0 ||
        strcmp(v, "False") == 0 || strcmp(v, "off") == 0 ||
        strcmp(v, "0") == 0) {
        return 0;
    }
    return -1;
}

hpu_output_t* hpu_output_create(const char* type, const hpu_kv_t* kvs,
                                size_t nkv, const char* name, int fsync_sev,
                                int use_utc, uint32_t flush_interval_ms,
                                size_t batch_max, int* err)
{
    const hpulogc_sink_ops_t* ops;
    hpu_output_base_t* b;
    size_t i;

    if (err != NULL) {
        *err = HPULOGC_OK;
    }
    hpu_sink_registry_init();
    ops = hpu_sink_registry_find(type);
    if (ops == NULL) {
        if (err != NULL) {
            *err = HPULOGC_ERR_CONFIG;
        }
        return NULL;
    }
    b = calloc(1, sizeof(*b) + ops->priv_size);
    if (b == NULL) {
        if (err != NULL) {
            *err = HPULOGC_ERR_NO_MEM;
        }
        return NULL;
    }
    b->ops = ops;
    b->fsync_sev = fsync_sev;
    b->use_utc = use_utc;
    b->enabled = 1;
    snprintf(b->name, sizeof(b->name), "%s", name != NULL ? name : "");

    /* Private keys first, common keys consumed by the core (§4.7.3). */
    for (i = 0; i < nkv; i++) {
        const char* key = kvs[i].key;
        const char* val = kvs[i].val;

        if (is_common_key(key)) {
            if (strcmp(key, "enabled") == 0) {
                int en = parse_bool_str(val);

                if (en < 0) {
                    goto config_err;
                }
                b->enabled = en;
            } else if (strcmp(key, "async") == 0) {
                int as = parse_bool_str(val);

                if (as < 0) {
                    goto config_err;
                }
                b->async = as;
            } else {
                unsigned long long sz;

                if (val == NULL || hpu_parse_size_str(val, &sz) != 0 ||
                    sz < 64U * 1024U || sz > 16U * 1024U * 1024U) {
                    goto config_err;
                }
                b->queue_size = (size_t)sz;
            }
            continue;
        }
        if (ops->configure == NULL) {
            goto config_err;
        }
        if (ops->configure((hpulogc_sink_t*)(void*)b, key, val) != 0) {
            goto config_err;
        }
    }

    if (b->async && !(ops->caps & HPULOGC_CAP_ASYNC)) {
        /* async=on on a type that does not declare the capability */
        if (err != NULL) {
            *err = HPULOGC_ERR_CONFIG;
        }
        free(b);
        return NULL;
    }
    if (ops->init != NULL &&
        ops->init((hpulogc_sink_t*)(void*)b) != 0) {
        if (err != NULL) {
            *err = HPULOGC_ERR_CONFIG;
        }
        free(b);
        return NULL;
    }
    if (ops->start != NULL &&
        ops->start((hpulogc_sink_t*)(void*)b) != 0) {
        if (err != NULL) {
            *err = HPULOGC_ERR_IO;
        }
        if (ops->destroy != NULL) {
            ops->destroy((hpulogc_sink_t*)(void*)b);
        }
        free(b);
        return NULL;
    }
    if (b->async) {
        /* Second-level queue + worker (§4.10.6); failure -> NO_MEM. */
        if (b->queue_size == 0) {
            b->queue_size = 256U * 1024U; /* defensive: default queue size */
        }
        if (hpu_sink_queue_create((hpu_sink_queue_t**)&b->queue,
                                  (hpulogc_sink_t*)(void*)b, ops,
                                  b->queue_size, flush_interval_ms,
                                  batch_max) != 0) {
            if (err != NULL) {
                *err = HPULOGC_ERR_NO_MEM;
            }
            if (ops->destroy != NULL) {
                ops->destroy((hpulogc_sink_t*)(void*)b);
            }
            free(b);
            return NULL;
        }
    }
    return (hpu_output_t*)(void*)b;

config_err:
    if (err != NULL) {
        *err = HPULOGC_ERR_CONFIG;
    }
    if (ops->destroy != NULL) {
        ops->destroy((hpulogc_sink_t*)(void*)b);
    }
    free(b);
    return NULL;
}

void hpu_output_close(hpu_output_t* o, uint32_t drain_timeout_ms)
{
    hpu_output_base_t* b = (hpu_output_base_t*)(void*)o;

    if (o == NULL) {
        return;
    }
    if (b->queue != NULL) {
        hpu_sink_queue_flush_wait((hpu_sink_queue_t*)b->queue,
                                  drain_timeout_ms);
        hpu_sink_queue_destroy((hpu_sink_queue_t*)b->queue);
        b->queue = NULL;
    }
    (void)hpu_output_flush(o);
    if (b->ops->destroy != NULL) {
        b->ops->destroy((hpulogc_sink_t*)(void*)o);
    }
    free(o);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

int hpu_output_deliver(hpu_output_t* o, const hpulogc_event_t* ev)
{
    hpu_output_base_t* b = (hpu_output_base_t*)(void*)o;

    if (o == NULL || ev == NULL || ev->line_len == 0 || !b->enabled) {
        return -1;
    }
    if (b->queue != NULL) {
        /* Async delivery: pack into the second-level queue; a full queue
         * discards the event and counts the drop (D-S5). */
        if (hpu_sink_queue_push((hpu_sink_queue_t*)b->queue, ev) != 0) {
            hpu_at_fetch_add_u64(&b->st_dropped, 1, HPU_MO_RELAXED);
            return -1;
        }
    } else {
        b->ops->emit((hpulogc_sink_t*)(void*)o, ev);
    }
    hpu_at_fetch_add_u64(&b->st_written, 1, HPU_MO_RELAXED);
    hpu_at_fetch_add_u64(&b->st_bytes, (uint64_t)ev->line_len,
                         HPU_MO_RELAXED);
    return 0;
}

int hpu_output_flush(hpu_output_t* o)
{
    hpu_output_base_t* b = (hpu_output_base_t*)(void*)o;

    if (o == NULL) {
        return 0;
    }
    if (b->ops->flush != NULL) {
        return b->ops->flush((hpulogc_sink_t*)(void*)o);
    }
    return 0;
}

int hpu_output_sync(hpu_output_t* o)
{
    hpu_output_base_t* b = (hpu_output_base_t*)(void*)o;

    if (o == NULL) {
        return 0;
    }
    if (hpu_output_flush(o) != 0) {
        return -1;
    }
    if (b->ops->sync != NULL) {
        return b->ops->sync((hpulogc_sink_t*)(void*)o);
    }
    return 0;
}

int hpu_output_periodic(hpu_output_t* o, uint64_t now_ns,
                        uint64_t interval_ns)
{
    hpu_output_base_t* b = (hpu_output_base_t*)(void*)o;

    if (o == NULL) {
        return 0;
    }
    if (b->ops->periodic != NULL) {
        return b->ops->periodic((hpulogc_sink_t*)(void*)o, now_ns,
                                interval_ns);
    }
    return 0;
}

const char* hpu_output_name(const hpu_output_t* o)
{
    if (o == NULL) {
        return "(null)";
    }
    return ((hpu_output_base_t*)(void*)o)->name;
}

unsigned long long hpu_output_lost_total(hpu_output_t* o)
{
    hpu_output_base_t* b = (hpu_output_base_t*)(void*)o;

    if (o == NULL) {
        return 0;
    }
    return hpu_at_load_u64(&b->st_failed, HPU_MO_ACQUIRE);
}

void hpu_output_get_stats(hpu_output_t* o, hpulogc_sink_stats_t* out)
{
    hpu_output_base_t* b = (hpu_output_base_t*)(void*)o;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (o == NULL) {
        return;
    }
    out->written = hpu_at_load_u64(&b->st_written, HPU_MO_ACQUIRE);
    out->dropped = hpu_at_load_u64(&b->st_dropped, HPU_MO_ACQUIRE);
    out->failed = hpu_at_load_u64(&b->st_failed, HPU_MO_ACQUIRE);
    out->bytes_written = hpu_at_load_u64(&b->st_bytes, HPU_MO_ACQUIRE);
}

/* ---- internal helpers for built-in sinks ---- */

hpu_output_base_t* hpu_sink_base(hpulogc_sink_t* sink)
{
    return (hpu_output_base_t*)(void*)sink;
}

void hpu_sink_account_failed(hpulogc_sink_t* sink, unsigned long long n)
{
    if (sink != NULL && n > 0) {
        hpu_at_fetch_add_u64(&hpu_sink_base(sink)->st_failed, n,
                             HPU_MO_ACQ_REL);
    }
}

void* hpulogc_sink_priv(hpulogc_sink_t* sink)
{
    if (sink == NULL) {
        return NULL;
    }
    return (char*)(void*)sink + sizeof(hpu_output_base_t);
}

/* ------------------------------------------------------------------ */
/* Configuration-definition creation (flat shim + generic declaration) */
/* ------------------------------------------------------------------ */

/**
 * @brief Append one common key/value pair to the kv list.
 */
static void def_append_kv(hpu_kv_t* kvs, size_t* nkv, const char* key,
                          char* valbuf, size_t bufcap, long v)
{
    snprintf(valbuf, bufcap, "%ld", v);
    kvs[*nkv].key = key;
    kvs[*nkv].val = valbuf;
    (*nkv)++;
}

hpu_output_t* hpu_output_open_from_conf(const struct hpu_conf_output* def,
                                        int effective_fsync, int use_utc,
                                        uint32_t flush_interval_ms,
                                        size_t batch_max, int* err)
{
    static const char* const rotate_names[] = { "none", "size", "time",
                                                "both" };
    static const char* const unit_names[] = { "hour", "day", "week",
                                              "month" };
    hpu_kv_t kvs[HPU_CONF_MAX_SINK_KV + 12];
    char bufs[6][32];
    size_t nkv = 0;
    const char* type;
    hpu_output_t* o;
    size_t i;

    if (def == NULL) {
        if (err != NULL) {
            *err = HPULOGC_ERR_INVALID_ARG;
        }
        return NULL;
    }

    if (def->is_generic) {
        type = def->type_name;
        for (i = 0; i < def->kv_count; i++) {
            kvs[nkv].key = def->kv_pool + def->kv_key_off[i];
            kvs[nkv].val = def->kv_pool + def->kv_val_off[i];
            nkv++;
        }
    } else if (def->pub.type == HPULOGC_OUT_FILE) {
        /* Deprecated flat descriptor mapped onto rollingfile keys. */
        type = "rollingfile";
        kvs[nkv].key = "path";
        kvs[nkv].val = def->pub.path != NULL ? def->pub.path : "";
        nkv++;
        kvs[nkv].key = "rotate";
        kvs[nkv].val = rotate_names[def->pub.rotate & 3];
        nkv++;
        if (def->pub.max_size > 0) {
            snprintf(bufs[0], sizeof(bufs[0]), "%zu", def->pub.max_size);
            kvs[nkv].key = "max size";
            kvs[nkv].val = bufs[0];
            nkv++;
        }
        kvs[nkv].key = "time unit";
        kvs[nkv].val = unit_names[def->pub.time_unit & 3];
        nkv++;
        def_append_kv(kvs, &nkv, "max files", bufs[1], sizeof(bufs[1]),
                      (long)def->pub.max_files);
        if (def->pub.fsync) {
            kvs[nkv].key = "fsync";
            kvs[nkv].val = "true";
            nkv++;
        }
        if (def->pub.symlink_latest) {
            kvs[nkv].key = "symlink latest";
            kvs[nkv].val = "true";
            nkv++;
        }
        if (def->pub.rotate_naming != NULL) {
            kvs[nkv].key = "rotate naming";
            kvs[nkv].val = def->pub.rotate_naming;
            nkv++;
        }
        snprintf(bufs[2], sizeof(bufs[2]), "%o",
                 def->pub.file_mode != 0 ? def->pub.file_mode : 0644u);
        kvs[nkv].key = "file perms";
        kvs[nkv].val = bufs[2];
        nkv++;
        snprintf(bufs[3], sizeof(bufs[3]), "%o",
                 def->pub.dir_mode != 0 ? def->pub.dir_mode : 0755u);
        kvs[nkv].key = "dir perms";
        kvs[nkv].val = bufs[3];
        nkv++;
    } else {
        type = "console";
        kvs[nkv].key = "stream";
        kvs[nkv].val = def->pub.stream == 1 ? "stderr" : "stdout";
        nkv++;
        kvs[nkv].key = "color";
        kvs[nkv].val = def->pub.color ? "true" : "false";
        nkv++;
    }

    /* Common keys are appended uniformly for both shapes (§4.7.3). */
    if (!def->enabled) {
        kvs[nkv].key = "enabled";
        kvs[nkv].val = "false";
        nkv++;
    }
    if (def->async) {
        kvs[nkv].key = "async";
        kvs[nkv].val = "on";
        nkv++;
    }
    if (def->queue_size > 0) {
        snprintf(bufs[4], sizeof(bufs[4]), "%zu", def->queue_size);
        kvs[nkv].key = "queue size";
        kvs[nkv].val = bufs[4];
        nkv++;
    }
    o = hpu_output_create(type, kvs, nkv, def->name_buf, effective_fsync,
                          use_utc, flush_interval_ms, batch_max, err);
    if (o == NULL && err != NULL && *err == HPULOGC_ERR_CONFIG &&
        def->pub.type == HPULOGC_OUT_FILE && !def->is_generic) {
        /* The v0.2 flat path reported I/O failures for open problems;
         * config-value errors keep their code, everything else maps to
         * ERR_IO for compatibility. */
        *err = HPULOGC_ERR_IO;
    }
    return o;
}
