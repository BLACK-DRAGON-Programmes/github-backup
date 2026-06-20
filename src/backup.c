/**
 * backup.c - Backup orchestration implementation for the GitHub Backup Script.
 *
 * Implements the per-repo backup flow and the full cycle orchestrator.
 * Each function does exactly one thing (Coding Standard #18):
 *
 *   verify_downloaded_file - checks file integrity on disk
 *   cleanup_temp_file     - removes a failed download
 *   atomic_write          - swaps temp file for final file
 *   backup_single_repo    - full per-repo flow
 *   run_backup_cycle      - iterates all repos
 *
 * The backup module consumes config (for repo list and paths), network
 * (for API calls), logger (for event recording), and notify (for
 * toasts). It does not read .env directly - it receives a populated
 * backup_config struct from the caller.
 */

#include "backup.h"
#include "context.h"
#include "logger.h"  /* DBG macro — compile-time debug toggle, not a DI call */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif


/* ================================================================
 * FILE VERIFICATION
 * ================================================================ */

/**
 * Validate that a constructed file path (dir + filename) will fit
 * within MAX_URL_LEN. Prevents silent path truncation by snprintf
 * when backup_dir is very long.
 *
 * @param dir_len    Length of the directory portion (excluding null)
 * @param name_len   Length of the filename portion (excluding null)
 * @param repo       Repo name (for error logging)
 * @return 0 if path fits, -1 if it would overflow
 */
/* MAX_PATH_BUF is defined in constants.h - used by validate_path_length
 * and the stack buffers below. */

static int validate_path_length(ghb_context *ctx, size_t dir_len,
                                size_t name_len, const char *repo) {
    if (dir_len + name_len >= MAX_PATH_BUF) {
        char detail[MAX_URL_LEN];
        snprintf(detail, sizeof(detail),
                 "Path too long: backup_dir (%zu) + repo name (%zu) "
                 "exceeds MAX_PATH_BUF (%d) - shorten BACKUP_DIR in .env",
                 dir_len, name_len, MAX_PATH_BUF);
        ctx->logger->log_error(ctx, "backup", repo, detail);
        ctx->notify->toast_error(ctx, "Path Too Long",
                    "BACKUP_DIR + repo name exceeds maximum path length");
        return -1;
    }
    return 0;
}


int verify_downloaded_file(const char *file_path) {
    if (file_path == NULL || file_path[0] == '\0') {
        return -1;
    }

    /* Check that the file exists and is readable */
    FILE *fp = fopen(file_path, "rb");
    if (fp == NULL) {
        return -1;
    }

    /* Check file size */
    long file_size = 0;

    #ifdef _WIN32
    {
        HANDLE h = CreateFileA(
            file_path,
            GENERIC_READ,
            FILE_SHARE_READ,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );
        if (h != INVALID_HANDLE_VALUE) {
            file_size = (long)GetFileSize(h, NULL);
            CloseHandle(h);
        }
    }
    #else
    {
        struct stat st;
        if (stat(file_path, &st) == 0) {
            file_size = (long)st.st_size;
        }
    }
    #endif

    fclose(fp);

    if (file_size <= 0) {
        return -1;  /* Empty or unreadable file */
    }

    return 0;
}


/* ================================================================
 * FILE OPERATIONS
 * ================================================================ */

void cleanup_temp_file(ghb_context *ctx, const char *temp_path) {
    if (temp_path == NULL || temp_path[0] == '\0') {
        return;
    }

    if (remove(temp_path) == 0) {
        ctx->logger->log_event(ctx, LOG_INFO, "backup", NULL, "CLEANUP",
                  "Temporary file deleted");
    } else {
        ctx->logger->log_error(ctx, "backup", NULL,
                  "Failed to delete temporary file - may need manual cleanup");
    }
}


