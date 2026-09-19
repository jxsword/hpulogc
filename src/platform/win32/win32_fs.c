/* -*- coding: utf-8 -*- */

/**
 * @file win32_fs.c
 * @brief Win32 implementation of the file/directory contract.
 *
 * Design notes:
 *  - Every path parameter is UTF-8 and converted to UTF-16 before the W
 *    API is called (spec 2/12: UTF-8 everywhere).
 *  - hpu_fs_open_append() maps O_CREAT|O_APPEND to CreateFileW with
 *    OPEN_ALWAYS followed by a seek to EOF: with FILE_GENERIC_WRITE the
 *    auto-append access mode cannot be combined with FlushFileBuffers
 *    (fsync), and the library serializes writers per output anyway.
 *  - Permission bits are ignored with a one-time stderr warning (spec
 *    4.7); hpu_fs_fchmod() is a successful no-op.
 *  - hpu_fs_symlink() always fails: .latest needs privileges on Windows
 *    and the configuration is ignored upstream with its own warning.
 *  - hpu_fs_isatty() additionally switches fd 1/2 to CRT binary mode
 *    (no \n -> \r\n translation, so forced LF works) and enables
 *    ENABLE_VIRTUAL_TERMINAL_PROCESSING for ANSI colors (spec 16.2).
 *    Without VT support it reports "not a terminal" so the color layer
 *    downgrades cleanly.
 */

#include "platform/platform.h"

#include "../../atomic/hpulogc_atomic.h"

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* ------------------------------------------------------------------ */
/* UTF-8 <-> UTF-16 conversion                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Convert a UTF-8 path to UTF-16 into a fixed buffer.
 * @return 0 on success, -1 on conversion failure or overflow (errno set).
 */
static int utf8_to_utf16(const char* src, wchar_t* dst, size_t dst_units)
{
    int n;

    if (src == NULL) {
        errno = EINVAL;
        return -1;
    }
    n = MultiByteToWideChar(CP_UTF8, 0, src, -1, NULL, 0);
    if (n <= 0 || (size_t)n > dst_units) {
        errno = EINVAL;
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, n);
    return 0;
}

/**
 * @brief Convert a UTF-16 string to a malloc'd UTF-8 string.
 * @return Heap string or NULL on failure (errno set).
 */
static char* utf16_to_utf8(const wchar_t* src)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, NULL, 0, NULL, NULL);
    char* out;

    if (n <= 0) {
        return NULL;
    }
    out = malloc((size_t)n);
    if (out == NULL) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, src, -1, out, n, NULL, NULL) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* One-time permission warnings (spec 4.7)                             */
/* ------------------------------------------------------------------ */

/** @brief 1 = file-permission warning already emitted. */
static hpu_atomic_u32 g_warned_file_perms;
/** @brief 1 = directory-permission warning already emitted. */
static hpu_atomic_u32 g_warned_dir_perms;

/**
 * @brief Emit a warning once per process.
 * @return The warning text when it was emitted, NULL afterwards.
 */
