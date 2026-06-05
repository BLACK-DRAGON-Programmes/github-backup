/**
 * config.c - Configuration implementation for the GitHub Backup Script.
 *
 * Reads and parses the .env file line by line, extracting key-value
 * pairs. Handles whitespace trimming, comment skipping, and default
 * values for optional parameters.
 *
 * The parser is intentionally simple - no regex, no external libraries.
 * It scans for "KEY=VALUE" patterns and matches against known env
 * variable names from constants.h.
 */

/*
 * Target Windows Vista or later - required for SHCreateDirectoryExA to be
 * declared in <shlobj.h>. Without these macros, MinGW-w64 defaults to an
 * older NTDDI version that hides the function prototype.
 */
#ifndef WINVER
#define WINVER             0x0600
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT       0x0600
#endif

#include "config.h"
#include "logger.h"
#include "notify.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <limits.h>
#endif


/* ─── String Helpers ──────────────────────────────────────── */

/**
 * Trim leading and trailing whitespace in-place.
 *
 * @param str  String to trim (modified in-place)
 * @return     The trimmed string (same pointer)
 */
static char *trim(char *str) {
    while (*str && isspace((unsigned char)*str)) {
        str++;
    }
    if (*str == '\0') {
        return str;
    }
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return str;
}


/**
 * Check if a line is a comment (starts with #) or blank.
 */
static int is_comment_or_blank(const char *line) {
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    return (*line == '#' || *line == '\0' || *line == '\n');
}


/* ─── Exe Directory ──────────────────────────────────────── */

/**
 * Get the directory containing the running executable.
 * On Windows: uses GetModuleFileNameA() and strips the filename.
 * On Linux: reads /proc/self/exe symlink.
 * The output always includes a trailing path separator.
 * Returns an empty string if the path cannot be determined.
 */
void get_exe_dir(char *dir_out, size_t dir_size) {
    dir_out[0] = '\0';

    #ifdef _WIN32
    char exe_path[MAX_URL_LEN];
    DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_URL_LEN);
    if (len == 0 || len >= MAX_URL_LEN) {
        return;
    }
    /* Strip the filename: find last backslash or forward slash */
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep == NULL) {
        last_sep = strrchr(exe_path, '/');
    }
    if (last_sep != NULL) {
        size_t dir_len = (size_t)(last_sep - exe_path) + 1; /* include trailing sep */
        if (dir_len >= dir_size) {
            dir_len = dir_size - 1;
        }
        strncpy(dir_out, exe_path, dir_len);
        dir_out[dir_len] = '\0';
    }
    #else
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, PATH_MAX - 1);
    if (len <= 0) {
        return;
    }
    exe_path[len] = '\0';
    char *last_sep = strrchr(exe_path, '/');
    if (last_sep != NULL) {
        size_t dir_len = (size_t)(last_sep - exe_path) + 1;
        if (dir_len >= dir_size) {
            dir_len = dir_size - 1;
        }
        strncpy(dir_out, exe_path, dir_len);
        dir_out[dir_len] = '\0';
    }
    #endif
}


/* ─── Directory Creation ──────────────────────────────────── */

/**
 * Ensure a directory exists, creating it (and any parent directories)
 * if needed. Returns 0 on success (or already exists), -1 on failure.
 *
 * On Windows: uses SHCreateDirectoryExA which creates the full path
 * recursively in a single call.
 * On Linux: walks the path components and calls mkdir() for each.
 */
int ensure_dir_exists(const char *path) {
    if (path[0] == '\0') {
        return -1;
    }

    #ifdef _WIN32
    int result = SHCreateDirectoryExA(NULL, path, NULL);
    if (result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS) {
        return 0;
    }
    return -1;
    #else
    char tmp[MAX_URL_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);

    /* Remove trailing slash if present */
    size_t len = strlen(tmp);
    if (len > 0 && (tmp[len - 1] == '/')) {
        tmp[len - 1] = '\0';
    }

    /* Create each path component */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            char saved = *p;
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = saved;
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
    #endif
}


/* ─── Defaults ──────────────────────────────────────────── */

void apply_defaults(backup_config *config) {
    /*
     * If BACKUP_DIR is not set in .env, do NOT hardcode a default.
     * The caller (main.c) sets backup_dir to the exe's directory before
     * calling parse_env_file, so if BACKUP_DIR is missing from .env the
     * exe's own directory is used as the output directory.
     */
    if (config->cycle_interval == 0) {
        config->cycle_interval = 3600;
    }
    if (config->http_timeout == 0) {
        config->http_timeout = 30000;
    }
    if (config->connectivity_timeout == 0) {
        config->connectivity_timeout = 5000;
    }
    if (config->log_max_size == 0) {
        config->log_max_size = 1048576;  /* 1 MiB */
    }
    if (config->shutdown_check_interval == 0) {
        config->shutdown_check_interval = DEFAULT_SHUTDOWN_CHECK_INTERVAL_MS;
    }
}


/* ─── Path Builders ──────────────────────────────────────── */