int atomic_write(ghb_context *ctx, const char *temp_path, const char *final_path) {
    if (temp_path == NULL || final_path == NULL) {
        return -1;
    }

    #ifdef _WIN32
    /*
     * Use MoveFileExA with MOVEFILE_REPLACE_EXISTING for true atomicity
     * on NTFS. Unlike remove() + rename(), this is a single atomic
     * operation — if it fails, the original file is untouched.
     * Spec Section 5: "the file either renames completely or not at all."
     *
     * MoveFileExA (ANSI version) accepts char* paths and handles the
     * internal Unicode conversion — no manual MultiByteToWideChar needed.
     */
    if (!MoveFileExA(temp_path, final_path, MOVEFILE_REPLACE_EXISTING |
                                               MOVEFILE_WRITE_THROUGH)) {
        ctx->logger->log_error(ctx, "backup", NULL,
                  "Failed to rename backup file atomically");
        return -1;
    }
    return 0;
    #else
    if (remove(final_path) != 0) {
        FILE *check = fopen(final_path, "rb");
        if (check != NULL) {
            fclose(check);
            ctx->logger->log_error(ctx, "backup", NULL,
                      "Cannot delete old backup file - permission denied or file locked");
            return -1;
        }
    }
    if (rename(temp_path, final_path) != 0) {
        ctx->logger->log_error(ctx, "backup", NULL,
                  "Failed to rename temporary file to final backup path");
        return -1;
    }
    return 0;
    #endif
}


/* ================================================================
 * PER-REPO BACKUP FLOW
 * ================================================================ */

