/**
 * @file hpu_fs.h
 * @brief Platform contract: files, directories, permissions and rotation
 *        primitives (open/write/fsync/rename/unlink/dir scan/symlink).
 *
 * Rotation needs exactly the operations listed here; the portable rotate
 * logic in src/output/ composes them and must not touch OS headers.
 */

#ifndef HPU_FS_H
#define HPU_FS_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief File/directory kind returned by hpu_fs_stat_kind().
 */
typedef enum {
    HPU_FS_MISSING = 0, /*!< Path does not exist */
    HPU_FS_REGULAR = 1, /*!< Regular file */
    HPU_FS_DIR     = 2, /*!< Directory */
    HPU_FS_OTHER   = 3  /*!< FIFO, device, socket, symlink-to-non-file, ... */
} hpu_fs_kind_t;

/**
 * @brief Directory listing entry.
 */
typedef struct hpu_dir_entry {
    char* name; /*!< Entry file name (no directory part), owned by the array */
} hpu_dir_entry_t;

/**
 * @brief Open (and create) a file for appending writes.
 *
 * The file is opened write-only with O_APPEND semantics; @p mode applies
 * only when the file is created. The descriptor is unbuffered; all writes
 * go through hpu_fs_write().
 *
 * @param path  Target file path.
 * @param mode  Permission bits applied at creation (POSIX octal, e.g. 0644).
 * @return      Descriptor on success, -1 on failure (errno set).
 */
int hpu_fs_open_append(const char* path, unsigned mode);

/**
 * @brief Write the whole buffer to a descriptor.
 *
 * Loops over partial writes. Must not be interrupted semantics-wise:
 * returns only after all bytes are written or a hard error occurs.
 *
 * @param fd   Descriptor.
 * @param buf  Data to write.
 * @param len  Number of bytes.
 * @return     0 on success, -1 on failure (errno set).
 */
int hpu_fs_write(int fd, const void* buf, size_t len);

/**
 * @brief Flush OS buffers to stable storage (fsync).
 * @param fd  Descriptor.
 * @return    0 on success, -1 on failure (errno set).
 */
int hpu_fs_sync(int fd);

/**
 * @brief Close a descriptor.
 * @param fd  Descriptor.
 */
void hpu_fs_close(int fd);

/**
 * @brief Rename a file (rotation primitive; replaces the target on POSIX).
 * @param from  Existing path.
 * @param to    New path.
 * @return      0 on success, -1 on failure (errno set).
 */
int hpu_fs_rename(const char* from, const char* to);

/**
 * @brief Remove a file (rotation cleanup primitive).
 * @param path  File to remove.
 * @return      0 on success, -1 on failure (errno set).
 */
int hpu_fs_unlink(const char* path);

/**
 * @brief Stat a path and classify it.
 * @param path  Path to inspect.
 * @return      hpu_fs_kind_t classification.
 */
hpu_fs_kind_t hpu_fs_stat_kind(const char* path);

/**
 * @brief Identity of an open file, stable across renames.
 *
 * Used by the throttled external-rotation check: comparing the identity
 * recorded at open time with a fresh read detects that the path now refers
 * to a different file (logrotate-style mv + recreate).
 */
typedef struct hpu_fs_file_id {
    uint64_t dev; /*!< Device id (volume serial on Windows) */
    uint64_t ino; /*!< Inode / file index */
} hpu_fs_file_id_t;

/**
 * @brief Read the identity of an open file (throttled fstat primitive).
 *
 * Two descriptors (or a descriptor and a later re-open of the same path)
 * refer to the same file iff both dev and ino compare equal.
 *
 * @param fd   Descriptor.
 * @param out  Filled with the file identity on success.
 * @return     0 on success, -1 on failure (errno set).
 *
 * @note On filesystems without stable per-file ids (e.g. FAT), the index
 *       may be synthesized; callers treat a failed read as "unknown" and
 *       skip the comparison rather than assuming a change.
 */
int hpu_fs_fstat_id(int fd, hpu_fs_file_id_t* out);

/**
 * @brief Read the identity of a file by path (external-rotation check).
 *
 * The external-rotation detector compares this with the identity recorded
 * (via hpu_fs_fstat_id) when the descriptor was opened: a differing path
 * identity means the path now refers to a different file. Renames of the
 * open file itself do not change the descriptor's identity — only a
 * path-based read can reveal the replacement.
 *
 * @param path  Path to inspect.
 * @param out   Filled with the file identity on success.
 * @return      0 on success, -1 on failure (errno set; missing path).
 */
int hpu_fs_stat_id(const char* path, hpu_fs_file_id_t* out);

/**
 * @brief Current size of an open file (O(1), no reopen).
 * @param fd  Descriptor.
 * @return    Size in bytes, -1 on failure.
 */
int64_t hpu_fs_size(int fd);

/**
 * @brief Change permissions of an open file.
 *
 * Ignored on Windows (Phase 2 returns 0 with a one-time warning upstream).
 *
 * @param fd    Descriptor.
 * @param mode  Permission bits (POSIX octal).
 * @return      0 on success, -1 on failure.
 */
int hpu_fs_fchmod(int fd, unsigned mode);

/**
 * @brief Create a directory (single component, like mkdir(2)).
 * @param path  Directory path.
 * @param mode  Permission bits applied at creation.
 * @return      0 on success (including "already exists"), -1 on failure.
 */
int hpu_fs_mkdir(const char* path, unsigned mode);

/**
 * @brief Create a directory hierarchy (parents as needed).
 * @param path  Directory path; intermediate components are created with
 *              @p mode.
 * @param mode  Permission bits.
 * @return      0 on success, -1 on failure.
 */
int hpu_fs_mkdir_all(const char* path, unsigned mode);

/**
 * @brief Check whether a descriptor refers to a terminal device.
 * @param fd  Descriptor.
 * @return    Non-zero when the fd is a terminal.
 */
int hpu_fs_isatty(int fd);

/**
 * @brief Create a symbolic link (rotation .latest support).
 * @param target    Link target (stored verbatim).
 * @param linkpath  Link path to create.
 * @return          0 on success, -1 on failure.
 */
int hpu_fs_symlink(const char* target, const char* linkpath);

/**
 * @brief List the file names inside a directory (rotation cleanup scan).
 *
 * Only regular directory entries are returned (no "." / ".."). Order is
 * unspecified; callers sort as needed.
 *
 * @param dir     Directory path.
 * @param out     Filled with a malloc'd array of entries (names malloc'd).
 * @param count   Filled with the number of entries.
 * @return        0 on success (possibly 0 entries), -1 on failure.
 */
int hpu_fs_list_dir(const char* dir, hpu_dir_entry_t** out, size_t* count);

/**
 * @brief Free an array returned by hpu_fs_list_dir().
 * @param entries  Array pointer (may be NULL).
 * @param count    Number of entries.
 */
void hpu_fs_list_free(hpu_dir_entry_t* entries, size_t count);

#endif /* HPU_FS_H */
