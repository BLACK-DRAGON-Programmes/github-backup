/**
 * network.c — Network implementation for the GitHub Backup Script.
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
#include "logger.h"
#include "notify.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

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
 * Host name for the connectivity check. The pre-cycle internet
 * probe connects to github.com to verify network availability.
 */
static const wchar_t *CONNECTIVITY_HOST = L"github.com";

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
        return 0;  /* No timeout — WinHTTP uses system defaults */
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
        rate_info->remaining = atoi(header_value);
        rate_info->headers_parsed = 1;
    }

    if (read_header_value(h_request, RATELIMIT_RESET_HEADER,
                          header_value, sizeof(header_value)) == 0) {
        rate_info->reset_time = atol(header_value);
        /* headers_parsed already set by the first header check above,
           or remains 1 if only remaining was found */
    }
}


/* ─── Public Functions (Windows) ──────────────────────────── */

int network_init(void) {
    g_hSession = WinHttpOpen(
        L"GitHubBackup/1.0",                 /* User agent */
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,     /* Use system proxy settings */
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (g_hSession == NULL) {
        log_error("network", NULL,
                  "WinHttpOpen failed — cannot initialize HTTP session");
        return -1;
    }

    log_event(LOG_INFO, "network", NULL, "OK",
              "WinHTTP session initialized");
    return 0;
}


int check_connectivity(int timeout_ms) {
    (void)timeout_ms;  /* WinHttpConnect has no timeout parameter — resolved at request level */
    if (g_hSession == NULL) {
        log_error("network", NULL,
                  "Cannot check connectivity — session not initialized");
        return 0;
    }

    /*
     * Connect to github.com. This verifies DNS resolution and TCP
     * connection without sending a full HTTP request — the lightest
     * possible connectivity probe.
     */
    HINTERNET h_connect = WinHttpConnect(
        g_hSession,
        CONNECTIVITY_HOST,
        INTERNET_DEFAULT_HTTPS_PORT,
        0
    );

    if (h_connect == NULL) {
        log_event(LOG_WARNING, "network", NULL, "FAILED",
                  "Connectivity check failed — cannot reach github.com");
        return 0;
    }

    WinHttpCloseHandle(h_connect);
    log_event(LOG_INFO, "network", NULL, "OK",
              "Internet connectivity confirmed");
    return 1;
}


int http_get(const char *url, const char *token,
             char *response_body, int body_size,
             int *response_code, rate_limit_info *rate_info,
             int timeout_ms) {

    if (g_hSession == NULL) {
        log_error("network", NULL,
                  "Cannot send HTTP request — session not initialized");
        return -1;
    }

    /*
     * Determine which host to connect to based on the URL.
     * All API requests go to api.github.com.
     * The connectivity check uses github.com, but it does not
     * call http_get() — it uses check_connectivity() directly.
     */
    HINTERNET h_connect = WinHttpConnect(
        g_hSession,
        GITHUB_API_HOST,
        INTERNET_DEFAULT_HTTPS_PORT,
        0
    );

    if (h_connect == NULL) {
        log_error("network", NULL,
                  "WinHttpConnect to api.github.com failed");
        return -1;
    }

    /*
     * Extract the path component from the full URL.
     * URL format: https://api.github.com/repos/owner/repo
     * Path:       /repos/owner/repo
     *
     * Find the third slash (after "https://") to get the path.
     */
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
        log_error("network", NULL, "Cannot parse path from URL");
        WinHttpCloseHandle(h_connect);
        return -1;
    }

    /* Convert path to wide string for WinHTTP */
    int path_len = (int)strlen(path_start);
    wchar_t *wide_path = (wchar_t *)malloc(
        ((size_t)path_len + 1) * sizeof(wchar_t));
    if (wide_path == NULL) {
        log_error("network", NULL,
                  "Memory allocation failed for URL path");
        WinHttpCloseHandle(h_connect);
        return -1;
    }
    MultiByteToWideChar(CP_ACP, 0, path_start, -1,
                        wide_path, path_len + 1);

    /* Open the request handle */
    HINTERNET h_request = WinHttpOpenRequest(
        h_connect,
        L"GET",                      /* HTTP method */
        wide_path,                   /* Request path */
        NULL,                        /* HTTP version (NULL = HTTP/1.1) */
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, /* Accept headers */
        WINHTTP_FLAG_SECURE          /* HTTPS required for api.github.com */
    );

    free(wide_path);

    if (h_request == NULL) {
        log_error("network", NULL,
                  "WinHttpOpenRequest failed");
        WinHttpCloseHandle(h_connect);
        return -1;
    }

    /*
     * Set timeouts for this request. WinHTTP supports separate
     * resolve, connect, and send timeouts. We apply the same
     * timeout value to all three phases for simplicity.
     */
    DWORD timeout = to_winhttp_timeout(timeout_ms);
    if (timeout > 0) {
        WinHttpSetOption(h_request, WINHTTP_OPTION_RESOLVE_TIMEOUT,
                         &timeout, sizeof(timeout));
        WinHttpSetOption(h_request, WINHTTP_OPTION_CONNECT_TIMEOUT,
                         &timeout, sizeof(timeout));
        WinHttpSetOption(h_request, WINHTTP_OPTION_RECEIVE_TIMEOUT,
                         &timeout, sizeof(timeout));
    }

    /*
     * Build the Authorization header if a token is provided.
     * Format: "Authorization: Bearer <token>"
     *
     * IMPORTANT: The header must be built as a char string FIRST,
     * then converted to wchar_t. Writing ASCII bytes into a wchar_t
     * buffer via snprintf corrupts the data because wchar_t is 2 bytes
     * — writing one char per byte misaligns the wchar_t boundaries.
     * Using separate char and wchar_t buffers avoids this aliasing bug.
     */
    LPCWSTR additional_headers = NULL;
    char auth_ascii[MAX_URL_LEN];
    wchar_t auth_header[MAX_URL_LEN];

    if (token != NULL && token[0] != '\0') {
        snprintf(auth_ascii, sizeof(auth_ascii),
                 "Authorization: Bearer %s\r\n", token);
        /*
         * Convert the ASCII header string to wide string.
         * The header string is ASCII-compatible, so MultiByteToWideChar
         * with CP_ACP is safe. Separate source and destination buffers
         * prevent the overlapping-buffer aliasing bug.
         */
        MultiByteToWideChar(CP_ACP, 0, auth_ascii, -1,
                            auth_header, MAX_URL_LEN);
        additional_headers = auth_header;
    }

    /* Send the request */
    BOOL send_result = WinHttpSendRequest(
        h_request,
        additional_headers,     /* Additional headers */
        -1L,                    /* Headers length (-1 = null-terminated) */
        WINHTTP_NO_REQUEST_DATA,
        0,                      /* Data length */
        0,                      /* Total length */
        0                       /* Context pointer */
    );

    if (!send_result) {
        DWORD err = GetLastError();
        char err_detail[128];
        snprintf(err_detail, sizeof(err_detail),
                 "WinHttpSendRequest failed (error code: %lu)", err);
        log_error("network", NULL, err_detail);
        WinHttpCloseHandle(h_request);
        WinHttpCloseHandle(h_connect);
        return (err == ERROR_INTERNET_TIMEOUT) ? -2 : -1;
    }

    /* Receive the response */
    if (!WinHttpReceiveResponse(h_request, NULL)) {
        DWORD err = GetLastError();
        char err_detail[128];
        snprintf(err_detail, sizeof(err_detail),
                 "WinHttpReceiveResponse failed (error code: %lu)", err);
        log_error("network", NULL, err_detail);
        WinHttpCloseHandle(h_request);
        WinHttpCloseHandle(h_connect);
        return (err == ERROR_INTERNET_TIMEOUT) ? -2 : -1;
    }

    /* Read the HTTP status code */
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    WinHttpQueryHeaders(h_request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status_code, &status_size,
                        WINHTTP_NO_HEADER_INDEX);

    if (response_code != NULL) {
        *response_code = (int)status_code;
    }

    /* Parse rate limit headers before reading the body */
    if (rate_info != NULL) {
        parse_rate_limit_headers(h_request, rate_info);
    }

    /* Read the response body into the caller's buffer */
    int total_read = 0;
    DWORD bytes_available = 0;

    while (WinHttpQueryDataAvailable(h_request, &bytes_available)) {
        if (bytes_available == 0) {
            break;  /* End of response */
        }

        int remaining = body_size - total_read - 1;
        if (remaining <= 0) {
            log_event(LOG_WARNING, "network", NULL, "TRUNCATED",
                      "HTTP response body exceeds buffer — data truncated");
            break;
        }

        DWORD bytes_to_read = (bytes_available < (DWORD)remaining)
                              ? bytes_available
                              : (DWORD)remaining;
        DWORD bytes_read = 0;

        if (!WinHttpReadData(h_request,
                            response_body + total_read,
                            bytes_to_read,
                            &bytes_read)) {
            log_error("network", NULL,
                      "WinHttpReadData failed while reading response body");
            break;
        }

        total_read += (int)bytes_read;
    }

    response_body[total_read] = '\0';

    WinHttpCloseHandle(h_request);
    WinHttpCloseHandle(h_connect);

    return 0;
}


