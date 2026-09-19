/* -*- coding: utf-8 -*- */

/**
 * @file registry.c
 * @brief Category registry: registration with the count-publish protocol,
 *        per-level routing caches and per-category level overrides
 *        (spec 4.9; decisions 14/19).
 */

#include "core_internal.h"

#include <stdio.h>
#include <string.h>

hpu_registry_t g_reg;

/**
 * @brief FNV-1a 32-bit hash (decision 14).
 */
static uint32_t fnv1a(const char* s, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

int hpu_registry_init(void)
{
    memset(&g_reg, 0, sizeof(g_reg));
    return hpu_mutex_init(&g_reg.mu);
}

void hpu_registry_shutdown(void)
{
    hpu_mutex_destroy(&g_reg.mu);
}

void hpu_registry_reset(void)
{
    /* Called from the init/shutdown thread only (single-threaded state). */
    memset(&g_reg, 0, sizeof(g_reg));
    hpu_mutex_init(&g_reg.mu);
}

hpu_reg_entry_t* hpu_registry_find(const char* category, size_t len)
{
    uint32_t count = hpu_at_load_u32(&g_reg.count, HPU_MO_ACQUIRE);
    uint32_t i;

    if (category == NULL) {
        category = "*";
        len = 1;
    }
    for (i = 0; i < count; i++) {
        hpu_reg_entry_t* slot = &g_reg.entries[i];

        if (slot->name_len == len && memcmp(slot->name, category, len) == 0) {
            return slot;
        }
    }
    return NULL;
}

hpu_reg_entry_t* hpu_registry_lookup(const char* category, size_t len)
{
    hpu_reg_entry_t* slot;

    if (category == NULL) {
        category = "*";
        len = 1;
    }

    slot = hpu_registry_find(category, len);
    if (slot != NULL) {
        return slot;
    }

#if !HPULOGC_ENABLE_CATEGORY
    /* CATEGORY=OFF builds do not route by name; nothing to register. */
    return NULL;
#else
    if (len >= HPU_REG_NAME_LEN) {
        /* Over-long categories take the uncached slow path. */
        return NULL;
    }

    hpu_mutex_lock(&g_reg.mu);
    /* Double-check under the mutex (another thread may have registered). */
    slot = hpu_registry_find(category, len);
    if (slot == NULL) {
        uint32_t count = hpu_at_load_u32(&g_reg.count, HPU_MO_RELAXED);

        if (count < HPU_REG_MAX) {
            hpu_reg_entry_t* fresh = &g_reg.entries[count];

            memcpy(fresh->name, category, len);
            fresh->name[len] = '\0';
            fresh->name_len = (uint32_t)len;
            fresh->hash = fnv1a(category, len);
            fresh->level_override = HPU_REG_NO_OVERRIDE;
            fresh->gen = 0; /* forces a routing recompute */
            memset(fresh->rule_for_level, -1, sizeof(fresh->rule_for_level));
            /* Publish: slot content is complete before the count bump. */
            hpu_at_store_u32(&g_reg.count, count + 1, HPU_MO_RELEASE);
            slot = fresh;
        } else {
            /* Table full: warn once per rejected category (spec 4.9). */
            uint32_t hash = fnv1a(category, len);
            size_t w;
            int seen = 0;

            for (w = 0; w < g_reg.warned_count; w++) {
                if (g_reg.warned_hash[w] == hash) {
                    seen = 1;
                    break;
                }
            }
            if (!seen && g_reg.warned_count < HPU_REG_MAX) {
                g_reg.warned_hash[g_reg.warned_count++] = hash;
                fprintf(stderr,
                        "hpulogc: warning: category registry full; '%.*s' "
                        "routes by the fallback rule only\n",
                        (int)(len > 64 ? 64 : len), category);
            }
        }
    }
    hpu_mutex_unlock(&g_reg.mu);
    return slot;
#endif
}

void hpu_registry_refresh_route(hpu_reg_entry_t* slot,
                                const hpu_conf_t* conf, uint32_t gen)
{
    int level;

    if (slot->gen == gen) {
        return; /* cache is current */
    }
    for (level = 0; level < 7; level++) {
        size_t i;

        slot->rule_for_level[level] = -1;
        if (level > HPULOGC_LEVEL_FATAL) {
            continue; /* OFF is a threshold, never routed */
        }
        for (i = 0; i < conf->rule_count; i++) {
            const hpu_conf_rule_t* r = &conf->rules[i];

            if (r->min_level <= level && level <= r->max_level &&
                hpu_pipeline_selector_match(r->category, slot->name,
                                            slot->name_len)) {
                slot->rule_for_level[level] = (int)i;
                break;
            }
        }
    }
    slot->gen = gen;
}

int hpu_registry_set_override(const char* category, hpulogc_level_t level)
{
    size_t len = strlen(category);
    hpu_reg_entry_t* slot;

    if (len == 0) {
        category = "*";
        len = 1;
    }

    slot = hpu_registry_lookup(category, len);
    if (slot == NULL) {
        /* Unregistrable (too long) or registry full. */
        if (len < HPU_REG_NAME_LEN) {
            return HPULOGC_ERR_CONFIG;
        }
        return HPULOGC_ERR_INVALID_ARG;
    }
    /* Byte store; readers treat it as advisory and re-read per record. */
    if (level == (hpulogc_level_t)-1) {
        slot->level_override = HPU_REG_NO_OVERRIDE;
    } else {
        slot->level_override = (uint8_t)level;
    }
    return 0;
}
