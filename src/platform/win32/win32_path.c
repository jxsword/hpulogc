/* -*- coding: utf-8 -*- */

/**
 * @file win32_path.c
 * @brief Win32 implementation of the lexical path helpers.
 *
 * Windows accepts both separators; dirname/basename/stem treat '/' and
 * '\\' equally and preserve the input's separator style. normalize()
 * canonicalizes to forward slashes and folds ASCII case, so duplicate
 * file-output detection (spec 10.4) is separator- and case-insensitive.
 */

#include "platform/platform.h"

#include <string.h>

/**
 * @brief Find the last path separator ('/' or '\\').
 */
static const char* last_separator(const char* path)
{
    const char* s1 = strrchr(path, '/');
    const char* s2 = strrchr(path, '\\');

    if (s1 == NULL) {
        return s2;
    }
    if (s2 == NULL) {
        return s1;
    }
    return s1 > s2 ? s1 : s2;
}

int hpu_path_dirname(const char* path, char* out, size_t outsz)
{
    const char* sep;
    size_t len;

    if (path == NULL || out == NULL || outsz == 0) {
        return -1;
    }

    sep = last_separator(path);
    if (sep == NULL) {
        if (outsz < 2) {
            return -1;
        }
        out[0] = '.';
        out[1] = '\0';
        return 0;
    }

    len = (size_t)(sep - path);
    if (len == 0) {
        len = 1; /* root directory "/" or "\" */
    }
    if (len + 1 > outsz) {
        return -1;
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return 0;
}

int hpu_path_basename(const char* path, char* out, size_t outsz)
{
    const char* sep;
    const char* base;
    size_t len;

    if (path == NULL || out == NULL || outsz == 0) {
        return -1;
    }

    sep = last_separator(path);
    base = sep == NULL ? path : sep + 1;
    len  = strlen(base);
    if (len + 1 > outsz) {
        return -1;
    }
    memcpy(out, base, len + 1);
    return 0;
}

int hpu_path_stem(const char* path, char* out, size_t outsz)
{
    char base[HPULOGC_MAX_PATH_LEN];
    char* dot;

    if (hpu_path_basename(path, base, sizeof(base)) != 0) {
        return -1;
    }

    dot = strrchr(base, '.');
    if (dot != NULL && dot != base) { /* keep dotfiles like ".log" intact */
        *dot = '\0';
    }
    if (strlen(base) + 1 > outsz) {
        return -1;
    }
    memcpy(out, base, strlen(base) + 1);
    return 0;
}

/**
 * @brief Push one path component onto the normalization stack.
 */
static int path_push(const char** comps, size_t* lens, size_t* depth,
                     const char* seg, size_t seg_len)
{
    if (*depth >= 128) {
        return -1;
    }
    comps[*depth] = seg;
    lens[*depth]  = seg_len;
    (*depth)++;
    return 0;
}

int hpu_path_normalize(const char* path, char* out, size_t outsz)
{
    const char* src = path;
    const char* comps[128];
    size_t lens[128];
    size_t depth = 0;
    size_t o = 0;
    int absolute;
    size_t i;

    if (path == NULL || out == NULL || outsz == 0) {
        return -1;
    }

    /* Absolute: drive prefix ("X:"), or a leading separator. */
    absolute = src[0] == '/' || src[0] == '\\';
    if (!absolute && ((path[0] >= 'A' && path[0] <= 'Z') ||
                      (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':') {
        absolute = 1;
    }

    while (*src != '\0') {
        const char* seg;
        size_t seg_len;

        while (*src == '/' || *src == '\\') {
            src++;
        }
        if (*src == '\0') {
            break;
        }
        seg = src;
        while (*src != '\0' && *src != '/' && *src != '\\') {
            src++;
        }
        seg_len = (size_t)(src - seg);

        if (seg_len == 1 && seg[0] == '.') {
            continue;
        }
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (depth > 0) {
                depth--; /* pop (never above the root: absolute kept) */
            } else if (!absolute) {
                /* Relative path above its base keeps the ".." literally. */
                if (path_push(comps, lens, &depth, seg, seg_len) != 0) {
                    return -1;
                }
            }
            continue;
        }
        if (path_push(comps, lens, &depth, seg, seg_len) != 0) {
            return -1;
        }
    }

    /* Drive-letter prefix passes through verbatim (case preserved). */
    if (absolute && !((path[0] == '/' || path[0] == '\\'))) {
        if (o + 2 >= outsz) {
            return -1;
        }
        out[o++] = path[0];
        out[o++] = ':';
    } else if (absolute) {
        if (o + 1 >= outsz) {
            return -1;
        }
        out[o++] = '/';
    }
    for (i = 0; i < depth; i++) {
        if (o + 1 + lens[i] >= outsz) {
            return -1;
        }
        out[o++] = '/';
        memcpy(out + o, comps[i], lens[i]);
        o += lens[i];
    }
    if (o == 0) {
        if (outsz < 2) {
            return -1;
        }
        out[o++] = '.';
    }
    out[o] = '\0';

    /* Fold ASCII case for the duplicate-path comparison use case. */
    for (i = 0; out[i] != '\0'; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') {
            out[i] = (char)(out[i] - 'A' + 'a');
        }
    }
    return 0;
}
