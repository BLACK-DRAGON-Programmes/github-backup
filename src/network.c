/**
 * network.c - Network implementation for the GitHub Backup Script.
 *
 * Provides HTTP communication with the GitHub API using WinHTTP on
 * Windows. Includes platform-independent JSON parsing functions for
 * extracting fields from API responses.
 *
 * Platform support:
 *   Windows: Full WinHTTP implementation for all HTTP operations.
 *   Other:   Stub implementations that log warnings. JSON parsing
 *            functions work on all platforms.
 *
 * WinHTTP session management:
 *   The WinHTTP session handle (g_hSession) is opened by network_init()
 *   and closed by network_cleanup(). Individual requests open and close
 *   their own connect and request handles within each function call.
 *   WinHTTP handles connection pooling internally, so per-request
 *   connect handles do not create actual TCP connections each time.
 */

#include "network.h"
#include "context.h"
#include "logger.h"  /* DBG macro — compile-time debug toggle, not a DI call */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

#ifndef ERROR_INTERNET_TIMEOUT
#define ERROR_INTERNET_TIMEOUT 12002
#endif

/**
 * Persistent WinHTTP session handle. Opened once at startup,
 * used for all API requests, closed on shutdown.
 * Consistent with the static file-scope state pattern used by
 * logger.c (g_log_file) and notify.c (g_com_initialized).
 */
static HINTERNET g_hSession = NULL;

/**
 * Host name for GitHub API requests. All API calls (metadata,
 * zipball downloads) go through api.github.com.
 */
static const wchar_t *GITHUB_API_HOST = L"api.github.com";

/**
 * Host name for the connectivity check. Derived from CONNECTIVITY_CHECK_URL.
 * The full URL constant exists for documentation, but WinHttpConnect
 * needs just the host component.
 */
static const wchar_t CONNECTIVITY_HOST[] = L"github.com";

/**
 * Wide-string version of HTTP_USER_AGENT for WinHttpOpen.
 * Converted once at init time from the ASCII constant.
 */
static wchar_t w_user_agent[128];

/**
 * Resolve timeout value in milliseconds for WinHTTP. Converts a
 * positive millisecond value to the WinHTTP DWORD format. If the
 * value is zero or negative, returns zero (no timeout / infinite).
 *
 * @param timeout_ms  Timeout in milliseconds from config
 * @return             Timeout value suitable for WinHTTP resolve/connect/read
 */
static DWORD to_winhttp_timeout(int timeout_ms) {
    if (timeout_ms <= 0) {
        return 0;  /* No timeout - WinHTTP uses system defaults */
    }
    return (DWORD)timeout_ms;
}


/**
 * Read a specific HTTP response header from a WinHTTP request handle.
 * Looks up the header by name and returns its value as a string.
 *
 * @param h_request  WinHTTP request handle with a received response
 * @param header_name  Name of the header to read (ASCII)
 * @param value_out    Output buffer for the header value
 * @param value_len    Size of the output buffer
 * @return 0 on success, -1 if header not found or error
 */
static int read_header_value(HINTERNET h_request, const char *header_name,
                             char *value_out, int value_len) {
    wchar_t wide_name[256];
    MultiByteToWideChar(CP_ACP, 0, header_name, -1, wide_name, 256);

    wchar_t wide_value[512];
    DWORD wide_value_len = sizeof(wide_value) / sizeof(wchar_t);

    if (!WinHttpQueryHeaders(
            h_request,
            WINHTTP_QUERY_CUSTOM,
            wide_name,
            wide_value,
            &wide_value_len,
            WINHTTP_NO_HEADER_INDEX)) {
        return -1;
    }

    WideCharToMultiByte(CP_ACP, 0, wide_value, -1,
                        value_out, value_len, NULL, NULL);
    return 0;
}


/**
 * Parse rate limit headers from a WinHTTP response into the
 * rate_limit_info struct. Reads X-RateLimit-Remaining and
 * X-RateLimit-Reset headers.
 *
 * If headers are not present (e.g., connectivity check responses),
 * the struct's headers_parsed field remains zero.
 *
 * @param h_request  WinHTTP request handle
 * @param rate_info  Output: rate limit information
 */
static void parse_rate_limit_headers(HINTERNET h_request,
                                      rate_limit_info *rate_info) {
    char header_value[64];

    rate_info->remaining = 0;
    rate_info->reset_time = 0;
    rate_info->headers_parsed = 0;

    if (read_header_value(h_request, RATELIMIT_REMAINING_HEADER,
                          header_value, sizeof(header_value)) == 0) {
        {
            char *end_ptr = NULL;
            long val = strtol(header_value, &end_ptr, 10);
            rate_info->remaining = (end_ptr != header_value && *end_ptr == '\0')
                                   ? (int)val : 0;
        }
        rate_info->headers_parsed = 1;
    }

    if (read_header_value(h_request, RATELIMIT_RESET_HEADER,
                          header_value, sizeof(header_value)) == 0) {
        {
            char *end_ptr = NULL;
            long val = strtol(header_value, &end_ptr, 10);
            rate_info->reset_time = (end_ptr != header_value && *end_ptr == '\0')
                                    ? val : 0;
        }
        /* headers_parsed already set by the first header check above,
           or remains 1 if only remaining was found */
    }
}


/* ─── Shared HTTP Helpers (Windows) ───────────────────────── */

/**
 * Extract the path component from a full URL.
 * URL format: https://api.github.com/repos/owner/repo
 * Path:       /repos/owner/repo
 * Finds the third slash and returns everything from it onward.
 *
 * @param url            Full URL
 * @param path_out       Output: malloc'd wide-string path (caller must free)
 * @param path_len_out  Output: length of the extracted path (or NULL)
 * @return 0 on success, -1 if URL format is invalid
 */