static const char* warn_once(hpu_atomic_u32* flag, const char* text)
{
    uint32_t expected = 0;

    if (hpu_at_cas_u32(flag, &expected, 1, HPU_MO_ACQ_REL, HPU_MO_ACQUIRE)) {
        return text;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Contract implementation                                             */
/* ------------------------------------------------------------------ */

int hpu_fs_open_append(const char* path, unsigned mode)
{
    wchar_t wpath[HPULOGC_MAX_PATH_LEN];
    DWORD access = FILE_GENERIC_WRITE;
    HANDLE h;
    int fd;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (utf8_to_utf16(path, wpath, HPULOGC_MAX_PATH_LEN) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (mode != 0) {
        const char* w = warn_once(&g_warned_file_perms,
                                  "hpulogc: warning: file permissions are "
                                  "ignored on this platform\n");

        if (w != NULL) {
            fputs(w, stderr);
        }
    }

    /* OPEN_ALWAYS: create when missing, keep existing content (matches
     * O_CREAT|O_APPEND without O_TRUNC). */
    h = CreateFileW(wpath, access, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EIO;
        return -1;
    }
    /* Append semantics: continue at the current end of file. */
    (void)SetFilePointer(h, 0, NULL, FILE_END);

    fd = _open_osfhandle((intptr_t)h, 0);
    if (fd < 0) {
        (void)CloseHandle(h);
        errno = EMFILE;
        return -1;
    }
    return fd;
}

int hpu_fs_write(int fd, const void* buf, size_t len)
{
    const char* p = buf;
    HANDLE h;

    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    while (len > 0) {
        DWORD written = 0;
        DWORD chunk = len > 0x40000000U ? 0x40000000U : (DWORD)len;

        if (!WriteFile(h, p, chunk, &written, NULL) || written == 0) {
            errno = EIO;
            return -1;
        }
        p += written;
        len -= written;
    }
    return 0;
}

int hpu_fs_sync(int fd)
{
    HANDLE h;

    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (!FlushFileBuffers(h)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

void hpu_fs_close(int fd)
{
    if (fd >= 0) {
        (void)_close(fd);
    }
}

int hpu_fs_rename(const char* from, const char* to)
{
    wchar_t wfrom[HPULOGC_MAX_PATH_LEN];
    wchar_t wto[HPULOGC_MAX_PATH_LEN];

    if (utf8_to_utf16(from, wfrom, HPULOGC_MAX_PATH_LEN) != 0 ||
        utf8_to_utf16(to, wto, HPULOGC_MAX_PATH_LEN) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (!MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int hpu_fs_unlink(const char* path)
{
    wchar_t wpath[HPULOGC_MAX_PATH_LEN];

    if (utf8_to_utf16(path, wpath, HPULOGC_MAX_PATH_LEN) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (!DeleteFileW(wpath)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

hpu_fs_kind_t hpu_fs_stat_kind(const char* path)
{
    wchar_t wpath[HPULOGC_MAX_PATH_LEN];
    DWORD attrs;

    if (path == NULL ||
        utf8_to_utf16(path, wpath, HPULOGC_MAX_PATH_LEN) != 0) {
        return HPU_FS_MISSING;
    }
    attrs = GetFileAttributesW(wpath);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return HPU_FS_MISSING;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        return HPU_FS_DIR;
    }
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        return HPU_FS_OTHER;
    }
    return HPU_FS_REGULAR;
}

int64_t hpu_fs_size(int fd)
{
    HANDLE h;
    LARGE_INTEGER sz;

    if (fd < 0) {
        return -1;
    }
    h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &sz)) {
        return -1;
    }
    return (int64_t)sz.QuadPart;
}

int hpu_fs_fchmod(int fd, unsigned mode)
{
    /* Permissions are not enforced on Windows; the caller already warned
     * once when the file was opened (spec 4.7). */
    (void)fd;
    (void)mode;
    return 0;
}

int hpu_fs_mkdir(const char* path, unsigned mode)
{
    wchar_t wpath[HPULOGC_MAX_PATH_LEN];

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (mode != 0) {
        const char* w = warn_once(&g_warned_dir_perms,
                                  "hpulogc: warning: directory permissions "
                                  "are ignored on this platform\n");

        if (w != NULL) {
            fputs(w, stderr);
        }
    }
    if (utf8_to_utf16(path, wpath, HPULOGC_MAX_PATH_LEN) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (CreateDirectoryW(wpath, NULL)) {
        return 0;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS &&
        hpu_fs_stat_kind(path) == HPU_FS_DIR) {
        return 0; /* already exists (spec: success) */
    }
    errno = EIO;
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

    /* Strip trailing separators so the last component is handled normally. */
    while (len > 1 && (buf[len - 1] == '/' || buf[len - 1] == '\\')) {
        buf[--len] = '\0';
    }

    /* Walk the components, creating each prefix. Intermediate failures
     * are tolerated when the prefix already exists as a directory (this
     * also skips drive roots like "C:" and UNC shares gracefully). */
    for (i = 1; i < len; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char sep = buf[i];

            buf[i] = '\0';
            if (hpu_fs_mkdir(buf, mode) != 0 &&
                hpu_fs_stat_kind(buf) != HPU_FS_DIR) {
                buf[i] = sep;
                return -1;
            }
            buf[i] = sep;
        }
    }
    if (hpu_fs_mkdir(buf, mode) != 0 &&
        hpu_fs_stat_kind(buf) != HPU_FS_DIR) {
        return -1;
    }
    return 0;
}

/**
 * @brief Console/terminal check with VT enable and binary-mode side
 *        effects (see the file header; spec 4.7/16.2/12).
 *
 * The binary-mode switch must run before the isatty verdict: the CRT
 * text mode translates \n -> \r\n on ANY non-binary stdio stream (pipes
 * and files included), which would turn the library's rendered \r\n
 * lines into \r\r\n. Forced-LF logs have the same requirement.
 */
static int console_terminal_check(int fd)
{
    static int streams_prepared = 0;
    HANDLE h;

    if ((fd == 1 || fd == 2) && !streams_prepared) {
        streams_prepared = 1;
        (void)_setmode(1, _O_BINARY);
        (void)_setmode(2, _O_BINARY);
    }

    if (!_isatty(fd)) {
        return 0;
    }

    h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    {
        DWORD mode = 0;

        if (!GetConsoleMode(h, &mode)) {
            return 0; /* pipe/device with a tty fd: no ANSI colors */
        }
        if (!SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            return 0; /* pre-VT console: color layer downgrades */
        }
    }
    return 1;
}

int hpu_fs_isatty(int fd)
{
    return console_terminal_check(fd);
}

int hpu_fs_symlink(const char* target, const char* linkpath)
{
    /* Symlink creation requires privileges on Windows; the .latest
     * feature is ignored upstream with its own one-time warning. */
    (void)target;
    (void)linkpath;
    errno = ENOSYS;
    return -1;
}

int hpu_fs_list_dir(const char* dir, hpu_dir_entry_t** out, size_t* count)
{
    wchar_t wdir[HPULOGC_MAX_PATH_LEN];
    wchar_t pattern[HPULOGC_MAX_PATH_LEN + 4];
    WIN32_FIND_DATAW fd;
    HANDLE find = INVALID_HANDLE_VALUE;
    hpu_dir_entry_t* entries = NULL;
    size_t n = 0;
    size_t cap = 0;
    size_t dirlen;

    if (out == NULL || count == NULL || dir == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out   = NULL;
    *count = 0;

    if (utf8_to_utf16(dir, wdir, HPULOGC_MAX_PATH_LEN) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    dirlen = wcslen(wdir);
    if (dirlen > 0 && (wdir[dirlen - 1] == L'/' || wdir[dirlen - 1] == L'\\')) {
        _snwprintf_s(pattern, sizeof(pattern) / sizeof(pattern[0]),
                     _TRUNCATE, L"%s*", wdir);
    } else {
        _snwprintf_s(pattern, sizeof(pattern) / sizeof(pattern[0]),
                     _TRUNCATE, L"%s\\*", wdir);
    }
    pattern[(sizeof(pattern) / sizeof(pattern[0])) - 1] = L'\0';

    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        errno = ENOENT;
        return -1;
    }

    for (;;) {
        if (wcscmp(fd.cFileName, L".") != 0 &&
            wcscmp(fd.cFileName, L"..") != 0) {
            char* name = utf16_to_utf8(fd.cFileName);

            if (name == NULL) {
                goto fail;
            }
            if (n == cap) {
                size_t new_cap = cap == 0 ? 16 : cap * 2;
                hpu_dir_entry_t* grown =
                    realloc(entries, new_cap * sizeof(*grown));

                if (grown == NULL) {
                    free(name);
                    goto fail;
                }
                entries = grown;
                cap = new_cap;
            }
            entries[n].name = name;
            n++;
        }
        if (!FindNextFileW(find, &fd)) {
            break;
        }
    }
    (void)FindClose(find);

    *out   = entries;
    *count = n;
    return 0;

fail:
    (void)FindClose(find);
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
