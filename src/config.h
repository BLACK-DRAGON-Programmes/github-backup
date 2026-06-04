/**
 * config.h — Configuration interface for the GitHub Backup Script.
 *
 * Reads the .env file and populates a config struct with all runtime
 * parameters. Parses GITHUB_BASE_URL to extract the authentication
 * token and target owner. Parses the REPOS list into an array.
 *
 * The config module is the first data-processing module in the build
 * sequence. All domain modules (network, backup) depend on it for
 * the token, owner, and repo list.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "constants.h"

#include <stddef.h>


/**
 * Runtime configuration struct. Populated by parse_env_file() from the
 * .env file. Contains all configurable variables defined in env.example.
 */
typedef struct {
    /* Raw values from .env */
    char base_url[MAX_URL_LEN];           /* GITHUB_BASE_URL — token + owner URL */
    char repos_raw[MAX_URL_LEN];         /* REPOS — raw comma-separated string */

    /* Parsed values */
    char token[MAX_TOKEN_LEN];           /* Extracted from base_url authority */
    char owner[MAX_REPO_NAME_LEN];       /* Extracted from base_url path */
    char repos[MAX_REPOS][MAX_REPO_NAME_LEN]; /* Parsed repo name array */
    int  repo_count;                      /* Number of repos in the array */

    /* Configurable runtime values (with defaults) */
    char backup_dir[MAX_URL_LEN];         /* BACKUP_DIR — deployment directory */
    int  cycle_interval;                  /* CYCLE_INTERVAL_SECONDS */
    int  http_timeout;                    /* HTTP_TIMEOUT_MS */
    int  connectivity_timeout;            /* CONNECTIVITY_CHECK_TIMEOUT_MS */
    long log_max_size;                    /* LOG_MAX_SIZE_BYTES */
    int  shutdown_check_interval;       /* SHUTDOWN_CHECK_INTERVAL_MS */
} backup_config;


/**
 * Parse the .env file and populate the config struct. Reads all
 * variables, extracts token and owner from GITHUB_BASE_URL, parses
 * the REPOS list. Missing configurable values get defaults (documented
 * in env.example). Missing mandatory values (GITHUB_BASE_URL, REPOS)
 * result in an error return.
 *
 * @param config  Pointer to the config struct to populate
 * @return 0 on success, -1 if .env is missing or mandatory fields are absent
 */
int parse_env_file(backup_config *config);


/**
 * Construct the full path to the .env file by appending ".env" to
 * the executable's directory. The .env file sits next to the exe.
 *
 * @param exe_dir    The directory containing the running executable
 * @param path_out   Output buffer (at least MAX_URL_LEN bytes)
 */
void build_env_path(const char *exe_dir, char *path_out);


/**
 * Get the directory containing the running executable.
 * On Windows: uses GetModuleFileNameA(). On Linux: reads /proc/self/exe.
 * Output always includes a trailing path separator.
 *
 * @param dir_out   Output buffer (at least MAX_URL_LEN bytes)
 * @param dir_size  Size of the output buffer
 */
void get_exe_dir(char *dir_out, size_t dir_size);


/**
 * Ensure a directory exists, creating it (and parent directories) if needed.
 * Returns 0 on success or if already exists, -1 on failure.
 *
 * @param path  The directory path to ensure
 */
int ensure_dir_exists(const char *path);


/**
 * Construct the full path to the log file by appending "backup.log"
 * to the backup directory.
 *
 * @param backup_dir  The BACKUP_DIR value
 * @param path_out    Output buffer (at least MAX_URL_LEN bytes)
 */
void build_log_path(const char *backup_dir, char *path_out);


/**
 * Extract the personal access token from the GITHUB_BASE_URL.
 * The token is the portion before "@github.com" in the URL authority.
 *
 * Example: "https://ghp_ABC123@github.com/owner/"
 *          → token = "ghp_ABC123"
 *
 * @param base_url   The full GITHUB_BASE_URL string
 * @param token_out  Output buffer (at least MAX_TOKEN_LEN bytes)
 * @return 0 on success, -1 if the URL format is invalid
 */
int extract_token(const char *base_url, char *token_out);


/**
 * Extract the owner/organization name from the GITHUB_BASE_URL.
 * The owner is the path segment after "github.com/".
 *
 * Example: "https://ghp_ABC123@github.com/my-organization/"
 *          → owner = "my-organization"
 *
 * @param base_url   The full GITHUB_BASE_URL string
 * @param owner_out  Output buffer (at least MAX_REPO_NAME_LEN bytes)
 * @return 0 on success, -1 if the URL format is invalid
 */
int extract_owner(const char *base_url, char *owner_out);


/**
 * Parse the raw REPOS string into an array of individual repo names.
 * Trims whitespace around names, skips comments (#) and blanks.
 *
 * @param repos_raw   The raw comma-separated string from .env
 * @param repos       Output array of repo names
 * @param count       Output: number of repos parsed
 * @return 0 on success, -1 if the array would exceed MAX_REPOS
 */
int parse_repos(const char *repos_raw,
                char repos[][MAX_REPO_NAME_LEN], int *count);


/**
 * Validate that all mandatory config fields are populated.
 * GITHUB_BASE_URL and REPOS are mandatory. Configurable values
 * already have defaults from parse_env_file.
 *
 * @param config  The config struct to validate
 * @return 0 if valid, -1 if mandatory fields are missing
 */
int validate_config(const backup_config *config);


/**
 * Apply default values to any optional config fields that were
 * not set in the .env file. Called by parse_env_file internally,
 * and also exposed for use during startup validation when the
 * .env file hasn't been parsed yet.
 *
 * @param config  The config struct to apply defaults to
 */
void apply_defaults(backup_config *config);


#endif /* CONFIG_H */