static int extract_url_path(const char *url, wchar_t **path_out, int *path_len_out) {
    const char *path_start = NULL;
    int slash_count = 0;
    for (const char *p = url; *p != '\0'; p++) {
        if (*p == '/') {
            slash_count++;
            if (slash_count == 3) {
                path_start = p;
                break;
            }
        }
    }
    if (path_start == NULL) {
        return -1;
    }

    int path_len = (int)strlen(path_start);
    wchar_t *wide_path = (wchar_t *)malloc(
        ((size_t)path_len + 1) * sizeof(wchar_t));
    if (wide_path == NULL) {
        return -1;
    }
    MultiByteToWideChar(CP_ACP, 0, path_start, -1,
                        wide_path, path_len + 1);
    *path_out = wide_path;
    if (path_len_out != NULL) {
        *path_len_out = path_len;
    }
    return 0;
}

/**
 * Build the Authorization header from a token.
 * Uses AUTH_HEADER_PREFIX constant from constants.h.
 *
 * @param token          Bearer token string
 * @param auth_header_out Output: malloc'd wide-string header (caller must free)
 * @return 0 on success, -1 if token is empty
 */
static int build_auth_header(const char *token,
                              wchar_t **auth_header_out) {
    if (token == NULL || token[0] == '\0') {
        *auth_header_out = NULL;
        return 0;
    }

    char auth_ascii[MAX_URL_LEN];
    snprintf(auth_ascii, sizeof(auth_ascii),
             "Authorization: %s%s\r\n", AUTH_HEADER_PREFIX, token);

    wchar_t *w_header = (wchar_t *)malloc(
        MAX_URL_LEN * sizeof(wchar_t));
    if (w_header == NULL) {
        return -1;
    }
    MultiByteToWideChar(CP_ACP, 0, auth_ascii, -1,
                        w_header, MAX_URL_LEN);
    *auth_header_out = w_header;
    return 0;
}

/**
 * Holds all WinHTTP handles needed for an HTTP request.
 * Allows a shared setup function to prepare the request for any caller.
 */
typedef struct {
    HINTERNET h_connect;
    HINTERNET h_request;
    wchar_t *wide_path;     /* Malloc'd — caller must free */
    wchar_t *auth_header;   /* Malloc'd — caller must free, or NULL */
} http_request_handles;

/* Forward declaration — defined after close_http_request */
static void set_request_timeouts(HINTERNET h_request, int timeout_ms);

/**
 * Prepare an HTTP GET request to api.github.com.
 * Opens connection, extracts URL path, sets timeouts, builds auth header,
 * opens request handle. Caller must close handles and free wide_path/auth_header.
 *
 * @param timeout_ms   HTTP timeout
 * @param token        Bearer token (or NULL for unauthenticated)
 * @param url           Full URL with path
 * @param repo          Repository name (for error logging)
 * @param req_out       Output: populated request handles
 * @return 0 on success, -1 on any failure (handles are cleaned up)
 */
