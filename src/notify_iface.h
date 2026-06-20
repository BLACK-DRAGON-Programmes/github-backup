/**
 * notify_iface.h - Notification interface for dependency injection.
 *
 * Defines the notify_ops struct (function pointer table) that decouples
 * consumers from the concrete notification implementation (PowerShell
 * toast bridge on Windows, stubs on other platforms).
 *
 * This is the MINIMAL interface that other modules need from the notifier.
 * Lifecycle functions (notify_init, notify_cleanup) are declared in
 * notify.h — only the composition root (main.c) needs those.
 *
 * Interface decoupling: backup.c, network.c, and config.c include this
 * header instead of notify.h. They see only what they need.
 */

#ifndef NOTIFY_IFACE_H
#define NOTIFY_IFACE_H

#include "constants.h"

/* Forward declaration — full definition in context.h */
typedef struct ghb_context ghb_context;


/* ================================================================
 * NOTIFY OPS — DEPENDENCY INJECTION INTERFACE
 * ================================================================ */

/**
 * Function pointer table for the notification module.
 *
 * Enables dependency injection: consumers receive a notify_ops pointer
 * through the ghb_context and call through function pointers instead of
 * direct function calls. This allows:
 *   - Test isolation: inject a fake notifier that records calls silently
 *   - Alternative implementations: no-op notifier, syslog, email, etc.
 *   - Interface decoupling: consumers only depend on this minimal header
 *
 * Every function takes ghb_context *ctx as the first parameter so the
 * implementation can call ctx->logger->log_event() for dual output
 * (toast + log, per Coding Standard #40).
 */
typedef struct notify_ops {
    /**
     * Fire an informational toast notification.
     * See notify.h for full documentation.
     */
    void (*toast_info)(ghb_context *ctx, const char *title,
                       const char *message);

    /**
     * Fire a success toast for a specific repository backup.
     * See notify.h for full documentation.
     */
    void (*toast_success)(ghb_context *ctx, const char *repo,
                          const char *message);

    /**
     * Fire an error toast notification.
     * See notify.h for full documentation.
     */
    void (*toast_error)(ghb_context *ctx, const char *title,
                        const char *message);

    /**
     * Initialize the notification subsystem (COM on Windows).
     * See notify.h for full documentation.
     */
    int  (*notify_init)(ghb_context *ctx);

    /**
     * Release notification resources.
     * See notify.h for full documentation.
     */
    void (*notify_cleanup)(ghb_context *ctx);
} notify_ops;


#endif /* NOTIFY_IFACE_H */
