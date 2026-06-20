/**
 * network.h - Network interface for the GitHub Backup Script.
 *
 * Handles all HTTP communication with the GitHub API. Uses WinHTTP on
 * Windows for HTTP requests, with platform stubs for compilation testing
 * on non-Windows environments.
 *
 * The network module is a pure I/O layer - it receives token, owner, and
 * repo name as function parameters and does not read the .env file
 * directly. This keeps the module decoupled from configuration.
 *
 * JSON parsing functions are platform-independent and work on any OS.
 */

#ifndef NETWORK_H
#define NETWORK_H

#include "network_iface.h"


/* ================================================================
 * NETWORK-SPECIFIC BUFFER CONSTANTS
 *
 * These buffer sizes are specific to the network module's internal
 * operations. They are not general-purpose constants needed by other
 * modules, so they are defined here rather than in constants.h.
 * ================================================================ */

/**
 * Maximum size of an HTTP response body buffer for API metadata calls.
 * Used when reading JSON responses (repo metadata, branch info).
 *
 * Rationale: GitHub API JSON responses for single repo metadata are
 * typically 1-5KB. A limit of 65536 bytes (64KB) provides generous
 * headroom for unusually large responses without excessive stack use.
 * Zip downloads bypass this buffer - they stream directly to disk.
 */
#define MAX_HTTP_RESPONSE_LEN         65536


/**
 * Chunk size for streaming HTTP response data to disk during zip
 * downloads. Controls how much data is read per WinHttpReadData call
 * and written per fwrite call.
 *
 * Rationale: 8192 bytes (8KB) balances memory usage and I/O efficiency.
 * Larger chunks reduce syscall overhead; smaller chunks reduce peak
 * memory consumption. 8KB is a common page-aligned I/O buffer size.
 */
#define HTTP_READ_CHUNK_SIZE          8192


/**
 * HTTP User-Agent string sent with every API request.
 * GitHub's API rejects requests without a User-Agent header (HTTP 403).
 * This constant satisfies that requirement with a descriptive agent name.
 */
#define HTTP_USER_AGENT               "GitHubBackup/1.0"


/* ================================================================
 * RATE LIMIT DATA STRUCTURE
 * ================================================================ */

/**
 * Rate limit information extracted from GitHub API response headers.
 * Populated by http_get() and returned to the caller for rate limit
 * handling decisions (sleep until reset window when rate-limited).
 *
 * The two relevant GitHub API headers are:
 *   X-RateLimit-Remaining - integer, requests left in current window
 *   X-RateLimit-Reset - integer, Unix timestamp when window resets
 */
typedef struct {
    int remaining;      /* X-RateLimit-Remaining: requests left in current window */
    long reset_time;   /* X-RateLimit-Reset: Unix timestamp when window resets */
    int headers_parsed; /* Non-zero if rate limit headers were found in response */
} rate_limit_info;


/* ================================================================
 * FUNCTION DECLARATIONS
 * ================================================================ */

/**
 * Initialize the network subsystem. Opens a WinHTTP session handle
 * that persists for the lifetime of the program.
 *
 * Must be called once before any network operation. Paired with
 * network_cleanup() on shutdown.
 *
 * @return 0 on success, -1 if WinHTTP session could not be opened
 */
int network_init(ghb_context *ctx);


/**
 * Check internet connectivity by attempting to connect to the
 * host defined in CONNECTIVITY_CHECK_URL.
 *
 * This is a lightweight probe - it verifies DNS resolution and TCP
 * connection, not full HTTP request/response. Used before each backup
 * cycle to decide whether to proceed or skip.
 *
 * @param timeout_ms  Maximum time to wait for connectivity response
 * @return 1 if internet is available, 0 if not
 */
int check_connectivity(ghb_context *ctx, int timeout_ms);