static int prepare_http_request(ghb_context *ctx, int timeout_ms, const char *token,
                                const char *url, const char *repo,
                                http_request_handles *req_out) {
    memset(req_out, 0, sizeof(*req_out));

    req_out->h_connect = WinHttpConnect(
        g_hSession, GITHUB_API_HOST,
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (req_out->h_connect == NULL) {
        DBG("prepare_http_request: WinHttpConnect FAILED (error %lu) for %s", GetLastError(), repo);
        ctx->logger->log_error(ctx, "network", repo,
                  "WinHttpConnect failed");
        return -1;
    }

    if (extract_url_path(url, &req_out->wide_path, NULL) != 0) {
        ctx->logger->log_error(ctx, "network", repo, "Cannot parse path from URL");
        WinHttpCloseHandle(req_out->h_connect);
        return -1;
    }

    req_out->h_request = WinHttpOpenRequest(
        req_out->h_connect, L"GET", req_out->wide_path,
        NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

    if (req_out->h_request == NULL) {
        DBG("prepare_http_request: WinHttpOpenRequest FAILED (error %lu) for %s", GetLastError(), repo);
        ctx->logger->log_error(ctx, "network", repo, "WinHttpOpenRequest failed");
        free(req_out->wide_path);
        WinHttpCloseHandle(req_out->h_connect);
        return -1;
    }

    /* Set timeouts */
    set_request_timeouts(req_out->h_request, timeout_ms);

    /* Build auth header */
    if (build_auth_header(token, &req_out->auth_header) != 0) {
        ctx->logger->log_error(ctx, "network", repo, "Failed to build auth header");
        free(req_out->wide_path);
        WinHttpCloseHandle(req_out->h_request);
        WinHttpCloseHandle(req_out->h_connect);
        return -1;
    }

    return 0;
}

/**
 * Close handles and free memory in an http_request_handles struct.
 */
static void close_http_request(ghb_context *ctx, http_request_handles *req) {
    (void)ctx;  /* Reserved for future debug logging — cleanup is silent */
    if (req->h_request) WinHttpCloseHandle(req->h_request);
    if (req->h_connect) WinHttpCloseHandle(req->h_connect);
    if (req->wide_path) free(req->wide_path);
    if (req->auth_header) free(req->auth_header);
    memset(req, 0, sizeof(*req));
}

/**
 * Apply RESOLVE/CONNECT/RECEIVE timeouts to a WinHTTP request handle.
 * Consolidates the 3x WinHttpSetOption pattern used by prepare_http_request()
 * and check_connectivity(). If timeout is zero or negative, no timeouts
 * are set (WinHTTP uses system defaults).
 *
 * @param h_request   WinHTTP request handle
 * @param timeout_ms  Timeout in milliseconds from config
 */
static void set_request_timeouts(HINTERNET h_request, int timeout_ms) {
    DWORD timeout = to_winhttp_timeout(timeout_ms);
    if (timeout > 0) {
        WinHttpSetOption(h_request, WINHTTP_OPTION_RESOLVE_TIMEOUT,
                         &timeout, sizeof(timeout));
        WinHttpSetOption(h_request, WINHTTP_OPTION_CONNECT_TIMEOUT,
                         &timeout, sizeof(timeout));
        WinHttpSetOption(h_request, WINHTTP_OPTION_RECEIVE_TIMEOUT,
                         &timeout, sizeof(timeout));
    }
}

/**
 * Sleep until the GitHub API rate limit reset window, checking for
 * shutdown events in 1-second intervals. Consolidates the duplicated
 * sleep-until-reset logic in get_default_branch() and download_repo_zip().
 *
 * Spec Section 7: "If rate-limited, log, fire a toast, sleep until
 * the reset window and retry." Maximum wait is 1 hour (3600 seconds).
 *
 * @param ctx           Context for logging and notifications
 * @param rate          Parsed rate limit info with reset_time
 * @param repo          Repository name for log/toast messages
 * @param operation     Description of the rate-limited operation (e.g., "metadata", "zip download")
 * @return 0 if sleep completed, -1 if conditions not met (no sleep performed)
 */
static int rate_limit_sleep(ghb_context *ctx, const rate_limit_info *rate,
                            const char *repo, const char *operation) {
    if (!rate->headers_parsed || rate->reset_time <= 0) {
        return -1;  /* No valid rate limit info - caller should handle */
    }

    time_t now = time(NULL);
    long wait_seconds = (long)(rate->reset_time - (long)now);
    if (wait_seconds <= 0 || wait_seconds >= 3600) {
        return -1;  /* Wait time invalid or too long - caller should handle */
    }

    char detail[256];
    snprintf(detail, sizeof(detail),
             "Rate limited on %s - sleeping %ld seconds until reset window",
             operation, wait_seconds);
    ctx->logger->log_event(ctx, LOG_WARNING, "network", repo, "RATE_LIMITED", detail);

    /* Toast with reset time so the operator knows when to expect recovery */
    char rl_toast[256];
    time_t reset_local = (time_t)rate->reset_time;
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S",
             localtime(&reset_local));
    snprintf(rl_toast, sizeof(rl_toast),
             "GitHub API rate limit - retrying at %s UTC", time_str);
    ctx->notify->toast_error(ctx, "Rate Limited", rl_toast);

    DBG("rate_limit_sleep: Sleeping %lds for %s (repo=%s)", wait_seconds, operation, repo);

    /* Sleep in 1-second intervals so we can be interrupted by shutdown */
#ifdef _WIN32
    for (long s = 0; s < wait_seconds; s++) {
        HANDLE h_evt = OpenEventA(SYNCHRONIZE, FALSE,
                                      BACKUP_SHUTDOWN_EVENT_NAME);
        if (h_evt != NULL) {
            DWORD wait_result = WaitForSingleObject(h_evt, 1000);
            CloseHandle(h_evt);
            if (wait_result == WAIT_OBJECT_0) {
                DBG("rate_limit_sleep: Shutdown requested during sleep");
                break;
            }
        }
        Sleep(1000);
    }
#else
    sleep((unsigned int)wait_seconds);
#endif

    return 0;
}


/* ─── Public Functions (Windows) ──────────────────────────── */

int network_init(ghb_context *ctx) {
    /*
     * WINHTTP_ACCESS_TYPE_NO_PROXY bypasses WPAD (Web Proxy Auto-Discovery).
     * WINHTTP_ACCESS_TYPE_DEFAULT_PROXY can block for 60+ seconds on
     * systems with slow or misconfigured proxy auto-detection - the user
     * sees the program hang after printing BACKUP_DIR with no output.
     * The backup tool connects directly to api.github.com and
     * codeload.github.com, so proxy detection is unnecessary.
     */

    DBG("network_init: Calling WinHttpOpen (NO_PROXY)...");

    MultiByteToWideChar(CP_ACP, 0, HTTP_USER_AGENT, -1, w_user_agent, 128);

    g_hSession = WinHttpOpen(
        w_user_agent,                          /* User agent */
        WINHTTP_ACCESS_TYPE_NO_PROXY,          /* Direct connection - no WPAD */
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (g_hSession == NULL) {
        ctx->logger->log_error(ctx, "network", NULL,
                  "WinHttpOpen failed - cannot initialize HTTP session");
        return -1;
    }

    ctx->logger->log_event(ctx, LOG_INFO, "network", NULL, "OK",
              "WinHTTP session initialized");
    return 0;
}


int check_connectivity(ghb_context *ctx, int timeout_ms) {
    if (g_hSession == NULL) {
        ctx->logger->log_error(ctx, "network", NULL,
                  "Cannot check connectivity - session not initialized");
        return 0;
    }


    /*
     * Use WinHttpConnect + WinHttpOpenRequest + WinHttpSendRequest with
     * per-request timeouts to implement the connectivity check with timeout.
     * WinHttpConnect alone has no timeout parameter - if DNS resolution
     * hangs, it blocks indefinitely. By opening a request and setting
     * RESOLVE/CONNECT/RECEIVE timeouts, we ensure the entire check
     * completes within the configured CONNECTIVITY_CHECK_TIMEOUT_MS.
     */
    DBG("check_connectivity: Connecting to %S:%d (timeout=%dms)...",
        CONNECTIVITY_HOST, INTERNET_DEFAULT_HTTPS_PORT, timeout_ms);

    HINTERNET h_connect = WinHttpConnect(
        g_hSession,
        CONNECTIVITY_HOST,
        INTERNET_DEFAULT_HTTPS_PORT,
        0
    );

    if (h_connect == NULL) {
        DWORD err = GetLastError();
        DBG("check_connectivity: WinHttpConnect FAILED (error %lu)", err);
        (void)err;
        ctx->logger->log_event(ctx, LOG_WARNING, "network", NULL, "FAILED",
                  "Connectivity check failed - cannot reach github.com");
        return 0;
    }

    HINTERNET h_request = WinHttpOpenRequest(
        h_connect,
        L"HEAD",                    /* HEAD request - no body download */
        L"/",                       /* Root path */
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );

    if (h_request == NULL) {
        DWORD err = GetLastError();
        DBG("check_connectivity: WinHttpOpenRequest FAILED (error %lu)", err);
        (void)err;
        WinHttpCloseHandle(h_connect);
        return 0;
    }

    /* Apply timeout to all phases (resolve, connect, receive) */
    set_request_timeouts(h_request, timeout_ms);

    /* Send the request - this is where the timeout applies */
    BOOL send_result = WinHttpSendRequest(
        h_request,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0
    );

    if (!send_result) {
        DWORD err = GetLastError();
        (void)err;
        DBG("check_connectivity: WinHttpSendRequest FAILED (error %lu)", err);
        DBG("check_connectivity: Timed out after %dms - no internet", timeout_ms);
        WinHttpCloseHandle(h_request);
        WinHttpCloseHandle(h_connect);
        ctx->logger->log_event(ctx, LOG_WARNING, "network", NULL, "FAILED",
                  "Connectivity check failed - cannot reach github.com");
        return 0;
    }

    /* We don't need to read the response - the fact that SendRequest
     * succeeded means DNS resolved, TCP connected, and TLS negotiated. */
    WinHttpCloseHandle(h_request);
    WinHttpCloseHandle(h_connect);

    DBG("check_connectivity: Connected to %S successfully", CONNECTIVITY_HOST);
    ctx->logger->log_event(ctx, LOG_INFO, "network", NULL, "OK",
              "Internet connectivity confirmed");
    return 1;
}


int http_get(ghb_context *ctx, const char *url, const char *token,
             char *response_body, int body_size,
             int *response_code, rate_limit_info *rate_info,
             int timeout_ms) {

    if (g_hSession == NULL) {
        ctx->logger->log_error(ctx, "network", NULL,
                  "Cannot send HTTP request - session not initialized");
        return -1;
    }

    http_request_handles req;
    if (prepare_http_request(ctx, timeout_ms, token, url, "metadata", &req) != 0) {
        return -1;
    }

    LPCWSTR additional_headers = req.auth_header;

    /* Send the request */
    DBG("http_get: Sending request...");
    BOOL send_result = WinHttpSendRequest(
        req.h_request,
        additional_headers,     /* Additional headers */
        -1L,                    /* Headers length (-1 = null-terminated) */
        WINHTTP_NO_REQUEST_DATA,
        0,                      /* Data length */
        0,                      /* Total length */
        0                       /* Context pointer */
    );

    if (!send_result) {
        DWORD err = GetLastError();
        DBG("http_get: WinHttpSendRequest FAILED (error %lu)", err);
        char err_detail[128];
        snprintf(err_detail, sizeof(err_detail),
                 "WinHttpSendRequest failed (error code: %lu)", err);
        ctx->logger->log_error(ctx, "network", NULL, err_detail);
        close_http_request(ctx, &req);
        return (err == ERROR_INTERNET_TIMEOUT) ? -2 : -1;
    }

    DBG("http_get: Request sent, waiting for response...");

    /* Receive the response */
    if (!WinHttpReceiveResponse(req.h_request, NULL)) {
        DWORD err = GetLastError();
        DBG("http_get: WinHttpReceiveResponse FAILED (error %lu)", err);
        char err_detail[128];
        snprintf(err_detail, sizeof(err_detail),
                 "WinHttpReceiveResponse failed (error code: %lu)", err);
        ctx->logger->log_error(ctx, "network", NULL, err_detail);
        close_http_request(ctx, &req);
        return (err == ERROR_INTERNET_TIMEOUT) ? -2 : -1;
    }

    /* Read the HTTP status code */
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    WinHttpQueryHeaders(req.h_request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status_code, &status_size,
                        WINHTTP_NO_HEADER_INDEX);

    DBG("http_get: Response received - HTTP %lu, reading body...", status_code);

    if (response_code != NULL) {
        *response_code = (int)status_code;
    }

    /* Parse rate limit headers before reading the body */
    if (rate_info != NULL) {
        parse_rate_limit_headers(req.h_request, rate_info);
    }

    /* Read the response body into the caller's buffer */
    int total_read = 0;
    DWORD bytes_available = 0;

    while (WinHttpQueryDataAvailable(req.h_request, &bytes_available)) {
        if (bytes_available == 0) {
            break;  /* End of response */
        }

        int remaining = body_size - total_read - 1;
        if (remaining <= 0) {
            ctx->logger->log_event(ctx, LOG_WARNING, "network", NULL, "TRUNCATED",
                      "HTTP response body exceeds buffer - data truncated");
            break;
        }

        DWORD bytes_to_read = (bytes_available < (DWORD)remaining)
                              ? bytes_available
                              : (DWORD)remaining;
        DWORD bytes_read = 0;

        if (!WinHttpReadData(req.h_request,
                            response_body + total_read,
                            bytes_to_read,
                            &bytes_read)) {
            ctx->logger->log_error(ctx, "network", NULL,
                      "WinHttpReadData failed while reading response body");
            break;
        }

        total_read += (int)bytes_read;
    }

    response_body[total_read] = '\0';

    DBG("http_get: Body read complete - %d bytes", total_read);

    close_http_request(ctx, &req);

    return 0;
}


void network_cleanup(ghb_context *ctx) {
    if (g_hSession != NULL) {
        WinHttpCloseHandle(g_hSession);
        g_hSession = NULL;
        ctx->logger->log_event(ctx, LOG_INFO, "network", NULL, "OK",
                  "WinHTTP session closed");
    }
}


#else

/* ================================================================
 * NON-WINDOWS STUBS
 *
 * Minimal stub implementations for compilation testing on non-Windows
 * platforms. The JSON parsing functions (below) are platform-independent
 * and work on all operating systems.
 * ================================================================ */

int network_init(ghb_context *ctx) {
    ctx->logger->log_event(ctx, LOG_INFO, "network", NULL, "INFO",
              "Network module initialized (non-Windows - HTTP stubs active)");
    return 0;
}

int check_connectivity(ghb_context *ctx, int timeout_ms) {
    (void)timeout_ms;
    ctx->logger->log_event(ctx, LOG_INFO, "network", NULL, "OK",
              "Connectivity check skipped (non-Windows stub)");
    return 1;  /* Assume connectivity on non-Windows */
}

int http_get(ghb_context *ctx, const char *url, const char *token,
             char *response_body, int body_size,
             int *response_code, rate_limit_info *rate_info,
             int timeout_ms) {
    (void)url; (void)token; (void)timeout_ms;
    if (response_body != NULL && body_size > 0) {
        response_body[0] = '\0';
    }
    if (response_code != NULL) {
        *response_code = 0;
    }
    if (rate_info != NULL) {
        rate_info->remaining = 0;
        rate_info->reset_time = 0;
        rate_info->headers_parsed = 0;
    }
    ctx->logger->log_event(ctx, LOG_WARNING, "network", NULL, "STUB",
              "HTTP GET is a stub on non-Windows platforms");
    return -1;
}

void network_cleanup(ghb_context *ctx) {
    ctx->logger->log_event(ctx, LOG_INFO, "network", NULL, "OK",
              "Network cleanup (non-Windows - no-op)");
}

#endif /* _WIN32 */


/* ================================================================
 * JSON PARSING FUNCTIONS (Platform-Independent)
 *
 * These functions extract fields from JSON strings without using
 * an external JSON library. They implement a minimal parser that
 * handles flat JSON objects as returned by the GitHub API for
 * single-repo metadata queries.
 *
 * Limitations:
 *   - Only handles flat JSON objects (no nested objects or arrays)
 *   - Returns the first occurrence of a key if it appears multiple times
 *   - String values must be quoted with double quotes
 *   - Integer values may be positive or negative
 *
 * These limitations are acceptable because the only JSON fields we
 * parse are "default_branch" (string) and rate limit values (integers)
 * from GitHub API responses - all flat, single-value fields.
 * ================================================================ */

/**
 * Advance the pointer past any whitespace characters.
 *
 * @param p  Pointer to a position in a string
 * @return   Pointer to the first non-whitespace character
 */
static const char *skip_whitespace(const char *p) {
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}


/**
 * Find a JSON key in a JSON string. Searches for the pattern
 * "key" (with quotes) in the string.
 *
 * @param json  JSON string to search
 * @param key   Key name to find (without quotes)
 * @return      Pointer to the opening quote of the found key,
 *              or NULL if not found
 */
static const char *find_json_key(const char *json, const char *key) {
    /*
     * Build the search pattern: "<key>"
     * We search for the quoted key name in the JSON string.
     * This is sufficient for flat JSON - it does not match
     * key names that appear inside string values.
     */
    size_t key_len = strlen(key);
    size_t pattern_len = key_len + 2;  /* +2 for surrounding quotes */

    char pattern[MAX_REPO_NAME_LEN + 2];
    if (pattern_len >= sizeof(pattern)) {
        return NULL;
    }
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    return strstr(json, pattern);
}


int parse_json_string(const char *json, const char *key,
                      char *value_out, int value_len) {
    if (json == NULL || key == NULL || value_out == NULL || value_len <= 0) {
        return -1;
    }

    const char *key_pos = find_json_key(json, key);
    if (key_pos == NULL) {
        return -1;
    }

    /* Advance past the key name (including closing quote) */
    const char *after_key = key_pos + strlen(key) + 2;

    /* Skip whitespace and the colon separator */
    after_key = skip_whitespace(after_key);
    if (*after_key != ':') {
        return -1;
    }
    after_key = skip_whitespace(after_key + 1);

    /* Expect a quoted string value */
    if (*after_key != '"') {
        return -1;
    }
    after_key++;  /* Skip opening quote */

    /* Copy characters until closing quote or end of buffer.
     * Handle JSON escape sequences: \", \\, \/, \n, \r, \t, \uXXXX.
     */
    int i = 0;
    while (*after_key != '\0' && *after_key != '"' && i < value_len - 1) {
        if (*after_key == '\\' && *(after_key + 1) != '\0') {
            after_key++;  /* Skip the backslash */
            switch (*after_key) {
                case '"':  value_out[i++] = '"';  break;
                case '\\': value_out[i++] = '\\'; break;
                case '/':  value_out[i++] = '/';  break;
                case 'n':  value_out[i++] = '\n';  break;
                case 'r':  value_out[i++] = '\r';  break;
                case 't':  value_out[i++] = '\t';  break;
                case 'u':  {
                    /* \uXXXX — copy 4 hex digits as-is (no full Unicode support needed) */
                    if (i + 4 < value_len) {
                        value_out[i++] = '\\';
                        value_out[i++] = 'u';
                        value_out[i++] = *(after_key + 1);
                        value_out[i++] = *(after_key + 2);
                        value_out[i++] = *(after_key + 3);
                        after_key += 3;  /* Skip the 3 digits we just copied */
                    }
                    break;
                }
                default:
                    /* Unknown escape — keep the character after backslash as-is */
                    value_out[i++] = *after_key;
                    break;
            }
        } else {
            value_out[i++] = *after_key;
        }
        after_key++;
    }
    value_out[i] = '\0';

    /*
     * If we stopped because of the buffer limit, advance past the
     * remaining characters to find the closing quote. This handles
     * truncation gracefully - the value is truncated but the parse
     * itself succeeded. The caller's responsibility to provide an
     * adequately-sized buffer.
     */
    while (*after_key != '\0' && *after_key != '"') {
        after_key++;
    }

    if (*after_key != '"') {
        return -1;  /* Truly unterminated string */
    }

    return 0;
}


int parse_json_int(const char *json, const char *key, int *value_out) {
    if (json == NULL || key == NULL || value_out == NULL) {
        return -1;
    }

    const char *key_pos = find_json_key(json, key);
    if (key_pos == NULL) {
        return -1;
    }

    /* Advance past the key name and colon */
    const char *after_key = key_pos + strlen(key) + 2;
    after_key = skip_whitespace(after_key);
    if (*after_key != ':') {
        return -1;
    }
    after_key = skip_whitespace(after_key + 1);

    /* Parse the integer value */
    char *end_ptr = NULL;
    long value = strtol(after_key, &end_ptr, 10);

    /*
     * Verify that strtol consumed at least one digit.
     * end_ptr should point past the digits.
     */
    if (end_ptr == after_key) {
        return -1;  /* No digits found */
    }

    *value_out = (int)value;
    return 0;
}


/* ================================================================
 * CONVENIENCE FUNCTIONS (Use platform-specific http_get internally)
 * ================================================================ */

#ifdef _WIN32

int get_default_branch(ghb_context *ctx, const char *owner, const char *repo,
                       const char *token, char *branch_out,
                       int branch_len, int timeout_ms) {
    char url[MAX_URL_LEN];
    snprintf(url, sizeof(url), "%s%s%s/%s",
             GITHUB_API_BASE, API_REPOS_PATH, owner, repo);

    int attempt;
    for (attempt = 0; attempt < 2; attempt++) {
        char response[MAX_HTTP_RESPONSE_LEN];
        int status_code = 0;
        rate_limit_info rate = {0, 0, 0};

        int result = http_get(ctx, url, token, response, sizeof(response),
                              &status_code, &rate, timeout_ms);

        if (result != 0) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "Network error querying repo metadata for %s/%s (result: %d)",
                     owner, repo, result);
            ctx->logger->log_error(ctx, "network", repo, detail);
            return -1;
        }

        if (status_code == HTTP_NOT_FOUND) {
            ctx->logger->log_event(ctx, LOG_WARNING, "network", repo, "NOT_FOUND",
                      "Repository does not exist or token lacks access");
            ctx->notify->toast_error(ctx, "Repository Not Found", repo);
            return HTTP_NOT_FOUND;
        }

        if (status_code == HTTP_UNAUTHORIZED || status_code == HTTP_FORBIDDEN) {
            ctx->logger->log_event(ctx, LOG_ERROR, "network", repo, "AUTH_ERROR",
                      "Token is invalid, expired, or lacks required scope");
            ctx->notify->toast_error(ctx, "Authentication Failed",
                       "Token is invalid or expired - check .env");
            return status_code;
        }

        if (status_code == HTTP_RATE_LIMITED) {
            if (attempt == 0) {
                /* Spec Section 7: sleep until the reset window and retry */
                if (rate_limit_sleep(ctx, &rate, repo, "metadata") == 0) {
                    continue;  /* Retry */
                }
            }
            ctx->logger->log_event(ctx, LOG_WARNING, "network", repo, "RATE_LIMITED",
                      "GitHub API rate limit reached");
            ctx->notify->toast_error(ctx, "Rate Limited", "GitHub API rate limit reached");
            return HTTP_RATE_LIMITED;
        }

        if (status_code != HTTP_OK) {
            char detail[128];
            snprintf(detail, sizeof(detail),
                     "Unexpected HTTP status %d for %s/%s",
                     status_code, owner, repo);
            ctx->logger->log_error(ctx, "network", repo, detail);
            return status_code;
        }

        /* Extract the default_branch field from the JSON response */
        if (parse_json_string(response, JSON_FIELD_DEFAULT_BRANCH,
                              branch_out, branch_len) != 0) {
            ctx->logger->log_error(ctx, "network", repo,
                      "Failed to parse default_branch from API response");
            return -1;
        }

        ctx->logger->log_event(ctx, LOG_INFO, "network", repo, "OK",
                  "Default branch resolved");
        return 0;
    }

    /* Should not reach here - the loop only continues on rate limit retry */
    return -1;
}


