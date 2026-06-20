/**
 * logger_iface.h - Logger interface for dependency injection.
 *
 * Defines the log_level enum and the logger_ops struct (function pointer
 * table) that decouples consumers from the concrete logger implementation.
 * Consumers include this header and call through ctx->logger->log_event(),
 * never directly calling the logger module's functions.
 *
 * This is the MINIMAL interface that other modules need from the logger.
 * Lifecycle functions (log_init, log_close, rotate_log, etc.) are declared
 * in logger.h — only the composition root (main.c) needs those.
 *
 * Interface decoupling: backup.c, network.c, config.c, and notify.c
 * include this header instead of logger.h. They see only what they need.
 */

#ifndef LOGGER_IFACE_H
#define LOGGER_IFACE_H

#include "constants.h"

/* Forward declaration — full definition in context.h */
typedef struct ghb_context ghb_context;


/* ================================================================
 * LOG LEVEL
 * ================================================================ */

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


/* ================================================================
 * LOGGER OPS — DEPENDENCY INJECTION INTERFACE
 * ================================================================ */

/**
 * Function pointer table for the logger module.
 *
 * Enables dependency injection: consumers receive a logger_ops pointer
 * through the ghb_context and call through function pointers instead of
 * direct function calls. This allows:
 *   - Test isolation: inject a fake logger that records calls
 *   - Alternative implementations: silent logger, remote logger, etc.
 *   - Interface decoupling: consumers only depend on this minimal header
 *
 * Every function takes ghb_context *ctx as the first parameter for
 * consistency, even if the implementation doesn't use it (the logger
 * is the base module with no cross-module dependencies).
 */
typedef struct logger_ops {
    /**
     * Write a structured log entry.
     * See logger.h for full documentation.
     */
    void (*log_event)(ghb_context *ctx, log_level level,
                      const char *action, const char *repo,
                      const char *status, const char *detail);

    /**
     * Shorthand for error-level logging.
     * See logger.h for full documentation.
     */
    void (*log_error)(ghb_context *ctx, const char *action,
                      const char *repo, const char *detail);

    /**
     * Initialize the logging subsystem.
     * See logger.h for full documentation.
     */
    int  (*log_init)(ghb_context *ctx, const char *log_path);

    /**
     * Flush and close the log file handle.
     * See logger.h for full documentation.
     */
    void (*log_close)(ghb_context *ctx);

    /**
     * Rotate the log file if it exceeds the size threshold.
     * See logger.h for full documentation.
     */
    void (*rotate_log)(ghb_context *ctx, long max_size_bytes);
} logger_ops;


#endif /* LOGGER_IFACE_H */
