/**
 * console.h — Console output interface for the GitHub Backup Script.
 *
 * Provides ANSI color-coded, column-aligned console output for
 * viewer mode (Section 8b of spec). On Windows 10+, ANSI escape
 * codes are enabled via SetConsoleMode with ENABLE_VIRTUAL_TERMINAL_PROCESSING.
 *
 * This module also provides the log viewer mode (Section 10c):
 * when a viewer process is launched (double-click or toast click),
 * it tails the daemon's log file with colored output. The viewer
 * supports 'q' to signal daemon shutdown (Section 11a).
 *
 * Viewer controls:
 *   q       — Signal daemon shutdown (Section 11a)
 *   Ctrl+C  — Close viewer only (Section 11c)
 *   Close   — Close viewer only (Section 11c)
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include "logger.h"

/**
 * Initialize the console for ANSI output.
 * Enables ENABLE_VIRTUAL_TERMINAL_PROCESSING on the stdout handle.
 * Must be called once before any console output in viewer mode.
 * Safe to call even if there's no console — it just fails silently.
 */
int console_init(void);

/**
 * Print a log entry to the console with ANSI color formatting.
 * Color scheme: INFO=gray, OK/SUCCESS=green, WARN=yellow, ERROR=red.
 * Timestamps are dim gray, repo names are cyan, separators are dim gray.
 * Format: [timestamp]  LEVEL  | action | repo | status | detail
 *
 * @param level    Log level (LOG_INFO, LOG_SUCCESS, LOG_WARNING, LOG_ERROR)
 * @param action   What happened (e.g., "backup", "network")
 * @param repo     Repository name, or NULL
 * @param status   Status string (e.g., "OK", "FAILED")
 * @param detail   Additional detail, or NULL
 */
void console_print_log(log_level level, const char *action,
                       const char *repo, const char *status,
                       const char *detail);

/**
 * Enter log viewer mode. Opens backup.log, seeks to end, tails new entries
 * with ANSI color formatting. Blocks until 'q' is pressed (signals daemon
 * shutdown) or Ctrl+C (viewer only).
 *
 * The viewer polls for 'q' key presses using non-blocking keyboard input.
 * When 'q' is detected, it opens the named shutdown event and sets it,
 * then waits up to 5 seconds for the daemon's shutdown log entries before
 * exiting.
 *
 * @param log_path  Full path to backup.log
 */
void console_log_viewer(const char *log_path);

/**
 * Check if the console has ANSI support enabled.
 *
 * @return 1 if ANSI is active, 0 if not (background mode, old Windows, etc.)
 */
int console_is_active(void);

/**
 * Clean up console resources.
 */
void console_cleanup(void);

#endif /* CONSOLE_H */