/*
 * Stream the HTTP response body to an open file in chunks.
 * Logs progress every 64KB (dev-phase visibility in viewer).
 *
 * The caller owns fp and req on ALL return paths — this function
 * only streams; it does not close anything (Rule 37: the function
 * that opened a resource is the one that closes it). On read or
 * write failure, this function returns the error code and the
 * caller is responsible for closing fp and req.
 *
 * Returns 0 on success, -1 on WinHttpReadData failure, -3 on write
 * failure (possible disk full).
 */
static int stream_zip_to_file(ghb_context *ctx, http_request_handles *req,
                              FILE *fp, const char *repo,
                              unsigned long *out_total_downloaded)
{
    DWORD bytes_available = 0;
    unsigned char chunk[HTTP_READ_CHUNK_SIZE];
    unsigned long total_downloaded = 0;
    unsigned long last_progress_log = 0;  /* For periodic progress logging */

    *out_total_downloaded = 0;

    while (WinHttpQueryDataAvailable(req->h_request, &bytes_available)) {
        if (bytes_available == 0) {
            break;  /* End of response */
        }

        /*
         * Check for shutdown request mid-download (Spec override per Sir R153).
         * Original spec said "finish current download" — Sir wants immediate kill.
         * Safe because atomic_write pattern means .tmp is deleted, old .zip intact.
         */
        if (ctx->should_stop && ctx->should_stop()) {
            DBG("stream_zip_to_file: Shutdown requested mid-download (repo=%s)", repo);
            return -5;  /* Shutdown requested — caller cleans up .tmp */
        }

        DWORD to_read = (bytes_available < HTTP_READ_CHUNK_SIZE)
                        ? bytes_available
                        : HTTP_READ_CHUNK_SIZE;
        DWORD bytes_read = 0;

        if (!WinHttpReadData(req->h_request, chunk, to_read, &bytes_read)) {
            ctx->logger->log_error(ctx, "network", repo,
                      "WinHttpReadData failed during zip download");
            return -1;
        }

        size_t written = fwrite(chunk, 1, bytes_read, fp);
        if (written != bytes_read) {
            ctx->logger->log_error(ctx, "network", repo,
                      "File write error during zip download - possible disk full");
            return -3;  /* Disk full or write error */
        }

        total_downloaded += bytes_read;

        /*
         * DEV PHASE: Log download progress every 64KB.
         * This makes the daemon's download activity visible in the viewer
         * (which tails the log file). Without this, the viewer shows
         * nothing during downloads that can take 30+ seconds per repo.
         */
        if (total_downloaded - last_progress_log >= 65536) {
            DBG("download_repo_zip: Progress %lu KB downloaded (repo=%s)",
                total_downloaded / 1024, repo);
            last_progress_log = total_downloaded;
        }
    }

    *out_total_downloaded = total_downloaded;
    return 0;
}


