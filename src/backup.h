/**
 * backup.h - Backup orchestration interface for the GitHub Backup Script.
 *
 * Provides the per-repo backup flow (branch resolution, download,
 * verification, atomic write) and the full cycle orchestrator that
 * iterates over all repos in the configuration.
 *
 * This is the highest-level domain module - it ties together config
 * (repo list), network (API calls), logger (event recording), and
 * notify (user feedback). Only main.c depends on this module.
 *
 * Atomic write guarantee (Decision 005):
 *   1. Download to {repo}.zip.tmp
 *   2. Verify .zip.tmp (non-zero size, readable)
 *   3. Delete old {repo}.zip (if exists)
 *   4. Rename .zip.tmp → .zip
 *   At no point does a repo have zero valid backups.
 */

#ifndef BACKUP_H
#define BACKUP_H

#include "constants.h"
#include "config.h"
#include "context.h"


/**
 * Backup result codes. Returned by backup_single_repo() to indicate
 * the outcome of a single repository backup attempt.
 */
typedef enum {
    BACKUP_OK,               /* Backup completed successfully */
    BACKUP_NOT_FOUND,        /* Repository returned 404 - skip */
    BACKUP_AUTH_ERROR,       /* Token invalid/expired - skip */
    BACKUP_NETWORK_ERROR,   /* Network failure - skip */
    BACKUP_VERIFY_FAILED,    /* Download corrupt - old backup intact */
    BACKUP_DISK_FULL,        /* Disk full - stop cycle */
    BACKUP_RATE_LIMITED,     /* Rate limited - sleep until reset */
    BACKUP_SHUTDOWN,         /* Shutdown requested mid-download - abort cycle */
    BACKUP_UNKNOWN_ERROR     /* Unclassified error */
} backup_result;


/**
 * Perform a full backup cycle for a single repository.
 * Resolves the default branch, downloads the zip archive, verifies
 * it on disk, and performs an atomic write (delete old, rename new).
 *
 * @param ctx     Dependency injection context (logger, notify, network)
 * @param owner   Repository owner/organization
 * @param repo    Repository name
 * @param token   GitHub personal access token
 * @param config  Runtime configuration (timeouts, paths)
 * @return        backup_result code indicating the outcome
 */
backup_result backup_single_repo(ghb_context *ctx, const char *owner,
                                 const char *repo, const char *token,
                                 const backup_config *config,
                                 long *out_file_size);


/**
 * Verify that a downloaded file exists on disk, is non-zero in size,
 * and is readable. Used as the verification step in the atomic write
 * pattern before replacing the old backup.
 *
 * @param file_path  Full path to the file to verify
 * @return 0 if verification passes, -1 if file is missing, empty,
 *         or unreadable
 */
int verify_downloaded_file(const char *file_path);


/**
 * Perform an atomic file write: delete the old backup file (if it
 * exists), then rename the temporary file to the final path.
 *
 * This is the last step in the atomic write pattern. The old backup
 * is only deleted after the new backup has been verified on disk.
 *
 * @param ctx        Dependency injection context (logger)
 * @param temp_path  Path to the verified temporary file (.zip.tmp)
 * @param final_path Path to the final backup file (.zip)
 * @return 0 on success, -1 if rename or delete fails
 */
int atomic_write(ghb_context *ctx, const char *temp_path,
                 const char *final_path);


/**
 * Delete a temporary file. Used when download verification fails
 * or when a disk-full error occurs during write. The previous backup
 * (if any) is left untouched.
 *
 * @param ctx        Dependency injection context (logger)
 * @param temp_path  Path to the temporary file to delete
 */
void cleanup_temp_file(ghb_context *ctx, const char *temp_path);


/**
 * Run a full backup cycle: iterate over all repos in the config,
 * attempt backup for each, and track succeeded/failed counts.
 *
 * Fires cycle-start and cycle-complete toasts. Handles rate limiting
 * by sleeping until the reset window and retrying.
 *
 * @param ctx       Dependency injection context (logger, notify, network)
 * @param config    Runtime configuration (token, owner, repos, timeouts, paths)
 * @param succeeded Output: number of repos backed up successfully
 * @param failed    Output: number of repos that failed
 * @return 0 if all repos processed, -1 if cycle was aborted (disk full)
 */
int run_backup_cycle(ghb_context *ctx, const backup_config *config,
                     int *succeeded, int *failed);


#endif /* BACKUP_H */
