/**
 * constants.h — Compile-time constants for the GitHub Backup Script.
 *
 * This header is the root of the dependency tree. Every source module
 * includes this file for buffer sizes, HTTP status codes, API endpoints,
 * header names, and environment variable name lookups.
 *
 * === CONSTANT SPLIT ===
 *
 * Constants are divided into two categories:
 *
 *   1. CONFIGURABLE values — stored in .env, read at runtime by the
 *      config module. Changes take effect on the next cycle without
 *      recompilation. These are NOT defined here. See env.example for
 *      the full list: BACKUP_DIR, CYCLE_INTERVAL_SECONDS, HTTP_TIMEOUT_MS,
 *      CONNECTIVITY_CHECK_TIMEOUT_MS, LOG_MAX_SIZE_BYTES.
 *
 *   2. NON-CONFIGURABLE values — defined here as preprocessor constants.
 *      These are protocol definitions (HTTP status codes, API URLs),
 *      compile-time constraints (buffer sizes, array bounds), interface
 *      contracts (env variable names, JSON field names), and internal
 *      implementation details (file suffixes). They cannot be changed at
 *      runtime because they are either fixed by external standards or
 *      required by the C compiler for stack allocation.
 *
 * Coding Standard #10: No raw numbers in code — every numeric value is
 *   defined here as a named constant with a comment explaining its purpose.
 * Coding Standard #38: No magic strings — concept-representing string
 *   literals are defined here as named constants.
 */

#ifndef CONSTANTS_H
#define CONSTANTS_H


/* ================================================================
 * FILE EXTENSION CONSTANTS
 *
 * Suffixes for temporary and final backup archive files. Used in the
 * atomic write pattern defined in Decision 005: download to a .tmp
 * file, verify integrity, then rename to the final .zip path.
 *
 * These are internal implementation details of the atomic write
 * mechanism — not operator-configurable.
 * ================================================================ */

/**
 * Suffix appended to a repository name to form the temporary file path
 * during download. The .tmp file is deleted if verification fails or
 * renamed to the final path if verification succeeds.
 * Spec Section 5c: "Download the zip to a temporary file: D:\BACKUP\
 * <repo-name>.zip.tmp"
 */
#define TEMP_FILE_SUFFIX              ".zip.tmp"


/**
 * Suffix appended to a repository name to form the final backup archive
 * path. Only one file per repo exists at any time — no timestamped
 * copies, no accumulation.
 * Spec Section 1: "Each repository is downloaded as a zip archive and
 * stored in D:\BACKUP\<repo-name>.zip"
 */
#define FINAL_FILE_SUFFIX             ".zip"


/* ================================================================
 * NETWORK CONSTANTS
 *
 * URLs, endpoints, and HTTP header names for internet connectivity
 * checks and GitHub API communication.
 *
 * These are fixed by the GitHub API contract and the HTTP specification.
 * They are not operator-configurable — changing them would point the
 * script at a different service or break the API protocol.
 * ================================================================ */

/**
 * URL used for the pre-cycle internet connectivity check. The script
 * sends a lightweight HTTP request to this URL before each cycle.
 *
 * Rationale: Using github.com directly — it is lightweight, relevant
 * to the script's purpose, and more reliable than a dedicated health-
 * check endpoint that could itself become unavailable.
 * Spec Section 4: "a basic HTTP request or DNS resolution test"
 */
#define CONNECTIVITY_CHECK_URL        "https://github.com"


/**
 * Base URL for the GitHub REST API. All API requests (repo metadata
 * queries and zip archive downloads) use this as their URL prefix.
 * Spec Section 5: "GET https://api.github.com/repos/<owner>/..."
 */
#define GITHUB_API_BASE               "https://api.github.com"


/**
 * HTTP Authorization header prefix. The GitHub API requires Bearer
 * token authentication. The actual token is extracted from the
 * GITHUB_BASE_URL in .env and appended after this prefix to form
 * the complete header value.
 * Spec Section 5: "Authenticate with Authorization: Bearer <token>."
 */
#define AUTH_HEADER_PREFIX            "Bearer "


