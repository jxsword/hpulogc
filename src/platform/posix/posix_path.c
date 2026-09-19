/**
 * @file posix_path.c
 * @brief POSIX implementation of lexical path helpers ('/' separators).
 */

#include "platform/platform.h"

#include <string.h>

int hpu_path_dirname(const char* path, char* out, size_t outsz)
{
    const char* last_slash;
    size_t len;

    if (path == NULL || out == NULL || outsz == 0) {
        return -1;
    }

    last_slash = strrchr(path, '/');
    if (last_slash == NULL) {
        if (outsz < 2) {
            return -1;
        }
        out[0] = '.';
        out[1] = '\0';
        return 0;
    }

    len = (size_t)(last_slash - path);
    if (len == 0) {
        len = 1; /* root directory "/" */
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
    const char* last_slash;
    const char* base;
    size_t len;

    if (path == NULL || out == NULL || outsz == 0) {
        return -1;
    }

    last_slash = strrchr(path, '/');
    base = last_slash == NULL ? path : last_slash + 1;
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
 *
 * @param comps     Component pointer stack.
 * @param lens      Component lengths.
 * @param depth     Current depth (in/out).
 * @param seg       Component start.
 * @param seg_len   Component length.
 * @return          0 on success, -1 when the stack overflows.
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

    absolute = (src[0] == '/');

    while (*src != '\0') {
        const char* seg;
        size_t seg_len;

        while (*src == '/') {
            src++;
        }
        if (*src == '\0') {
            break;
        }
        seg = src;
        while (*src != '\0' && *src != '/') {
            src++;
        }
        seg_len = (size_t)(src - seg);

        if (seg_len == 1 && seg[0] == '.') {
            continue;
        }
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (depth > 0) {
                depth--; /* pop (never above root: leading "/" kept) */
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

    if (absolute) {
        if (o + 1 >= outsz) {
            return -1;
        }
        out[o++] = '/';
    }
    for (i = 0; i < depth; i++) {
        if (i > 0 || (o > 0 && !(absolute && o == 1))) {
            if (o + 1 >= outsz) {
                return -1;
            }
            out[o++] = '/';
        }
        if (o + lens[i] >= outsz) {
            return -1;
        }
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
    return 0;
}