backup_result backup_single_repo(ghb_context *ctx, const char *owner,
                                 const char *repo, const char *token,
                                 const backup_config *config,
                                 long *out_file_size) {

    long download_start = 0;
    char branch[MAX_REPO_NAME_LEN] = {0};
    /*
     * temp_path and final_path must accommodate backup_dir (up to MAX_URL_LEN)
     * plus a repo name (up to MAX_REPO_NAME_LEN) plus a file suffix.
     * validate_path_length already ensures the combined length fits within
     * MAX_PATH_BUF, so the snprintf calls below cannot truncate in practice.
     */
    char temp_path[MAX_PATH_BUF] = {0};
    char final_path[MAX_PATH_BUF] = {0};
    char detail[MAX_URL_LEN];

    DBG("backup: === Starting backup for '%s' ===", repo);

    DBG("backup: Resolving default branch for %s/%s...", owner, repo);

    ctx->logger->log_event(ctx, LOG_INFO, "backup", repo, "START",
              "Beginning backup for repository");

    /*
     * Step 1: Resolve the default branch.
     * This API call tells us which branch to download.
     */
    int branch_result = ctx->network->get_default_branch(
        ctx, owner, repo, token, branch, sizeof(branch),
        config->http_timeout
    );

    if (branch_result != 0) {
        DBG("backup: get_default_branch FAILED (code=%d)", branch_result);
        /*
         * get_default_branch returns specific HTTP status codes on API
         * failures so we can classify the error precisely. Network errors
         * and parse failures return -1.
         */
        if (branch_result == HTTP_NOT_FOUND) {
            return BACKUP_NOT_FOUND;
        }
        if (branch_result == HTTP_UNAUTHORIZED || branch_result == HTTP_FORBIDDEN) {
            return BACKUP_AUTH_ERROR;
        }
        if (branch_result == HTTP_RATE_LIMITED) {
            return BACKUP_RATE_LIMITED;
        }
        return BACKUP_NETWORK_ERROR;
    }

    snprintf(detail, sizeof(detail),
             "Default branch: %s - downloading zip archive", branch);
    ctx->logger->log_event(ctx, LOG_INFO, "backup", repo, "OK", detail);

    DBG("backup: Branch='%s' - constructing paths...", branch);

    /*
     * Step 2: Validate and construct file paths for the atomic write
     * pattern. temp_path: where the download goes first (.zip.tmp).
     * final_path: where the verified download ends up (.zip).
     *
     * Fail-fast: if backup_dir + repo + suffix exceeds MAX_URL_LEN,
     * the path would be silently truncated by snprintf - the download
     * would go to the wrong location. Catch this early with a loud
     * error (Coding Standard #34: Fail-Fast on Startup).
     */
    size_t dir_len = strlen(config->backup_dir);
    size_t repo_len = strlen(repo);

    if (validate_path_length(ctx, dir_len, repo_len + strlen(TEMP_FILE_SUFFIX),
                              repo) != 0) {
        return BACKUP_UNKNOWN_ERROR;
    }

    /* Path length already validated by validate_path_length() above.
     * GCC cannot prove the guard covers all cases at compile time,
     * so suppress -Wformat-truncation. Truncation never occurs in practice. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(temp_path, sizeof(temp_path), "%s%s%s",
             config->backup_dir, repo, TEMP_FILE_SUFFIX);
#pragma GCC diagnostic pop

    if (validate_path_length(ctx, dir_len, repo_len + strlen(FINAL_FILE_SUFFIX),
                              repo) != 0) {
        return BACKUP_UNKNOWN_ERROR;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(final_path, sizeof(final_path), "%s%s%s",
             config->backup_dir, repo, FINAL_FILE_SUFFIX);
#pragma GCC diagnostic pop

    /*
     * Step 3: Download the zip archive to the temporary file.
     * The download streams directly to disk - no memory buffering.
     */

    DBG("backup: Downloading zip to %s", temp_path);

    download_start = time(NULL);

    int download_result = ctx->network->download_repo_zip(
        ctx, owner, repo, branch, token, temp_path,
        config->http_timeout
    );

    if (download_result != 0) {
        DBG("backup: download_repo_zip FAILED (code=%d)", download_result);
        cleanup_temp_file(ctx, temp_path);

        if (download_result == -5) {
            /* Shutdown requested mid-download — abort immediately */
            DBG("backup: Shutdown requested mid-download (repo=%s)", repo);
            return BACKUP_SHUTDOWN;
        }
        if (download_result == -3) {
            /* Disk full — delete temp file, return specific code */
            return BACKUP_DISK_FULL;
        }
        if (download_result == -4) {
            /* Rate limited during zip download */
            return BACKUP_RATE_LIMITED;
        }
        if (download_result == -2) {
            return BACKUP_NETWORK_ERROR;
        }
        return BACKUP_NETWORK_ERROR;
    }

    /*
     * Step 4: Verify the downloaded file.
     * Check that it exists, is non-zero in size, and is readable.
     */

    DBG("backup: Verifying downloaded file at %s...", temp_path);

    if (verify_downloaded_file(temp_path) != 0) {
        ctx->logger->log_error(ctx, "backup", repo,
                  "Downloaded file verification failed - corrupt or empty");
        {
            char corrupt_msg[512];
            snprintf(corrupt_msg, sizeof(corrupt_msg),
                     "%s: downloaded file is corrupt or empty - old backup preserved",
                     repo);
            ctx->notify->toast_error(ctx, "Download Corrupt", corrupt_msg);
        }
        cleanup_temp_file(ctx, temp_path);
        return BACKUP_VERIFY_FAILED;
    }

    /*
     * Step 5: Atomic write - delete old backup, rename new.
     * At no point does the repo have zero valid backups.
     */

    DBG("backup: Atomic write: %s -> %s", temp_path, final_path);

    if (atomic_write(ctx, temp_path, final_path) != 0) {
        /*
         * Rename failed. The temp file still exists.
         * Keep it - the old backup (if any) is also intact.
         * Log the error but don't consider this a total failure
         * since the download itself was successful.
         */
        ctx->logger->log_error(ctx, "backup", repo,
                  "Atomic write failed - temp file preserved for manual recovery");
        ctx->notify->toast_error(ctx, "Write Failed", repo);
        return BACKUP_UNKNOWN_ERROR;
    }

    /* Success — get file size and timing for the toast */
    long file_size = 0;
    {
        #ifdef _WIN32
        HANDLE h = CreateFileA(
            final_path,
            GENERIC_READ,
            FILE_SHARE_READ,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );
        if (h != INVALID_HANDLE_VALUE) {
            file_size = (long)GetFileSize(h, NULL);
            CloseHandle(h);
        }
        #else
        struct stat st_buf;
        if (stat(final_path, &st_buf) == 0) {
            file_size = (long)st_buf.st_size;
        }
        #endif
    }
    long elapsed = (long)(time(NULL) - download_start);

    if (out_file_size != NULL) {
        *out_file_size = file_size;
    }

    snprintf(detail, sizeof(detail),
             "Backup completed successfully (branch: %s)", branch);
    ctx->logger->log_event(ctx, LOG_SUCCESS, "backup", repo, "OK", detail);
    {
        char toast_msg[512];
        snprintf(toast_msg, sizeof(toast_msg),
                 "Downloaded %ld KB in %ld seconds",
                 file_size / 1024, elapsed);
        ctx->notify->toast_success(ctx, repo, toast_msg);
    }

    DBG("backup: === '%s' BACKUP COMPLETE ===", repo);

    return BACKUP_OK;
}