/**
 * Name of the HTTP response header that reports how many API requests
 * remain in the current rate limit window. Checked after every API
 * call to avoid exceeding GitHub's rate limit.
 * Spec Section 7: "Respect X-RateLimit-Remaining headers."
 */
#define RATELIMIT_REMAINING_HEADER    "X-RateLimit-Remaining"


/**
 * Name of the HTTP response header that reports the Unix timestamp
 * (seconds since epoch) at which the current rate limit window resets.
 * Used to calculate how long to sleep before retrying a rate-limited
 * request.
 * Spec Section 7: "sleep until the reset window and retry."
 */
#define RATELIMIT_RESET_HEADER        "X-RateLimit-Reset"


/* ================================================================
 * API PATH CONSTANTS
 *
 * Path segments appended to GITHUB_API_BASE when constructing GitHub
 * REST API request URLs. These are fixed by the GitHub API contract.
 * ================================================================ */

/**
 * Path prefix for all repository-scoped GitHub API endpoints. Both
 * the metadata query and zipball download URLs begin with this path.
 * Spec Section 5a: "GET https://api.github.com/repos/<owner>/<repo>"
 * Spec Section 5b: "GET https://api.github.com/repos/<owner>/<repo>/zipball/<branch>"
 */
#define API_REPOS_PATH                "/repos/"


/**
 * Path segment appended to a repository URL to request a zip archive
 * download of the repository's default branch.
 * Spec Section 5b: "GET .../repos/<owner>/<repo-name>/zipball/<default-branch>"
 */
#define API_ZIPBALL_PATH              "/zipball/"


/* ================================================================
 * BUFFER SIZE CONSTANTS
 *
 * Maximum sizes for strings, buffers, and arrays used throughout the
 * program. These prevent buffer overflows and define memory allocation
 * limits for all string operations.
 *
 * These are compile-time constraints. C requires stack-allocated arrays
 * to have sizes known at compile time — they cannot be runtime values
 * read from a configuration file. The eventual NASM translation also
 * requires all buffer sizes to be assembly-time constants.
 * ================================================================ */

/**
 * Maximum length of a repository name in characters, including the
 * null terminator.
 *
 * Rationale: GitHub enforces a maximum repository name length of 100
 * characters. A limit of 256 characters provides generous headroom for
 * safety and accommodates potential future changes to GitHub's limit
 * without requiring a constants update.
 */
#define MAX_REPO_NAME_LEN             256


/**
 * Maximum length of a single line in the .env file, including the null
 * terminator.
 *
 * Rationale: The longest expected line is GITHUB_BASE_URL, which
 * contains a token (~40 characters), the github.com domain, and an
 * owner path (~50 characters), totaling well under 200 characters.
 * A limit of 1024 provides comfortable headroom for unusually long
 * tokens or owner paths.
 */
#define MAX_ENV_LINE_LEN              1024


/**
 * Maximum length of a GitHub personal access token string, including
 * the null terminator.
 *
 * Rationale: GitHub fine-grained and classic personal access tokens
 * are typically 40-84 characters. A limit of 256 characters provides
 * ample buffer for all current token formats and future changes.
 */
#define MAX_TOKEN_LEN                 256


/**
 * Maximum length of a constructed URL string, including the null
 * terminator. Covers both API request URLs and the GITHUB_BASE_URL
 * from .env.
 *
 * Rationale: 2048 characters is the widely-adopted maximum URL length
 * across browsers, servers, and proxy implementations. Constructed
 * GitHub API URLs (base + path + owner + repo + branch) are well
 * under 500 characters, making 2048 a safe upper bound.
 */
#define MAX_URL_LEN                   2048


/**
 * Maximum length of a single log entry string, including the null
 * terminator. Each entry contains a timestamp, action, repo name,
 * status, and optional detail.
 *
 * Rationale: A typical log entry contains: ISO timestamp (~25 chars),
 * action description (~30 chars), repo name (~50 chars), status
 * indicator (~10 chars), and error detail (~100 chars). 512
 * characters provides comfortable headroom for verbose error messages
 * without consuming excessive memory per entry.
 */