void build_env_path(const char *exe_dir, char *path_out) {
    /*
     * Build .env path from the executable's directory.
     * The .env file sits next to the exe - not in BACKUP_DIR.
     */
    size_t dir_len = strlen(exe_dir);
    if (dir_len + 4 >= MAX_URL_LEN) {  /* 4 = strlen(".env") */
        char detail[MAX_URL_LEN];
        snprintf(detail, sizeof(detail),
                 "Exe directory path too long (%zu chars) - cannot build .env path "
                 "(max %d chars). Move the executable to a shorter path.",
                 dir_len, MAX_URL_LEN - 5);
        log_error("config", NULL, detail);
        path_out[0] = '\0';
        return;
    }
    snprintf(path_out, MAX_URL_LEN, "%s.env", exe_dir);
}


void build_log_path(const char *backup_dir, char *path_out) {
    /*
     * Validate: backup_dir + "backup.log" must fit in MAX_URL_LEN.
     * Same fail-fast logic as build_env_path.
     */
    size_t dir_len = strlen(backup_dir);
    if (dir_len + 10 >= MAX_URL_LEN) {  /* 10 = strlen("backup.log") */
        char detail[MAX_URL_LEN];
        snprintf(detail, sizeof(detail),
                 "BACKUP_DIR too long (%zu chars) - cannot build log path "
                 "(max %d chars). Shorten BACKUP_DIR in .env.",
                 dir_len, MAX_URL_LEN - 11);
        log_error("config", NULL, detail);
        path_out[0] = '\0';
        return;
    }
    snprintf(path_out, MAX_URL_LEN, "%sbackup.log", backup_dir);
}


/* ─── URL Parsing ──────────────────────────────────────── */

int extract_token(const char *base_url, char *token_out) {
    /*
     * Format: https://<TOKEN>@github.com/<OWNER>/
     * Find "://" then find "@" after it. The token is between them.
     */
    const char *after_scheme = strstr(base_url, "://");
    if (after_scheme == NULL) {
        return -1;
    }
    after_scheme += 3;

    const char *at_sign = strchr(after_scheme, '@');
    if (at_sign == NULL) {
        return -1;
    }

    int token_len = (int)(at_sign - after_scheme);
    if (token_len <= 0 || token_len >= MAX_TOKEN_LEN) {
        return -1;
    }

    strncpy(token_out, after_scheme, (size_t)token_len);
    token_out[token_len] = '\0';
    return 0;
}


int extract_owner(const char *base_url, char *owner_out) {
    /*
     * Format: https://<TOKEN>@github.com/<OWNER>/
     * Find "@github.com/" then extract the path segment after it.
     */
    const char *prefix = "@github.com/";
    const char *owner_start = strstr(base_url, prefix);
    if (owner_start == NULL) {
        return -1;
    }
    owner_start += strlen(prefix);

    /*
     * Owner ends at the next '/' or end of string.
     * A trailing slash after owner is optional but common.
     */
    const char *owner_end = strchr(owner_start, '/');
    int owner_len;
    if (owner_end != NULL) {
        owner_len = (int)(owner_end - owner_start);
    } else {
        owner_len = (int)strlen(owner_start);
    }

    if (owner_len <= 0 || owner_len >= MAX_REPO_NAME_LEN) {
        return -1;
    }

    strncpy(owner_out, owner_start, (size_t)owner_len);
    owner_out[owner_len] = '\0';
    return 0;
}


/* ─── Repo List Parsing ─────────────────────────────────── */