void network_cleanup(void) {
    if (g_hSession != NULL) {
        WinHttpCloseHandle(g_hSession);
        g_hSession = NULL;
        log_event(LOG_INFO, "network", NULL, "OK",
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

int network_init(void) {
    log_event(LOG_INFO, "network", NULL, "INFO",
              "Network module initialized (non-Windows — HTTP stubs active)");
    return 0;
}

int check_connectivity(int timeout_ms) {
    (void)timeout_ms;
    log_event(LOG_INFO, "network", NULL, "OK",
              "Connectivity check skipped (non-Windows stub)");
    return 1;  /* Assume connectivity on non-Windows */
}

int http_get(const char *url, const char *token,
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
    log_event(LOG_WARNING, "network", NULL, "STUB",
              "HTTP GET is a stub on non-Windows platforms");
    return -1;
}

void network_cleanup(void) {
    log_event(LOG_INFO, "network", NULL, "OK",
              "Network cleanup (non-Windows — no-op)");
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
 * from GitHub API responses — all flat, single-value fields.
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
     * This is sufficient for flat JSON — it does not match
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

    /* Copy characters until closing quote or end of buffer */
    int i = 0;
    while (*after_key != '\0' && *after_key != '"' && i < value_len - 1) {
        value_out[i] = *after_key;
        i++;
        after_key++;
    }
    value_out[i] = '\0';

    /*
     * If we stopped because of the buffer limit, advance past the
     * remaining characters to find the closing quote. This handles
     * truncation gracefully — the value is truncated but the parse
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

int get_default_branch(const char *owner, const char *repo,
                       const char *token, char *branch_out,
                       int branch_len, int timeout_ms) {
    char url[MAX_URL_LEN];
    snprintf(url, sizeof(url), "%s%s%s/%s",
             GITHUB_API_BASE, API_REPOS_PATH, owner, repo);

    char response[MAX_HTTP_RESPONSE_LEN];
    int status_code = 0;
    rate_limit_info rate = {0, 0, 0};

    int result = http_get(url, token, response, sizeof(response),
                          &status_code, &rate, timeout_ms);

    if (result != 0) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Network error querying repo metadata for %s/%s (result: %d)",
                 owner, repo, result);
        log_error("network", repo, detail);
        return -1;
    }

    if (status_code == HTTP_NOT_FOUND) {
        log_event(LOG_WARNING, "network", repo, "NOT_FOUND",
                  "Repository does not exist or token lacks access");
        toast_error("Repository Not Found", repo);
        return HTTP_NOT_FOUND;
    }

    if (status_code == HTTP_UNAUTHORIZED || status_code == HTTP_FORBIDDEN) {
        log_event(LOG_ERROR, "network", repo, "AUTH_ERROR",
                  "Token is invalid, expired, or lacks required scope");
        toast_error("Authentication Failed",
                   "Token is invalid or expired — check .env");
        return status_code;
    }

    if (status_code == HTTP_RATE_LIMITED) {
        log_event(LOG_WARNING, "network", repo, "RATE_LIMITED",
                  "GitHub API rate limit reached");
        toast_error("Rate Limited", "GitHub API rate limit reached");
        return HTTP_RATE_LIMITED;
    }

    if (status_code != HTTP_OK) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "Unexpected HTTP status %d for %s/%s",
                 status_code, owner, repo);
        log_error("network", repo, detail);
        return status_code;
    }

    /* Extract the default_branch field from the JSON response */
    if (parse_json_string(response, JSON_FIELD_DEFAULT_BRANCH,
                          branch_out, branch_len) != 0) {
        log_error("network", repo,
                  "Failed to parse default_branch from API response");
        return -1;
    }

    log_event(LOG_INFO, "network", repo, "OK",
              "Default branch resolved");
    return 0;
}


int download_repo_zip(const char *owner, const char *repo,
                      const char *branch, const char *token,
                      const char *output_path, int timeout_ms) {
    char url[MAX_URL_LEN];
    snprintf(url, sizeof(url), "%s%s%s/%s%s%s",
             GITHUB_API_BASE, API_REPOS_PATH,
             owner, repo, API_ZIPBALL_PATH, branch);

    if (g_hSession == NULL) {
        log_error("network", repo,
                  "Cannot download — session not initialized");
        return -1;
    }

    /* Connect to the API host */
    HINTERNET h_connect = WinHttpConnect(
        g_hSession, GITHUB_API_HOST,
        INTERNET_DEFAULT_HTTPS_PORT, 0);

    if (h_connect == NULL) {
        log_error("network", repo,
                  "WinHttpConnect failed for zip download");
        return -1;
    }

    /* Extract path from URL */
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
        log_error("network", repo, "Cannot parse path from download URL");
        WinHttpCloseHandle(h_connect);
        return -1;
    }

    int path_len = (int)strlen(path_start);
    wchar_t *wide_path = (wchar_t *)malloc(
        ((size_t)path_len + 1) * sizeof(wchar_t));
    if (wide_path == NULL) {
        log_error("network", repo,
                  "Memory allocation failed for download URL path");
        WinHttpCloseHandle(h_connect);
        return -1;
    }
    MultiByteToWideChar(CP_ACP, 0, path_start, -1,
                        wide_path, path_len + 1);

    HINTERNET h_request = WinHttpOpenRequest(
        h_connect, L"GET", wide_path,
        NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

    free(wide_path);

    if (h_request == NULL) {
        log_error("network", repo,
                  "WinHttpOpenRequest failed for zip download");
        WinHttpCloseHandle(h_connect);
        return -1;
    }

    /* Set timeouts */
    DWORD timeout = to_winhttp_timeout(timeout_ms);
    if (timeout > 0) {
        WinHttpSetOption(h_request, WINHTTP_OPTION_RESOLVE_TIMEOUT,
                         &timeout, sizeof(timeout));
        WinHttpSetOption(h_request, WINHTTP_OPTION_CONNECT_TIMEOUT,
                         &timeout, sizeof(timeout));
        WinHttpSetOption(h_request, WINHTTP_OPTION_RECEIVE_TIMEOUT,
                         &timeout, sizeof(timeout));
    }

    /*
     * Build Authorization header if a token is provided.
     * Uses separate char and wchar_t buffers to avoid the
     * overlapping-buffer aliasing bug (see http_get above).
     */
    LPCWSTR additional_headers = NULL;
    char auth_ascii[MAX_URL_LEN];
    wchar_t auth_header[MAX_URL_LEN];

    if (token != NULL && token[0] != '\0') {
        snprintf(auth_ascii, sizeof(auth_ascii),
                 "Authorization: Bearer %s\r\n", token);
        MultiByteToWideChar(CP_ACP, 0, auth_ascii, -1,
                            auth_header, MAX_URL_LEN);
        additional_headers = auth_header;
    }

    /* Send request */
    if (!WinHttpSendRequest(h_request, additional_headers, -1L,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        DWORD err = GetLastError();
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "WinHttpSendRequest failed for zip download (error: %lu)",
                 err);
        log_error("network", repo, detail);
        WinHttpCloseHandle(h_request);
        WinHttpCloseHandle(h_connect);
        return (err == ERROR_INTERNET_TIMEOUT) ? -2 : -1;
    }

    /* Receive response */
    if (!WinHttpReceiveResponse(h_request, NULL)) {
        DWORD err = GetLastError();
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "WinHttpReceiveResponse failed for zip download (error: %lu)",
                 err);
        log_error("network", repo, detail);
        WinHttpCloseHandle(h_request);
        WinHttpCloseHandle(h_connect);
        return (err == ERROR_INTERNET_TIMEOUT) ? -2 : -1;
    }

    /* Check status code */
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    WinHttpQueryHeaders(h_request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status_code, &status_size,
                        WINHTTP_NO_HEADER_INDEX);

    if (status_code != HTTP_OK) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "Zip download returned HTTP %lu for %s",
                 status_code, repo);
        log_error("network", repo, detail);

        if (status_code == HTTP_RATE_LIMITED) {
            toast_error("Rate Limited", repo);
        }

        WinHttpCloseHandle(h_request);
        WinHttpCloseHandle(h_connect);
        return -1;
    }

    /* Open output file for writing */
    FILE *fp = fopen(output_path, "wb");
    if (fp == NULL) {
        log_error("network", repo,
                  "Cannot open output file for zip download");
        WinHttpCloseHandle(h_request);
        WinHttpCloseHandle(h_connect);
        return -1;
    }

    /* Stream response body to file in chunks */
    DWORD bytes_available = 0;
    unsigned char chunk[HTTP_READ_CHUNK_SIZE];

    while (WinHttpQueryDataAvailable(h_request, &bytes_available)) {
        if (bytes_available == 0) {
            break;  /* End of response */
        }

        DWORD to_read = (bytes_available < HTTP_READ_CHUNK_SIZE)
                        ? bytes_available
                        : HTTP_READ_CHUNK_SIZE;
        DWORD bytes_read = 0;

        if (!WinHttpReadData(h_request, chunk, to_read, &bytes_read)) {
            log_error("network", repo,
                      "WinHttpReadData failed during zip download");
            fclose(fp);
            WinHttpCloseHandle(h_request);
            WinHttpCloseHandle(h_connect);
            return -1;
        }

        size_t written = fwrite(chunk, 1, bytes_read, fp);
        if (written != bytes_read) {
            log_error("network", repo,
                      "File write error during zip download — possible disk full");
            fclose(fp);
            WinHttpCloseHandle(h_request);
            WinHttpCloseHandle(h_connect);
            return -1;
        }
    }

    fclose(fp);
    WinHttpCloseHandle(h_request);
    WinHttpCloseHandle(h_connect);

    log_event(LOG_INFO, "network", repo, "OK",
              "Zip archive downloaded successfully");
    return 0;
}


#else

/* Non-Windows stubs for the convenience functions */

int get_default_branch(const char *owner, const char *repo,
                       const char *token, char *branch_out,
                       int branch_len, int timeout_ms) {
    (void)owner; (void)repo; (void)token; (void)timeout_ms;
    if (branch_out != NULL && branch_len > 0) {
        branch_out[0] = '\0';
    }
    log_event(LOG_WARNING, "network", repo, "STUB",
              "get_default_branch is a stub on non-Windows platforms");
    return -1;
}

int download_repo_zip(const char *owner, const char *repo,
                      const char *branch, const char *token,
                      const char *output_path, int timeout_ms) {
    (void)owner; (void)repo; (void)branch;
    (void)token; (void)output_path; (void)timeout_ms;
    log_event(LOG_WARNING, "network", NULL, "STUB",
              "download_repo_zip is a stub on non-Windows platforms");
    return -1;
}

#endif /* _WIN32 */