#define MAX_LOG_ENTRY_LEN             512


/**
 * Maximum number of repositories that can be listed in the REPOS
 * variable in .env. This is a compile-time array allocation bound —
 * the repo list is stored as a fixed-size C array, and C requires
 * array sizes to be known at compile time.
 *
 * The actual number of repos backed up is determined by how many names
 * appear in the REPOS variable in .env. This constant sets the hard
 * upper limit on that count. The config module validates at startup that
 * the number of repos in REPOS does not exceed this value.
 *
 * Rationale: 100 repositories far exceeds the needs of a single
 * GitHub account or organization backup. If more repos are needed,
 * this constant can be increased and the program recompiled.
 */
#define MAX_REPOS                     100


/* ================================================================
 * HTTP STATUS CODE CONSTANTS
 *
 * Standard HTTP response status codes that the script checks when
 * communicating with the GitHub API. Using named constants instead of
 * raw numbers per Coding Standard #10.
 *
 * These are defined by the HTTP specification (RFC 9110). They are
 * not operator-configurable — they are facts about the protocol.
 * ================================================================ */

/**
 * HTTP 200 OK — the API request succeeded.
 * Expected for successful repo metadata queries and zip downloads.
 */
#define HTTP_OK                       200


/**
 * HTTP 401 Unauthorized — the token is missing, invalid, or expired.
 * Spec Section 7: "Invalid/expired token: Log the error, fire a
 * toast, do not crash. Retry on the next hourly cycle."
 */
#define HTTP_UNAUTHORIZED             401


/**
 * HTTP 403 Forbidden — the token exists but lacks the required
 * permission scope (typically missing 'repo' scope).
 */
#define HTTP_FORBIDDEN                403


/**
 * HTTP 404 Not Found — the repository does not exist or the token
 * lacks access to the private repository.
 * Spec Section 5g: "If the repository does not exist or returns 404,
 * log a warning, fire a toast, and skip."
 */
#define HTTP_NOT_FOUND                404


/**
 * HTTP 429 Too Many Requests — the GitHub API rate limit has been
 * exceeded. The script must read X-RateLimit-Reset and sleep until
 * the window resets before retrying.
 * Spec Section 7: "If rate-limited, log, fire a toast, sleep until
 * the reset window and retry."
 */
#define HTTP_RATE_LIMITED             429


/* ================================================================
 * ENVIRONMENT VARIABLE NAME CONSTANTS
 *
 * Names of the .env variables that the config module parses. Using
 * named constants ensures consistency between the env.example template
 * and the parser code — a typo in either place is caught at compile
 * time rather than silently producing a runtime misconfiguration.
 *
 * This section contains ALL .env variable names — both the original
 * two (GITHUB_BASE_URL, REPOS) and the five configurable runtime
 * values (BACKUP_DIR, CYCLE_INTERVAL_SECONDS, HTTP_TIMEOUT_MS,
 * CONNECTIVITY_CHECK_TIMEOUT_MS, LOG_MAX_SIZE_BYTES).
 * ================================================================ */

/**
 * Name of the .env variable containing the GitHub base URL with the
 * embedded personal access token and target owner/organization path.
 * Format: https://<TOKEN>@github.com/<OWNER>/
 */
#define ENV_VAR_GITHUB_BASE_URL      "GITHUB_BASE_URL"


/**
 * Name of the .env variable containing a standalone personal access
 * token. Alternative to embedding the token in GITHUB_BASE_URL.
 * When present, takes precedence for authentication.
 * Format: ghp_xxxxxxxxxxxxxxxxxxxx
 */
#define ENV_VAR_GITHUB_TOKEN          "GITHUB_TOKEN"


/**
 * Name of the .env variable containing the target GitHub account or
 * organization name. Used with GITHUB_TOKEN when GITHUB_BASE_URL
 * is not set. Specifies the target account for API requests.
 */
#define ENV_VAR_GITHUB_OWNER          "GITHUB_OWNER"


/**
 * Name of the .env variable containing the comma-separated list of
 * repository names to back up.
 * Format: repo-one,repo-two,repo-three
 */
#define ENV_VAR_REPOS                 "REPOS"


