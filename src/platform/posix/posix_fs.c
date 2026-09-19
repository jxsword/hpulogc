/**
 * @file posix_fs.c
 * @brief POSIX implementation of the file/directory contract.
 */

#include "platform/platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int hpu_fs_open_append(const char* path, unsigned mode)
{
    struct stat st;
    int created;
    int fd;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* Apply permission bits only at creation time; existing files keep
     * their current mode (matches zlog-style behavior). */
    created = (stat(path, &st) != 0);

    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, (mode_t)mode);
    if (fd < 0) {
        return -1;
    }

    if (created && fchmod(fd, (mode_t)mode) != 0) {
        /* Non-fatal: keep the descriptor; the umask already applied. */
    }
    return fd;
}

int hpu_fs_write(int fd, const void* buf, size_t len)
{
    const char* p = buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int hpu_fs_sync(int fd)
{
    int rc;

    do {
        rc = fsync(fd);
    } while (rc != 0 && errno == EINTR);

    return rc;
}

void hpu_fs_close(int fd)
{
    (void)close(fd);
}

int hpu_fs_rename(const char* from, const char* to)
{
    return rename(from, to);
}

int hpu_fs_unlink(const char* path)
{
    return unlink(path);
}

hpu_fs_kind_t hpu_fs_stat_kind(const char* path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return HPU_FS_MISSING;
    }
    if (S_ISREG(st.st_mode)) {
        return HPU_FS_REGULAR;
    }
    if (S_ISDIR(st.st_mode)) {
        return HPU_FS_DIR;
    }
    return HPU_FS_OTHER;
}

int64_t hpu_fs_size(int fd)
{
    struct stat st;

    if (fstat(fd, &st) != 0) {
        return -1;
    }
    return (int64_t)st.st_size;
}

int hpu_fs_fchmod(int fd, unsigned mode)
{
    return fchmod(fd, (mode_t)mode);
}

int hpu_fs_mkdir(const char* path, unsigned mode)
{
    if (mkdir(path, (mode_t)mode) == 0) {
        return 0;
    }
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            return 0;
        }
    }
    return -1;
}

int hpu_fs_mkdir_all(const char* path, unsigned mode)
{
    char buf[HPULOGC_MAX_PATH_LEN];
    size_t len;
    size_t i;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    len = strlen(path);
    if (len >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, len + 1);

    /* Strip trailing slashes so the last component is handled normally. */
    while (len > 1 && buf[len - 1] == '/') {
        buf[--len] = '\0';
    }

    for (i = 1; i < len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (hpu_fs_mkdir(buf, mode) != 0) {
                return -1;
            }
            buf[i] = '/';
        }
    }
    return hpu_fs_mkdir(buf, mode);
}

int hpu_fs_isatty(int fd)
{
    return isatty(fd);
}

int hpu_fs_symlink(const char* target, const char* linkpath)
{
    return symlink(target, linkpath);
}

int hpu_fs_list_dir(const char* dir, hpu_dir_entry_t** out, size_t* count)
{
    DIR* dp;
    struct dirent* de;
    hpu_dir_entry_t* entries = NULL;
    size_t n = 0;
    size_t cap = 0;

    if (out == NULL || count == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out   = NULL;
    *count = 0;

    dp = opendir(dir);
    if (dp == NULL) {
        return -1;
    }

    while ((de = readdir(dp)) != NULL) {
        char* copy;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (n == cap) {
            size_t new_cap = cap == 0 ? 16 : cap * 2;
            hpu_dir_entry_t* grown =
                realloc(entries, new_cap * sizeof(*grown));
            if (grown == NULL) {
                goto fail;
            }
            entries = grown;
            cap = new_cap;
        }
        copy = strdup(de->d_name);
        if (copy == NULL) {
            goto fail;
        }
        entries[n].name = copy;
        n++;
    }
    (void)closedir(dp);

    *out   = entries;
    *count = n;
    return 0;

fail:
    (void)closedir(dp);
    hpu_fs_list_free(entries, n);
    return -1;
}

void hpu_fs_list_free(hpu_dir_entry_t* entries, size_t count)
{
    size_t i;

    if (entries == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(entries[i].name);
    }
    free(entries);
}
