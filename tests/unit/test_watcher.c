/**
 * @file test_watcher.c
 * @brief Platform watcher contract (spec rd_v0.6 §4.5): baseline
 *        semantics, timeout, modify/delete/atomic-replace detection and
 *        idempotent stop, exercised against whatever backend the host
 *        provides (inotify / kqueue / ReadDirectoryChangesW / polling).
 *
 * Runs on every platform matrix so a backend regression (e.g. the
 * Windows RDC backend) fails CI instead of surfacing as a hot-reload
 * behavior drift.
 */

#include "test_util.h"
#include "platform/hpu_watcher.h"

#include <stdio.h>
#include <string.h>

/** @brief Watched file name (created in the test working directory). */
#define WATCHED_FILE "hpu_test_watcher.log"
/** @brief Staging file name for the atomic-replace scenario. */
#define STAGING_FILE "hpu_test_watcher.staging"
/** @brief Generous upper bound for asynchronous notification delivery. */
#define DETECT_TIMEOUT_MS 5000U

/**
 * @brief (Re)write a small text file with fresh mtime/size.
 */
static void write_file(const char* path, const char* text)
{
    FILE* fp = fopen(path, "w");

    if (fp != NULL) {
        fputs(text, fp);
        fclose(fp);
    }
}

TEST(watcher_no_change_times_out)
{
    hpu_watcher_t w;

    write_file(WATCHED_FILE, "one\n");
    memset(&w, 0, sizeof(w));
    CHECK(hpu_watcher_start(&w, WATCHED_FILE) == 0);
    /* baseline taken at start: an untouched file must not report */
    CHECK(hpu_watcher_wait(&w, 200) == 0);
    hpu_watcher_stop(&w);
    (void)remove(WATCHED_FILE);
}

TEST(watcher_modify_detected)
{
    hpu_watcher_t w;

    write_file(WATCHED_FILE, "one\n");
    memset(&w, 0, sizeof(w));
    CHECK(hpu_watcher_start(&w, WATCHED_FILE) == 0);
    write_file(WATCHED_FILE, "one\ntwo\n");
    CHECK(hpu_watcher_wait(&w, DETECT_TIMEOUT_MS) == 1);
    /* the detected change refreshed the baseline */
    CHECK(hpu_watcher_wait(&w, 200) == 0);
    hpu_watcher_stop(&w);
    (void)remove(WATCHED_FILE);
}

TEST(watcher_delete_detected)
{
    hpu_watcher_t w;

    write_file(WATCHED_FILE, "one\n");
    memset(&w, 0, sizeof(w));
    CHECK(hpu_watcher_start(&w, WATCHED_FILE) == 0);
    (void)remove(WATCHED_FILE);
    /* a vanished watched file counts as a change (reload path reports
     * the parse error) */
    CHECK(hpu_watcher_wait(&w, DETECT_TIMEOUT_MS) == 1);
    hpu_watcher_stop(&w);
}

TEST(watcher_atomic_replace_detected)
{
    hpu_watcher_t w;

    write_file(WATCHED_FILE, "old\n");
    memset(&w, 0, sizeof(w));
    CHECK(hpu_watcher_start(&w, WATCHED_FILE) == 0);
    write_file(STAGING_FILE, "brand new content\n");
    /* POSIX rename() replaces the target; Windows rename() fails when
     * the target exists, so remove first — the transient absence is
     * itself a change the watcher must report. */
    (void)remove(WATCHED_FILE);
    CHECK(rename(STAGING_FILE, WATCHED_FILE) == 0);
    CHECK(hpu_watcher_wait(&w, DETECT_TIMEOUT_MS) == 1);
    hpu_watcher_stop(&w);
    (void)remove(WATCHED_FILE);
}

TEST(watcher_stop_idempotent_and_restart)
{
    hpu_watcher_t w;

    write_file(WATCHED_FILE, "one\n");
    memset(&w, 0, sizeof(w));
    CHECK(hpu_watcher_start(&w, WATCHED_FILE) == 0);
    hpu_watcher_stop(&w);
    hpu_watcher_stop(&w);
    /* a stopped handle must be reusable */
    CHECK(hpu_watcher_start(&w, WATCHED_FILE) == 0);
    CHECK(hpu_watcher_wait(&w, 200) == 0);
    hpu_watcher_stop(&w);
    (void)remove(WATCHED_FILE);
}
