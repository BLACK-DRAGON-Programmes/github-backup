/**
 * context.h - Dependency injection container for the GitHub Backup Script.
 *
 * Defines ghb_context — the single struct that holds all module interface
 * pointers. Initialized once at startup in main.c (the composition root),
 * then passed through the entire call chain. Every module that needs to
 * call another module does so through this context, never through direct
 * function calls.
 *
 * Usage pattern:
 *   ctx->logger->log_event(ctx, LOG_INFO, "action", NULL, "OK", "detail");
 *   ctx->notify->toast_error(ctx, "Title", "Message");
 *   ctx->network->get_default_branch(ctx, owner, repo, token, ...);
 *
 * The composition root (main.c) creates static const ops structs pointing
 * to the real implementations, then builds the ghb_context from them.
 * Tests create ops structs pointing to fake implementations.
 *
 * Why a single context struct instead of passing individual ops pointers?
 *   1. One parameter instead of three — less function signature bloat
 *   2. Adding a new module interface means adding one field here, not
 *      changing every function signature that uses it
 *   3. The context is the single source of truth for which
 *      implementations are active
 *
 * Dependency order (declared here, initialized in main.c):
 *   1. logger  — base module, no cross-module dependencies
 *   2. notify  — depends on logger (logs every toast)
 *   3. network — depends on logger and notify (errors, rate limit toasts)
 *   Backup and config modules use all three but are not in the context
 *   because they are not swappable backends — they are domain-specific
 *   orchestration that ties the swappable pieces together.
 */

#ifndef CONTEXT_H
#define CONTEXT_H

#include "logger_iface.h"
#include "notify_iface.h"
#include "network_iface.h"


/* ================================================================
 * GHB CONTEXT — DEPENDENCY INJECTION CONTAINER
 * ================================================================ */

/**
 * Shutdown check function type.
 * Returns non-zero when a graceful shutdown has been requested
 * (via --shutdown, q key in viewer, or named event signal).
 * The concrete implementation is platform-specific and wired
 * in main.c (the composition root).
 */
typedef int (*shutdown_check_fn)(void);


/**
 * Runtime context holding all module interfaces.
 * Initialized once at startup, passed through the call chain.
 *
 * All ops fields are const pointers — the context is immutable after
 * initialization. This prevents any module from swapping out an
 * interface at runtime.
 *
 * The should_stop function pointer allows long-running operations
 * (like run_backup_cycle) to check for shutdown requests between
 * work items, per spec Section 11: "the daemon finishes the current
 * repository download (if in progress), writes the cycle summary,
 * closes the log file, releases the mutex, and exits."
 */
typedef struct ghb_context {
    const logger_ops  *logger;       /* Logging interface */
    const notify_ops  *notify;       /* Toast notification interface */
    const network_ops *network;      /* HTTP/backend interface */
    shutdown_check_fn  should_stop;  /* Returns non-zero when shutdown requested, or NULL */
} ghb_context;


#endif /* CONTEXT_H */