/**
 * Name of the .env variable containing the deployment directory path.
 * The script uses this directory as the root for all output files
 * (zip archives, logs, configuration). The config module constructs
 * the full paths to .env and backup.log at runtime by appending
 * filenames to this directory path.
 * Format: D:\BACKUP\
 */
#define ENV_VAR_BACKUP_DIR            "BACKUP_DIR"


/**
 * Name of the .env variable containing the backup cycle interval
 * in seconds. After completing a cycle, the script sleeps for this
 * duration before checking connectivity and starting the next cycle.
 * Format: 3600 (for 1 hour)
 */
#define ENV_VAR_CYCLE_INTERVAL        "CYCLE_INTERVAL_SECONDS"


/**
 * Name of the .env variable containing the HTTP request timeout in
 * milliseconds. Applies to all GitHub API calls (metadata queries
 * and zip downloads).
 * Format: 30000 (for 30 seconds)
 */
#define ENV_VAR_HTTP_TIMEOUT          "HTTP_TIMEOUT_MS"


/**
 * Name of the .env variable containing the connectivity check
 * timeout in milliseconds. The pre-cycle internet probe must
 * complete within this duration or it is treated as no connectivity.
 * Format: 5000 (for 5 seconds)
 */
#define ENV_VAR_CONNECTIVITY_TIMEOUT  "CONNECTIVITY_CHECK_TIMEOUT_MS"


/**
 * Name of the .env variable containing the maximum log file size in
 * bytes before rotation occurs. When the log file exceeds this size,
 * it is deleted and started fresh.
 * Format: 1048576 (for 1 MiB)
 */
#define ENV_VAR_LOG_MAX_SIZE          "LOG_MAX_SIZE_BYTES"


/* ================================================================
 * GITHUB API JSON FIELD NAME CONSTANTS
 *
 * Keys used when parsing JSON responses from the GitHub API.
 * Coding Standard #38: Concept-representing string literals must be
 * named constants — these are part of the GitHub API contract.
 * ================================================================ */

/**
 * JSON field name for the repository's default branch.
 * Returned by the repo metadata endpoint.
 * Spec Section 5a: "Extract the default_branch field from the JSON
 * response."
 */
#define JSON_FIELD_DEFAULT_BRANCH     "default_branch"


/* ================================================================
 * SINGLE-INSTANCE AND IPC CONSTANTS
 *
 * Named objects for single-instance detection (mutex) and graceful
 * shutdown signaling (event). Using the Global\ namespace ensures
 * the mutex is visible across all sessions on the machine.
 * Spec Section 10: Single-instance detection via named mutex.
 * Spec Section 11: Shutdown via named event.
 * ================================================================ */

/** Named mutex for single-instance detection. */
#define BACKUP_MUTEX_NAME    "Global\\GitHubBackupMutex"

/** Named event for graceful shutdown signaling. */
#define BACKUP_SHUTDOWN_EVENT_NAME "Global\\GitHubBackupShutdown"

/**
 * Name of the .env variable containing the shutdown signal polling interval
 * in milliseconds. During sleep, the main loop checks the shutdown event
 * this frequently. Lower values = faster shutdown response but more CPU wakeups.
 * Format: 1000 (for 1 second)
 */
#define ENV_VAR_SHUTDOWN_CHECK_INTERVAL  "SHUTDOWN_CHECK_INTERVAL_MS"

/** Default polling interval (ms) for checking shutdown event during sleep. */
#define DEFAULT_SHUTDOWN_CHECK_INTERVAL_MS  1000

/** Maximum length of a formatted console log line. */
#define MAX_CONSOLE_LINE_LEN        1024

/**
 * Maximum length of a constructed file path (backup_dir + repo name + suffix + NUL).
 * Used for stack-allocated path buffers in backup.c and notify.c.
 * Calculated as MAX_URL_LEN + MAX_REPO_NAME_LEN + 16 (suffix + separator + NUL).
 */
#define MAX_PATH_BUF                (MAX_URL_LEN + MAX_REPO_NAME_LEN + 16)


#endif /* CONSTANTS_H */
