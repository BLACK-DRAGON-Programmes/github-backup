/**
 * notify.h — Windows toast notification interface.
 *
 * Fires Windows toast notifications for every runtime event — success,
 * failure, connectivity, rate limiting, cycle start/complete. Toasts are
 * the primary user-visible feedback mechanism in daemon mode (the daemon
 * has no console).
 *
 * Toast click interaction (Spec Section 9):
 *   Every toast registers an Activated event handler. When the user clicks
 *   a toast notification, the handler launches backup.exe with no flags.
 *   Since the daemon is always running when toasts fire, the mutex already
 *   exists and the new process enters viewer mode — the user sees a live
 *   log tail of backup activity.
 *
 * Toast events are also logged via the logger module (dual output per
 * Coding Standard #40: everything must be logged).
 *
 * Implementation uses the Windows Runtime toast API via PowerShell bridge.
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
 * Clicking the toast launches backup.exe in viewer mode.
 *
 * @param title   Toast title (e.g., "Backup Cycle")
 * @param message Toast body text (e.g., "Starting backup for 3 repos")
 */
void toast_info(const char *title, const char *message);


/**
 * Fire a success toast for a specific repository backup.
 * Clicking the toast launches backup.exe in viewer mode.
 *
 * @param repo    Repository name that was backed up
 * @param message Additional detail (e.g., "Saved 2.3 MB")
 */
void toast_success(const char *repo, const char *message);


/**
 * Fire an error toast notification. Used for failures, rate limiting,
 * connectivity loss, corrupt configuration.
 * Clicking the toast launches backup.exe in viewer mode.
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
