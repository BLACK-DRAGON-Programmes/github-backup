/**
 * notify.h — Windows toast notification interface.
 *
 * Fires Windows toast notifications for every runtime event — success,
 * failure, connectivity, rate limiting, cycle start/complete. The script
 * runs as a background scheduled task with no console window, so toasts
 * are the only user-visible feedback mechanism.
 *
 * Toast events are also logged via the logger module (dual output per
 * Coding Standard #40: everything must be logged).
 *
 * Implementation uses the Windows Runtime toast API via COM activation.
 */

#ifndef NOTIFY_H
#define NOTIFY_H


/**
 * Initialize the COM subsystem required for Windows toast notifications.
 * Must be called once at startup before any toast function.
 *
 * @return 0 on success, -1 if COM initialization fails
 */
int notify_init(void);


/**
 * Fire an informational toast notification. Used for neutral events
 * like cycle start, connectivity confirmed, service started.
 *
 * @param title   Toast title (e.g., "Backup Cycle")
 * @param message Toast body text (e.g., "Starting backup for 3 repos")
 */
void toast_info(const char *title, const char *message);


/**
 * Fire a success toast for a specific repository backup.
 *
 * @param repo    Repository name that was backed up
 * @param message Additional detail (e.g., "Saved 2.3 MB")
 */
void toast_success(const char *repo, const char *message);


/**
 * Fire an error toast notification. Used for failures, rate limiting,
 * connectivity loss, corrupt configuration.
 *
 * @param title   Error category (e.g., "Download Failed", "Rate Limited")
 * @param message Error detail (e.g., "repo-name: HTTP 404")
 */
void toast_error(const char *title, const char *message);


/**
 * Release COM resources. Called on graceful shutdown.
 */
void notify_cleanup(void);


#endif /* NOTIFY_H */