/**
 * Perform an HTTP GET request with optional Bearer token authentication.
 * Reads the response body into a caller-provided buffer and returns
 * the HTTP status code and rate limit information.
 *
 * The caller constructs the full URL. The function does not modify
 * the URL or token strings.
 *
 * @param url           Full URL to request (e.g., "https://api.github.com/repos/owner/repo")
 * @param token         Bearer token for Authorization header, or NULL for unauthenticated
 * @param response_body Output buffer for the response body
 * @param body_size     Size of the response_body buffer in bytes
 * @param response_code Output: HTTP status code (200, 404, 401, etc.)
 * @param rate_info     Output: rate limit information parsed from response headers
 * @param timeout_ms    Request timeout in milliseconds
 * @return 0 on success (response received), -1 on network error, -2 on timeout
 */
int http_get(ghb_context *ctx, const char *url, const char *token,
             char *response_body, int body_size,
             int *response_code, rate_limit_info *rate_info,
             int timeout_ms);


/**
 * Extract a string field value from a JSON response body.
 * Simple key-value parser - no external JSON library. Finds the first
 * occurrence of "key" followed by ":" and extracts the quoted string value.
 *
 * This parser handles flat JSON objects as returned by the GitHub API
 * for repo metadata. It does NOT handle nested objects or arrays.
 *
 * @param json       JSON string to parse
 * @param key        Field name to find (e.g., "default_branch")
 * @param value_out  Output buffer for the extracted value
 * @param value_len  Size of the output buffer in bytes
 * @return 0 on success, -1 if key not found or parse error
 */
int parse_json_string(const char *json, const char *key,
                      char *value_out, int value_len);


/**
 * Extract an integer field value from a JSON response body.
 * Finds the first occurrence of "key" followed by ":" and parses
 * the subsequent integer value.
 *
 * @param json      JSON string to parse
 * @param key       Field name to find
 * @param value_out Output: extracted integer value
 * @return 0 on success, -1 if key not found or parse error
 */
int parse_json_int(const char *json, const char *key, int *value_out);


/**
 * Resolve the default branch name for a GitHub repository.
 * Constructs the API URL, calls http_get(), and extracts the
 * "default_branch" field from the JSON response.
 *
 * Return values are specific HTTP status codes on failure so the caller
 * can classify the error type (404 → not found, 401/403 → auth error,
 * 429 → rate limited). Network errors and parse errors return -1.
 *
 * @param owner       Repository owner/organization name
 * @param repo        Repository name
 * @param token       GitHub personal access token for Authorization header
 * @param branch_out  Output buffer for the default branch name
 * @param branch_len  Size of the output buffer in bytes
 * @param timeout_ms  HTTP request timeout in milliseconds
 * @return 0 on success, -1 on network/parse error, or the HTTP status
 *         code (404, 401, 403, 429, etc.) on API failures
 */
int get_default_branch(ghb_context *ctx, const char *owner, const char *repo,
                       const char *token, char *branch_out,
                       int branch_len, int timeout_ms);


/**
 * Download a repository zip archive to a file on disk.
 * Streams the response body directly to the output file in chunks
 * (HTTP_READ_CHUNK_SIZE bytes per iteration) to avoid buffering
 * the entire archive in memory.
 *
 * The output_path should include the temporary file suffix
 * (TEMP_FILE_SUFFIX) for atomic write safety.
 *
 * @param owner        Repository owner/organization name
 * @param repo         Repository name
 * @param branch       Branch name to download
 * @param token        GitHub personal access token
 * @param output_path  Full file path to write the zip archive to
 * @param timeout_ms   HTTP request timeout in milliseconds
 * @return 0 on success, -1 on failure (network error, write error, disk full)
 */
int download_repo_zip(ghb_context *ctx, const char *owner, const char *repo,
                      const char *branch, const char *token,
                      const char *output_path, int timeout_ms);


/**
 * Close the WinHTTP session handle and release network resources.
 * Called on graceful shutdown.
 */
void network_cleanup(ghb_context *ctx);


#endif /* NETWORK_H */
