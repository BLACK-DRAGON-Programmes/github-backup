/**
 * logger.h - Logging interface for the GitHub Backup Script.
 *
 * Provides structured log entries with timestamp, action, repository name,
 * status, and optional detail. The logger writes to a single append-only
 * file and supports rotation when the file exceeds a configured size.
 *
 * Every module in the project calls logger functions to record events.
 * The logger is the first utility module in the build sequence because
 * all subsequent modules depend on it for event recording.
 *
 * === DEV PHASE DEBUG MACRO ===
 *
 * DBG() writes debug output to BOTH stderr AND the log file.
 * In daemon mode (headless, no console), stderr is invisible but the
 * log file is tailed by the viewer - so DBG output appears in the
 * viewer's terminal via the log file.
 *
 * To disable ALL debug output for production:
 *   1. Comment out or delete:   #define DBG_ENABLED
 *   2. Recompile
 * All DBG() calls compile to ((void)0) - zero overhead, zero output.
 * The log_debug() function body is also compiled out.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include "constants.h"

#include <stdarg.h>
#include <stdio.h>


/* ================================================================
 * DEV PHASE: Compile-time debug logging toggle.
 *
 * When DBG_ENABLED is defined, DBG(fmt, ...) expands to a do-while
 * block that writes to stderr AND appends to the log file (via
 * log_debug()). Both outputs include the [DBG] prefix.
 *
 * When DBG_ENABLED is NOT defined, DBG(fmt, ...) expands to
 * ((void)0) - the compiler eliminates all debug output completely.
 * ================================================================ */

#ifdef DBG_ENABLED

/**
 * Write a raw debug string to the log file with a timestamp prefix.
 * Called by the DBG macro. Writes "[YYYY-MM-DD HH:MM:SS] <msg>\n".
 * If the log file is not yet opened (g_log_file == NULL), silently
 * returns - the debug output still went to stderr.
 *
 * NOT for direct use in application code - use DBG() instead.
 */
void log_debug(const char *fmt, ...);

/**
 * DBG - Debug print macro. Writes to stderr AND the log file.
 *
 * Usage: DBG("network_init: Calling WinHttpOpen...");
 *        DBG("download progress: %lu bytes", total_downloaded);
 *
 * The [DBG] prefix is added automatically. A newline is appended
 * automatically. No manual \n needed in the format string.
 */
#define DBG(fmt, ...) do { \
    fprintf(stderr, "[DBG] " fmt "\n", ##__VA_ARGS__); \
    fflush(stderr); \
    log_debug("[DBG] " fmt, ##__VA_ARGS__); \
} while(0)

#else

/* Production build: DBG compiles to nothing */
#define DBG(fmt, ...) ((void)0)

#endif /* DBG_ENABLED */


/**
 * Log entry severity levels. Used in structured log entries to indicate
 * the nature of the event being recorded.
 */
typedef enum {
    LOG_INFO,    /* Informational event - cycle start, cycle complete */
    LOG_SUCCESS, /* Successful operation - repo backed up */
    LOG_WARNING, /* Non-fatal issue - repo 404, connectivity check failed */
    LOG_ERROR    /* Fatal or blocking error - corrupt .env, disk full */
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


/**
 * Enable or disable console output. When enabled, log_event prints
 * to the console with ANSI colors in addition to writing to the file.
 * When disabled (background mode), only file output is active.
 *
 * @param enabled  1 to enable console output, 0 to disable
 */
void log_set_console_output(int enabled);

/**
 * Returns whether console output is currently enabled.
 *
 * @return 1 if console output is active, 0 if not
 */
int log_get_console_output(void);


#endif /* LOGGER_H */
