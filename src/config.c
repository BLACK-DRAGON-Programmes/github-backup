/**
 * config.c — Configuration implementation for the GitHub Backup Script.
 *
 * Reads and parses the .env file line by line, extracting key-value
 * pairs. Handles whitespace trimming, comment skipping, and default
 * values for optional parameters.
 *
 * The parser is intentionally simple — no regex, no external libraries.
 * It scans for "KEY=VALUE" patterns and matches against known env
 * variable names from constants.h.
 */

#include "config.h"
#include "logger.h"
#include "notify.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>


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


/* ─── Defaults ──────────────────────────────────────────────── */

void apply_defaults(backup_config *config) {
    if (config->backup_dir[0] == '\0') {
        strncpy(config->backup_dir, "D:\\BACKUP\\", MAX_URL_LEN - 1);
    }
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
}


/* ─── Public Functions ──────────────────────────────────────── */

void build_env_path(const char *backup_dir, char *path_out) {
    snprintf(path_out, MAX_URL_LEN, "%s.env", backup_dir);
}


void build_log_path(const char *backup_dir, char *path_out) {
    snprintf(path_out, MAX_URL_LEN, "%sbackup.log", backup_dir);
}


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


int parse_repos(const char *repos_raw,
                char repos[][MAX_REPO_NAME_LEN], int *count) {
    *count = 0;
    char buf[MAX_URL_LEN];
    strncpy(buf, repos_raw, MAX_URL_LEN - 1);
    buf[MAX_URL_LEN - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(buf, ",", &saveptr);

    while (token != NULL) {
        if (*count >= MAX_REPOS) {
            log_error("config", NULL,
                      "Repo count exceeds MAX_REPOS — increase constant and recompile");
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
        toast_error("Config Error", "Invalid GITHUB_BASE_URL format — token not found");
        return -1;
    }
    if (config->owner[0] == '\0') {
        log_error("config", NULL, "Could not extract owner from GITHUB_BASE_URL");
        toast_error("Config Error", "Invalid GITHUB_BASE_URL format — owner not found");
        return -1;
    }
    return 0;
}


int parse_env_file(backup_config *config) {
    char env_path[MAX_URL_LEN];

    /*
     * If backup_dir is not yet set (first call), use the default.
     * After parsing, backup_dir will be populated from .env or default.
     */
    if (config->backup_dir[0] != '\0') {
        build_env_path(config->backup_dir, env_path);
    } else {
        build_env_path("D:\\BACKUP\\", env_path);
    }

    FILE *fp = fopen(env_path, "r");
    if (fp == NULL) {
        log_error("config", NULL, "Cannot open .env file");
        toast_error("Config Error", "Cannot open .env file");
        return -1;
    }

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
            continue;  /* Malformed line — skip */
        }

        *eq = '\0';
        char *key = trim(line);
        char *value = trim(eq + 1);

        /* Match against known env variable names */
        if (strcmp(key, ENV_VAR_GITHUB_BASE_URL) == 0) {
            strncpy(config->base_url, value, MAX_URL_LEN - 1);
            config->base_url[MAX_URL_LEN - 1] = '\0';
        } else if (strcmp(key, ENV_VAR_REPOS) == 0) {
            strncpy(config->repos_raw, value, MAX_URL_LEN - 1);
            config->repos_raw[MAX_URL_LEN - 1] = '\0';
        } else if (strcmp(key, ENV_VAR_BACKUP_DIR) == 0) {
            strncpy(config->backup_dir, value, MAX_URL_LEN - 1);
            config->backup_dir[MAX_URL_LEN - 1] = '\0';
        } else if (strcmp(key, ENV_VAR_CYCLE_INTERVAL) == 0) {
            config->cycle_interval = atoi(value);
        } else if (strcmp(key, ENV_VAR_HTTP_TIMEOUT) == 0) {
            config->http_timeout = atoi(value);
        } else if (strcmp(key, ENV_VAR_CONNECTIVITY_TIMEOUT) == 0) {
            config->connectivity_timeout = atoi(value);
        } else if (strcmp(key, ENV_VAR_LOG_MAX_SIZE) == 0) {
            config->log_max_size = atol(value);
        }
        /* Unknown keys are silently ignored — they may be from future .env versions */
    }

    fclose(fp);

    /* Apply defaults for any missing optional values */
    apply_defaults(config);

    /* Extract token and owner from base_url */
    if (config->base_url[0] != '\0') {
        extract_token(config->base_url, config->token);
        extract_owner(config->base_url, config->owner);
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
    return 0;
}