/*
 * Attempt a single zip download. Builds the HTTP request, sends it,
 * receives the response, checks the status code, opens the output
 * file, streams the body, and cleans up the request handles and
 * file pointer on every return path.
 *
 * On HTTP 429, parses the rate limit headers into *out_rate, sets
 * *was_rate_limited to 1, and returns -4 WITHOUT sleeping — the
 * caller (download_repo_zip) decides whether to retry and performs
 * the rate_limit_sleep. This function does NOT log the error or
 * fire the toast on 429; the caller does that when it gives up.
 *
 * Returns:
 *   0  on success
 *  -1  on generic error (request prep, send, receive, non-200
 *      non-429, file open, read failure)
 *  -2  on timeout (ERROR_INTERNET_TIMEOUT)
 *  -3  on disk full / write error (fwrite mismatch)
 *  -4  on rate limited (HTTP 429) — caller may retry
 */
static int try_download_once(ghb_context *ctx, const char *url, const char *token,
                             const char *repo, const char *output_path,
                             int timeout_ms, int *was_rate_limited,
                             rate_limit_info *out_rate)
{
    http_request_handles req;
    if (prepare_http_request(ctx, timeout_ms, token, url, repo, &req) != 0) {
        return -1;
    }

    LPCWSTR additional_headers = req.auth_header;

    /* Send request */
    DBG("download_repo_zip: Sending request...");

    if (!WinHttpSendRequest(req.h_request, additional_headers, -1L,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        DWORD err = GetLastError();
        DBG("download_repo_zip: WinHttpSendRequest FAILED (error %lu)", err);
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "WinHttpSendRequest failed for zip download (error: %lu)",
                 err);
        ctx->logger->log_error(ctx, "network", repo, detail);
        close_http_request(ctx, &req);
        return (err == ERROR_INTERNET_TIMEOUT) ? -2 : -1;
    }

    DBG("download_repo_zip: Request sent, waiting for response...");

    /* Receive response */
    if (!WinHttpReceiveResponse(req.h_request, NULL)) {
        DWORD err = GetLastError();
        DBG("download_repo_zip: WinHttpReceiveResponse FAILED (error %lu)", err);
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "WinHttpReceiveResponse failed for zip download (error: %lu)",
                 err);
        ctx->logger->log_error(ctx, "network", repo, detail);
        close_http_request(ctx, &req);
        return (err == ERROR_INTERNET_TIMEOUT) ? -2 : -1;
    }

    /* Check status code */
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    WinHttpQueryHeaders(req.h_request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status_code, &status_size,
                        WINHTTP_NO_HEADER_INDEX);

    DBG("download_repo_zip: Response - HTTP %lu", status_code);

    if (status_code != HTTP_OK) {
        DBG("download_repo_zip: Non-200 status (HTTP %lu)", status_code);

        if (status_code == HTTP_RATE_LIMITED) {
            /*
             * Spec Section 7: caller sleeps until the reset window and
             * retries. Parse rate limit headers into *out_rate so the
             * caller can sleep. The caller (download_repo_zip) decides
             * whether to retry, performs the rate_limit_sleep, and
             * logs/fires-toast only when it gives up. This function
             * closes the request and returns -4 without sleeping.
             */
            parse_rate_limit_headers(req.h_request, out_rate);
            close_http_request(ctx, &req);
            *was_rate_limited = 1;
            return -4;  /* Rate limited — caller may retry */
        }

        char detail[128];
        snprintf(detail, sizeof(detail),
                 "Zip download returned HTTP %lu for %s",
                 status_code, repo);
        ctx->logger->log_error(ctx, "network", repo, detail);
        close_http_request(ctx, &req);
        return -1;
    }

    /* Open output file for writing */
    FILE *fp = fopen(output_path, "wb");
    if (fp == NULL) {
        ctx->logger->log_error(ctx, "network", repo,
                  "Cannot open output file for zip download");
        close_http_request(ctx, &req);
        return -1;
    }

    /*
     * Stream response body to file in chunks. stream_zip_to_file does
     * NOT close fp or req — this function owns both and closes them on
     * every return path (Rule 37: cleanup is the caller's job).
     */
    unsigned long total_downloaded = 0;
    int stream_result = stream_zip_to_file(ctx, &req, fp, repo,
                                           &total_downloaded);

    if (stream_result == 0) {
        DBG("download_repo_zip: Stream complete - total %lu KB (repo=%s)",
            total_downloaded / 1024, repo);
    }

    fclose(fp);
    close_http_request(ctx, &req);

    if (stream_result != 0) {
        return stream_result;  /* -1 (read failure) or -3 (disk full) */
    }

    ctx->logger->log_event(ctx, LOG_INFO, "network", repo, "OK",
              "Zip archive downloaded successfully");
    return 0;
}


