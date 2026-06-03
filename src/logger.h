/**
 * logger.h — Logging interface for the GitHub Backup Script.
 *
 * Provides structured log entries with timestamp, action, repository name,
 * status, and optional detail. The logger writes to a single append-only
 * file and supports rotation when the file exceeds a configured size.
 *
 * Every module in the project calls logger functions to record events.
 * The logger is the first utility module in the build sequence because
 * all subsequent modules depend on it for event recording.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include "constants.h"


/**
 * Log entry severity levels. Used in structured log entries to indicate
 * the nature of the event being recorded.
 */
typedef enum {
    LOG_INFO,    /* Informational event — cycle start, cycle complete */
    LOG_SUCCESS, /* Successful operation — repo backed up */
    LOG_WARNING, /* Non-fatal issue — repo 404, connectivity check failed */
    LOG_ERROR    /* Fatal or blocking error — corrupt .env, disk full */
} log_level;


/**
 * Initialize the logging subsystem. Opens the log file at the specified
 * path in append mode. If the file cannot be opened, logs to stderr
 * (which is invisible in background mode but prevents silent failure).
 *
 * @param log_path  Full path to the log file (e.g., "D:\\BACKUP\\backup.log")
 * @return 0 on success, -1 if the file could not be opened
 */
int log_init(const char *log_path);


/**
 * Write a structured log entry to the log file. Each entry contains
 * an ISO 8601 timestamp, the log level, action description, optional
 * repository name, status indicator, and optional detail string.
 *
 * @param level    Severity level (LOG_INFO, LOG_SUCCESS, etc.)
 * @param action   What happened (e.g., "backup", "connectivity check")
 * @param repo     Repository name, or NULL if not applicable
 * @param status   Status string (e.g., "OK", "FAILED", "SKIPPED")
 * @param detail   Additional detail or error message, or NULL if none
 */
void log_event(log_level level, const char *action, const char *repo,
               const char *status, const char *detail);


/**
 * Shorthand for error-level logging. Equivalent to log_event with
 * level=LOG_ERROR.
 *
 * @param action  What failed
 * @param repo    Repository name, or NULL
 * @param detail  Error description
 */
void log_error(const char *action, const char *repo, const char *detail);


/**
 * Check whether the log file has exceeded the rotation threshold
 * and rotate if necessary. Rotation means deleting the log file and
 * starting fresh (Decision 003).
 *
 * @param max_size_bytes  Rotation threshold in bytes. If 0, rotation
 *                        is disabled and this function does nothing.
 */
void rotate_log(long max_size_bytes);


/**
 * Flush and close the log file handle. Called on graceful shutdown.
 */
void log_close(void);


#endif /* LOGGER_H */