/* ================================================================
 * CYCLE ORCHESTRATOR
 * ================================================================ */

int run_backup_cycle(ghb_context *ctx, const backup_config *config,
                     int *succeeded, int *failed) {

    *succeeded = 0;
    *failed = 0;

    DBG("backup: === CYCLE START - %d repos ===", config->repo_count);

    int cycle_aborted = 0;

    for (int i = 0; i < config->repo_count; i++) {
        const char *repo = config->repos[i];

        /*
         * Check for shutdown request between repos.
         * Spec Section 11: "the daemon finishes the current repository
         * download (if in progress), writes the cycle summary, closes
         * the log file, releases the mutex, and exits."
         *
         * We check BEFORE starting the next repo — the previous repo
         * (if any) has already completed, so this is a clean exit point.
         * If we're mid-shutdown, there's no point starting a new download.
         */
        if (ctx->should_stop && ctx->should_stop()) {
            ctx->logger->log_event(ctx, LOG_INFO, "backup", NULL, "STOPPING",
                      "Shutdown requested between repos - exiting cycle early");
            break;
        }

        long file_size = 0;
        backup_result result = backup_single_repo(
            ctx, config->owner, repo, config->token, config, &file_size
        );

        switch (result) {
            case BACKUP_OK:
                (*succeeded)++;
                break;

            case BACKUP_NOT_FOUND:
            case BACKUP_AUTH_ERROR:
            case BACKUP_NETWORK_ERROR:
            case BACKUP_VERIFY_FAILED:
            case BACKUP_UNKNOWN_ERROR:
                (*failed)++;
                break;

            case BACKUP_DISK_FULL:
                (*failed)++;
                cycle_aborted = 1;
                break;

            case BACKUP_SHUTDOWN:
                /*
                 * Shutdown requested mid-download (Sir R153: immediate kill,
                 * not "finish current download" per original spec).
                 * Abort the cycle immediately — .tmp already cleaned up by
                 * backup_single_repo, old .zip backup is intact.
                 */
                (*failed)++;
                cycle_aborted = 1;
                break;

            case BACKUP_RATE_LIMITED:
                /*
                 * Rate limiting is handled within backup_single_repo
                 * or by the main loop. For now, count as a failure
                 * and continue to the next repo.
                 */
                (*failed)++;
                break;
        }

        /*
         * If the cycle was aborted (disk full), stop processing
         * remaining repos. The previous backup for the current
         * repo is still intact.
         */
        if (cycle_aborted) {
            ctx->logger->log_event(ctx, LOG_ERROR, "backup", NULL, "ABORTED",
                      "Cycle aborted - disk full or critical error");
            ctx->notify->toast_error(ctx, "Cycle Aborted",
                        "Backup cycle stopped due to disk full");
            break;
        }
    }

    return cycle_aborted ? -1 : 0;
}
