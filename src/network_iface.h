/**
 * network_iface.h - Network interface for dependency injection.
 *
 * Defines the network_ops struct (function pointer table) that decouples
 * consumers from the concrete network implementation (WinHTTP on Windows,
 * stubs on other platforms). This is the KEY swappable interface —
 * alternative backends (libcurl, GitLab API, Bitbucket API) implement
 * this interface and are injected through the ghb_context.
 *
 * This is the MINIMAL interface that other modules need from the network
 * layer. Internal functions (http_get, JSON parsing, rate limit parsing)
 * and constants (MAX_HTTP_RESPONSE_LEN, rate_limit_info) stay in
 * network.h — only the composition root and the network implementation
 * need those.
 *
 * Interface decoupling: backup.c includes this header instead of
 * network.h. It sees only the three functions it needs, not the entire
 * network module's internal API.
 */

#ifndef NETWORK_IFACE_H
#define NETWORK_IFACE_H

#include "constants.h"

/* Forward declaration — full definition in context.h */
typedef struct ghb_context ghb_context;


/* ================================================================
 * NETWORK OPS — DEPENDENCY INJECTION INTERFACE
 * ================================================================ */

/**
 * Function pointer table for the network module.
 *
 * This is the primary swappable interface in ghb. A different backend
 * (libcurl, GitLab API, Bitbucket API) implements these functions and
 * provides a network_ops struct pointing to its implementations. The
 * composition root (main.c) selects which backend to use.
 *
 * Enables dependency injection:
 *   - Test isolation: inject a fake network that returns canned responses
 *   - Swappable backends: WinHTTP, libcurl, GitLab, Bitbucket, etc.
 *   - Interface decoupling: consumers only depend on this minimal header
 *
 * Every function takes ghb_context *ctx as the first parameter so the
 * implementation can call ctx->logger and ctx->notify for error reporting
 * and rate limit notifications.
 */
typedef struct network_ops {
    /**
     * Resolve the default branch name for a GitHub repository.
     * See network.h for full documentation.
     */
    int  (*get_default_branch)(ghb_context *ctx, const char *owner,
                               const char *repo, const char *token,
                               char *branch_out, int branch_len,
                               int timeout_ms);

    /**
     * Download a repository zip archive to a file on disk.
     * See network.h for full documentation.
     */
    int  (*download_repo_zip)(ghb_context *ctx, const char *owner,
                              const char *repo, const char *branch,
                              const char *token,
                              const char *output_path, int timeout_ms);

    /**
     * Check internet connectivity.
     * See network.h for full documentation.
     */
    int  (*check_connectivity)(ghb_context *ctx, int timeout_ms);

    /**
     * Initialize the network subsystem.
     * See network.h for full documentation.
     */
    int  (*network_init)(ghb_context *ctx);

    /**
     * Close the network session and release resources.
     * See network.h for full documentation.
     */
    void (*network_cleanup)(ghb_context *ctx);
} network_ops;


#endif /* NETWORK_IFACE_H */