int parse_repos(const char *repos_raw,
                char repos[][MAX_REPO_NAME_LEN], int *count) {
    *count = 0;
    char buf[MAX_URL_LEN];
    snprintf(buf, sizeof(buf), "%s", repos_raw);

    char *saveptr = NULL;
    char *token = strtok_r(buf, ",", &saveptr);

    while (token != NULL) {
        if (*count >= MAX_REPOS) {
            log_error("config", NULL,
                      "Repo count exceeds MAX_REPOS - increase constant and recompile");
            return -1;
        }

        char *trimmed = trim(token);

        /* Skip comments and blanks */
        if (is_comment_or_blank(trimmed)) {
            token = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        strncpy(repos[*count], trimmed, MAX_REPO_NAME_LEN - 1);
        repos[*count][MAX_REPO_NAME_LEN - 1] = '\0';
        (*count)++;

        token = strtok_r(NULL, ",", &saveptr);
    }

    return 0;
}


/* ─── Validation ─────────────────────────────────────────── */

int validate_config(const backup_config *config) {
    if (config->base_url[0] == '\0') {
        log_error("config", NULL, "GITHUB_BASE_URL is missing from .env");
        toast_error("Config Error", "GITHUB_BASE_URL is missing from .env");
        return -1;
    }
    if (config->repo_count == 0) {
        log_error("config", NULL, "REPOS is empty or missing from .env");
        toast_error("Config Error", "REPOS is empty or missing from .env");
        return -1;
    }
    if (config->token[0] == '\0') {
        log_error("config", NULL, "Could not extract token from GITHUB_BASE_URL");
        toast_error("Config Error", "Invalid GITHUB_BASE_URL format - token not found");
        return -1;
    }
    if (config->owner[0] == '\0') {
        log_error("config", NULL, "Could not extract owner from GITHUB_BASE_URL");
        toast_error("Config Error", "Invalid GITHUB_BASE_URL format - owner not found");
        return -1;
    }
    return 0;
}


/* ─── Main Parser ────────────────────────────────────────── */

int parse_env_file(backup_config *config) {
    char env_path[MAX_URL_LEN];

    /*
     * The .env file lives in the exe's directory, next to the executable.
     * config->backup_dir is used as a transport for exe_dir: the caller
     * (main.c) sets backup_dir = exe_dir before the first call, and
     * restore_exe_dir() after each cycle re-read.
     */
    if (config->backup_dir[0] != '\0') {
        build_env_path(config->backup_dir, env_path);
    } else {
        log_error("config", NULL, "Cannot locate .env - exe directory not set");
        return -1;
    }

    FILE *fp = fopen(env_path, "r");
    if (fp == NULL) {
        log_error("config", NULL, "Cannot open .env file");
        toast_error("Config Error", "Cannot open .env file");
        return -1;
    }

    /* Preserve exe_dir across the memset - save it, restore after */
    char exe_dir[MAX_URL_LEN];
    snprintf(exe_dir, sizeof(exe_dir), "%s", config->backup_dir);

    /* Zero out the struct */
    memset(config, 0, sizeof(backup_config));

    char line[MAX_ENV_LINE_LEN];
    while (fgets(line, MAX_ENV_LINE_LEN, fp) != NULL) {
        if (is_comment_or_blank(line)) {
            continue;
        }

        /* Find the '=' separator */
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            continue;  /* Malformed line - skip */
        }

        *eq = '\0';
        char *key = trim(line);
        char *value = trim(eq + 1);

        /* Match against known env variable names */
        if (strcmp(key, ENV_VAR_GITHUB_BASE_URL) == 0) {
            snprintf(config->base_url, sizeof(config->base_url),
                     "%s", value);
            DBG("config: Parsed %s (len=%zu)", key, strlen(value));
        } else if (strcmp(key, ENV_VAR_GITHUB_TOKEN) == 0) {
            /* Standalone token - takes precedence over URL-embedded token */
            snprintf(config->token, sizeof(config->token),
                     "%s", value);
            DBG("config: Parsed %s (len=%zu)", key, strlen(value));
        } else if (strcmp(key, ENV_VAR_GITHUB_OWNER) == 0) {
            /* Standalone owner - used with GITHUB_TOKEN when base_url is absent */
            snprintf(config->owner, sizeof(config->owner),
                     "%s", value);
            DBG("config: Parsed %s = '%s'", key, value);
        } else if (strcmp(key, ENV_VAR_REPOS) == 0) {
            snprintf(config->repos_raw, sizeof(config->repos_raw),
                     "%s", value);
        } else if (strcmp(key, ENV_VAR_BACKUP_DIR) == 0) {
            snprintf(config->backup_dir, sizeof(config->backup_dir),
                     "%s", value);
        } else if (strcmp(key, ENV_VAR_CYCLE_INTERVAL) == 0) {
            config->cycle_interval = atoi(value);
        } else if (strcmp(key, ENV_VAR_HTTP_TIMEOUT) == 0) {
            config->http_timeout = atoi(value);
        } else if (strcmp(key, ENV_VAR_CONNECTIVITY_TIMEOUT) == 0) {
            config->connectivity_timeout = atoi(value);
        } else if (strcmp(key, ENV_VAR_LOG_MAX_SIZE) == 0) {
            config->log_max_size = atol(value);
        } else if (strcmp(key, ENV_VAR_SHUTDOWN_CHECK_INTERVAL) == 0) {
            config->shutdown_check_interval = atoi(value);
        }
        /* Unknown keys are silently ignored - they may be from future .env versions */
    }

    fclose(fp);

    /* Apply defaults for any missing optional values */
    apply_defaults(config);

    /* Extract token and owner from base_url (only if not already set
     * via standalone GITHUB_TOKEN / GITHUB_OWNER fields).
     * The standalone fields take precedence per spec Section 2:
     * "When this field is present, it takes precedence for authentication."
     */
    if (config->base_url[0] != '\0') {
        /* Only extract from URL if standalone fields are empty */
        if (config->token[0] == '\0') {
            extract_token(config->base_url, config->token);
        }
        if (config->owner[0] == '\0') {
            extract_owner(config->base_url, config->owner);
        }
    }

    /* Parse repo list */
    if (config->repos_raw[0] != '\0') {
        if (parse_repos(config->repos_raw, config->repos, &config->repo_count) != 0) {
            return -1;
        }
    }

    /* Validate mandatory fields */
    if (validate_config(config) != 0) {
        return -1;
    }

    log_event(LOG_INFO, "config", NULL, "OK",
              "Loaded configuration from .env");

    DBG("config: Loaded - %d repos, owner='%s', backup_dir='%s'",
            config->repo_count, config->owner, config->backup_dir);
    for (int i = 0; i < config->repo_count; i++) {
        DBG("config:   repo[%d] = '%s'", i, config->repos[i]);
    }

    return 0;
}
