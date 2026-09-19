/**
 * @file hpu_path.h
 * @brief Platform contract: lexical path helpers (no filesystem access).
 */

#ifndef HPU_PATH_H
#define HPU_PATH_H

#include <stddef.h>

/**
 * @brief Copy the directory part of a path ("a/b/c" -> "a/b"; "c" -> ".").
 *
 * @param path  Input path.
 * @param out   Output buffer, always NUL-terminated on success.
 * @param outsz Buffer size.
 * @return      0 on success, -1 when the buffer is too small or input NULL.
 */
int hpu_path_dirname(const char* path, char* out, size_t outsz);

/**
 * @brief Copy the final component of a path ("a/b/c" -> "c").
 *
 * @param path  Input path.
 * @param out   Output buffer, always NUL-terminated on success.
 * @param outsz Buffer size.
 * @return      0 on success, -1 when the buffer is too small or input NULL.
 */
int hpu_path_basename(const char* path, char* out, size_t outsz);

/**
 * @brief Copy the file name without its final extension ("app.log" ->
 *        "app"; "app" -> "app"; "a.b/c" -> "c").
 *
 * This feeds the {base} placeholder of rotation naming templates.
 *
 * @param path  Input path (file name or full path).
 * @param out   Output buffer, always NUL-terminated on success.
 * @param outsz Buffer size.
 * @return      0 on success, -1 when the buffer is too small or input NULL.
 */
int hpu_path_stem(const char* path, char* out, size_t outsz);

/**
 * @brief Lexically normalize a path (collapse "//", "/./" and "/../").
 *
 * Purely lexical: symlinks are not resolved. Used to detect duplicate file
 * output paths at init (spec 10.4).
 *
 * @param path  Input path.
 * @param out   Output buffer, always NUL-terminated on success.
 * @param outsz Buffer size.
 * @return      0 on success, -1 when the buffer is too small or input NULL.
 */
int hpu_path_normalize(const char* path, char* out, size_t outsz);

#endif /* HPU_PATH_H */