int download_repo_zip(ghb_context *ctx, const char *owner, const char *repo,
                      const char *branch, const char *token,
                      const char *output_path, int timeout_ms) {
    DBG("download_repo_zip: Downloading %s/%s (branch: %s) to %s",
        owner, repo, branch, output_path);

    char url[MAX_URL_LEN];
    snprintf(url, sizeof(url), "%s%s%s/%s%s%s",
             GITHUB_API_BASE, API_REPOS_PATH,
             owner, repo, API_ZIPBALL_PATH, branch);


    if (g_hSession == NULL) {
        ctx->logger->log_error(ctx, "network", repo,
                  "Cannot download - session not initialized");
        return -1;
    }

    /*
     * Retry loop for rate limiting (Spec Section 7).
     * On HTTP 429 with a valid X-RateLimit-Reset header, sleep until
     * the reset window and retry the download once. All other errors
     * (-1, -2, -3) return immediately — only rate limiting triggers
     * a retry, matching the original single-function behavior.
     */
    int attempt;
    for (attempt = 0; attempt < 2; attempt++) {
        int was_rate_limited = 0;
        rate_limit_info rate = {0, 0, 0};

        int result = try_download_once(ctx, url, token, repo, output_path,
                                       timeout_ms, &was_rate_limited, &rate);

        if (result == 0) {
            return 0;  /* Success */
        }

        if (result == -4 && was_rate_limited) {
            /*
             * Spec Section 7: on the first 429, sleep until the reset
             * window and retry. rate_limit_sleep logs the WARNING event
             * and fires the "Rate Limited" toast with the reset time.
             * try_download_once already parsed *rate and closed the
             * request before returning -4.
             */
            if (attempt == 0 &&
                rate_limit_sleep(ctx, &rate, repo, "zip download") == 0) {
                continue;  /* Retry the download */
            }

            /*
             * Either this is the second 429 (attempt == 1), or
             * rate_limit_sleep could not sleep (invalid/missing
             * headers, or wait window too long). Log the error, fire
             * the toast with the repo name, and give up. This matches
             * the original give-up path exactly.
             */
            char detail[128];
            snprintf(detail, sizeof(detail),
                     "Zip download returned HTTP %lu for %s",
                     (unsigned long)HTTP_RATE_LIMITED, repo);
            ctx->logger->log_error(ctx, "network", repo, detail);
            ctx->notify->toast_error(ctx, "Rate Limited", repo);
            return -4;  /* Rate limited — maps to BACKUP_RATE_LIMITED */
        }

        /*
         * Any other non-zero result (-1, -2, -3): return immediately.
         * Only rate limiting triggers retry.
         */
        return result;
    }  /* end retry loop */

    /* Should not reach here - the loop only continues on rate limit retry */
    return -1;
}


#else

/* Non-Windows stubs for the convenience functions */

int get_default_branch(ghb_context *ctx, const char *owner, const char *repo,
                       const char *token, char *branch_out,
                       int branch_len, int timeout_ms) {
    (void)owner; (void)repo; (void)token; (void)timeout_ms;
    if (branch_out != NULL && branch_len > 0) {
        branch_out[0] = '\0';
    }
    ctx->logger->log_event(ctx, LOG_WARNING, "network", repo, "STUB",
              "get_default_branch is a stub on non-Windows platforms");
    return -1;
}

int download_repo_zip(ghb_context *ctx, const char *owner, const char *repo,
                      const char *branch, const char *token,
                      const char *output_path, int timeout_ms) {
    (void)owner; (void)repo; (void)branch;
    (void)token; (void)output_path; (void)timeout_ms;
    ctx->logger->log_event(ctx, LOG_WARNING, "network", NULL, "STUB",
              "download_repo_zip is a stub on non-Windows platforms");
    /* Return -1 (network error) to match Windows error code semantics.
     * Other codes: -2 (timeout), -3 (disk full), -4 (rate limited). */
    return -1;
}

#endif /* _WIN32 */
