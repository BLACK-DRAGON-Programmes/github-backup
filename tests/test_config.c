/**
 * test_config.c - Unit tests for the configuration module.
 *
 * Tests env file parsing, token/owner extraction, repo list parsing,
 * default value application, validation, directory creation, and
 * full integration with temp .env files.
 *
 * Compile (Linux):  gcc -o test_config tests/test_config.c src/config.c src/logger.c src/notify.c src/network.c src/backup.c -I src/
 */

#include "config.h"
#include "context.h"
#include "logger_iface.h"
#include "notify_iface.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/stat.h>


static int tests_passed = 0;
static int tests_failed = 0;


#define TEST(name) printf("  TEST: %s... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)


/* ─── Fake DI implementations for testing ─────────────────── */

/* Fake logger ops for testing — all no-ops */
static void fake_log_event(ghb_context *ctx, log_level level, const char *action, const char *repo, const char *status, const char *detail) { (void)ctx; (void)level; (void)action; (void)repo; (void)status; (void)detail; }
static void fake_log_error(ghb_context *ctx, const char *action, const char *repo, const char *detail) { (void)ctx; (void)action; (void)repo; (void)detail; }
static int  fake_log_init(ghb_context *ctx, const char *path) { (void)ctx; (void)path; return 0; }
static void fake_log_close(ghb_context *ctx) { (void)ctx; }
static void fake_rotate_log(ghb_context *ctx, long max_size) { (void)ctx; (void)max_size; }

static const logger_ops fake_logger = {
    .log_event = fake_log_event,
    .log_error = fake_log_error,
    .log_init = fake_log_init,
    .log_close = fake_log_close,
    .rotate_log = fake_rotate_log,
};

/* Fake notify ops for testing — all no-ops */
static void fake_toast_info(ghb_context *ctx, const char *title, const char *msg) { (void)ctx; (void)title; (void)msg; }
static void fake_toast_success(ghb_context *ctx, const char *repo, const char *msg) { (void)ctx; (void)repo; (void)msg; }
static void fake_toast_error(ghb_context *ctx, const char *title, const char *msg) { (void)ctx; (void)title; (void)msg; }
static int  fake_notify_init(ghb_context *ctx) { (void)ctx; return 0; }
static void fake_notify_cleanup(ghb_context *ctx) { (void)ctx; }

static const notify_ops fake_notify = {
    .toast_info = fake_toast_info,
    .toast_success = fake_toast_success,
    .toast_error = fake_toast_error,
    .notify_init = fake_notify_init,
    .notify_cleanup = fake_notify_cleanup,
};

/* Helper: build a test context with fake no-op logger and notify */
static ghb_context create_test_context(void) {
    ghb_context ctx;
    ctx.logger = &fake_logger;
    ctx.notify = &fake_notify;
    ctx.network = NULL;
    ctx.should_stop = NULL;
    return ctx;
}


/* ─── Token Extraction Tests ──────────────────────────────── */

static void test_token_extraction_valid(void) {
    TEST("extract_token - valid URL");

    char token[MAX_TOKEN_LEN] = {0};
    int result = extract_token(
        "https://ghp_ABCDEF1234567890@github.com/my-org/",
        token
    );

    if (result != 0) { FAIL("returned error"); return; }
    if (strcmp(token, "ghp_ABCDEF1234567890") != 0) {
        FAIL("wrong token value");
        return;
    }
    PASS();
}


static void test_token_extraction_no_at(void) {
    TEST("extract_token - no @ sign");

    char token[MAX_TOKEN_LEN] = {0};
    int result = extract_token("https://github.com/owner/", token);

    if (result == 0) { FAIL("should have failed"); return; }
    PASS();
}


static void test_token_extraction_no_scheme(void) {
    TEST("extract_token - no scheme");

    char token[MAX_TOKEN_LEN] = {0};
    int result = extract_token("ghp_token@github.com/owner/", token);

    if (result == 0) { FAIL("should have failed"); return; }
    PASS();
}


/* ─── Owner Extraction Tests ───────────────────────────────── */

static void test_owner_extraction_valid(void) {
    TEST("extract_owner - valid URL with trailing slash");

    char owner[MAX_REPO_NAME_LEN] = {0};
    int result = extract_owner(
        "https://ghp_token@github.com/my-org/",
        owner
    );

    if (result != 0) { FAIL("returned error"); return; }
    if (strcmp(owner, "my-org") != 0) {
        FAIL("wrong owner value");
        return;
    }
    PASS();
}


static void test_owner_extraction_no_trailing_slash(void) {
    TEST("extract_owner - no trailing slash");

    char owner[MAX_REPO_NAME_LEN] = {0};
    int result = extract_owner(
        "https://ghp_token@github.com/some-owner",
        owner
    );

    if (result != 0) { FAIL("returned error"); return; }
    if (strcmp(owner, "some-owner") != 0) {
        FAIL("wrong owner value");
        return;
    }
    PASS();
}


static void test_owner_extraction_no_prefix(void) {
    TEST("extract_owner - no github.com prefix");

    char owner[MAX_REPO_NAME_LEN] = {0};
    int result = extract_owner("https://ghp_token@gitlab.com/owner/", owner);

    if (result == 0) { FAIL("should have failed"); return; }
    PASS();
}


/* ─── Repo Parsing Tests ──────────────────────────────────── */

static void test_parse_repos_multiple(void) {
    TEST("parse_repos - multiple repos");

    ghb_context ctx = create_test_context();
    char repos[MAX_REPOS][MAX_REPO_NAME_LEN] = {0};
    int count = 0;
    int result = parse_repos(&ctx, "repo-one, repo-two,repo-three", repos, &count);

    if (result != 0) { FAIL("returned error"); return; }
    if (count != 3) { FAIL("wrong count"); return; }
    if (strcmp(repos[0], "repo-one") != 0) { FAIL("repo[0] mismatch"); return; }
    if (strcmp(repos[1], "repo-two") != 0) { FAIL("repo[1] mismatch"); return; }
    if (strcmp(repos[2], "repo-three") != 0) { FAIL("repo[2] mismatch"); return; }
    PASS();
}


static void test_parse_repos_single(void) {
    TEST("parse_repos - single repo");

    ghb_context ctx = create_test_context();
    char repos[MAX_REPOS][MAX_REPO_NAME_LEN] = {0};
    int count = 0;
    int result = parse_repos(&ctx, "my-repo", repos, &count);

    if (result != 0) { FAIL("returned error"); return; }
    if (count != 1) { FAIL("wrong count"); return; }
    if (strcmp(repos[0], "my-repo") != 0) { FAIL("repo mismatch"); return; }
    PASS();
}


static void test_parse_repos_comments_and_blanks(void) {
    TEST("parse_repos - skip comments and blanks");

    ghb_context ctx = create_test_context();
    char repos[MAX_REPOS][MAX_REPO_NAME_LEN] = {0};
    int count = 0;
    int result = parse_repos(&ctx, "#comment,repo-a,,repo-b,# another comment", repos, &count);

    if (result != 0) { FAIL("returned error"); return; }
    if (count != 2) { FAIL("wrong count"); return; }
    if (strcmp(repos[0], "repo-a") != 0) { FAIL("repo[0] mismatch"); return; }
    if (strcmp(repos[1], "repo-b") != 0) { FAIL("repo[1] mismatch"); return; }
    PASS();
}


/* ─── Path Construction Tests ─────────────────────────────── */

static void test_build_env_path(void) {
    TEST("build_env_path");

    ghb_context ctx = create_test_context();
    char path[MAX_URL_LEN] = {0};
    build_env_path(&ctx, "D:\\BACKUP\\", path);

    if (strcmp(path, "D:\\BACKUP\\.env") != 0) {
        FAIL("wrong path");
        return;
    }
    PASS();
}


static void test_build_log_path(void) {
    TEST("build_log_path");

    ghb_context ctx = create_test_context();
    char path[MAX_URL_LEN] = {0};
    build_log_path(&ctx, "D:\\BACKUP\\", path);

    if (strcmp(path, "D:\\BACKUP\\backup.log") != 0) {
        FAIL("wrong path");
        return;
    }
    PASS();
}


/* ─── Apply Defaults Tests ─────────────────────────────────── */

static void test_apply_defaults_empty(void) {
    TEST("apply_defaults - empty config gets all defaults");

    backup_config config;
    memset(&config, 0, sizeof(backup_config));

    apply_defaults(&config);

    if (strcmp(config.backup_dir, DEFAULT_BACKUP_DIR) != 0) {
        FAIL("wrong default backup_dir"); return;
    }
    if (config.cycle_interval != 3600) { FAIL("wrong default cycle_interval"); return; }
    if (config.http_timeout != 30000) { FAIL("wrong default http_timeout"); return; }
    if (config.connectivity_timeout != 5000) { FAIL("wrong default connectivity_timeout"); return; }
    if (config.log_max_size != 1048576) { FAIL("wrong default log_max_size"); return; }
    if (config.shutdown_check_interval != DEFAULT_SHUTDOWN_CHECK_INTERVAL_MS) {
        FAIL("wrong default shutdown_check_interval"); return;
    }
    PASS();
}


static void test_apply_defaults_preserves_set(void) {
    TEST("apply_defaults - preserves already-set values");

    backup_config config;
    memset(&config, 0, sizeof(backup_config));

    snprintf(config.backup_dir, sizeof(config.backup_dir), "/custom/dir/");
    config.cycle_interval = 7200;
    config.http_timeout = 60000;
    config.connectivity_timeout = 10000;
    config.log_max_size = 2097152;
    config.shutdown_check_interval = 500;

    apply_defaults(&config);

    if (strcmp(config.backup_dir, "/custom/dir/") != 0) { FAIL("overwrote backup_dir"); return; }
    if (config.cycle_interval != 7200) { FAIL("overwrote cycle_interval"); return; }
    if (config.http_timeout != 60000) { FAIL("overwrote http_timeout"); return; }
    if (config.connectivity_timeout != 10000) { FAIL("overwrote connectivity_timeout"); return; }
    if (config.log_max_size != 2097152) { FAIL("overwrote log_max_size"); return; }
    if (config.shutdown_check_interval != 500) { FAIL("overwrote shutdown_check_interval"); return; }
    PASS();
}


static void test_apply_defaults_partial(void) {
    TEST("apply_defaults - fills only missing values");

    backup_config config;
    memset(&config, 0, sizeof(backup_config));

    config.cycle_interval = 1800;
    /* All others left at 0 — should get defaults */

    apply_defaults(&config);

    if (config.cycle_interval != 1800) { FAIL("overwrote cycle_interval"); return; }
    if (strcmp(config.backup_dir, DEFAULT_BACKUP_DIR) != 0) { FAIL("didn't apply backup_dir default"); return; }
    if (config.http_timeout != 30000) { FAIL("didn't apply http_timeout default"); return; }
    PASS();
}


/* ─── Validate Config Tests ────────────────────────────────── */

static void test_validate_config_valid(void) {
    TEST("validate_config - valid config passes");

    ghb_context ctx = create_test_context();
    backup_config config;
    memset(&config, 0, sizeof(backup_config));

    snprintf(config.base_url, sizeof(config.base_url),
             "https://ghp_test123@github.com/my-org/");
    snprintf(config.token, sizeof(config.token), "ghp_test123");
    snprintf(config.owner, sizeof(config.owner), "my-org");
    snprintf(config.repos[0], MAX_REPO_NAME_LEN, "repo1");
    config.repo_count = 1;

    if (validate_config(&ctx, &config) != 0) { FAIL("valid config rejected"); return; }
    PASS();
}


static void test_validate_config_no_auth(void) {
    TEST("validate_config - no auth (empty base_url and token) fails");

    ghb_context ctx = create_test_context();
    backup_config config;
    memset(&config, 0, sizeof(backup_config));

    snprintf(config.repos[0], MAX_REPO_NAME_LEN, "repo1");
    config.repo_count = 1;

    if (validate_config(&ctx, &config) == 0) { FAIL("should fail with no auth"); return; }
    PASS();
}


static void test_validate_config_no_repos(void) {
    TEST("validate_config - empty repo list fails");

    ghb_context ctx = create_test_context();
    backup_config config;
    memset(&config, 0, sizeof(backup_config));

    snprintf(config.token, sizeof(config.token), "ghp_test123");
    snprintf(config.owner, sizeof(config.owner), "my-org");
    config.repo_count = 0;

    if (validate_config(&ctx, &config) == 0) { FAIL("should fail with no repos"); return; }
    PASS();
}


static void test_validate_config_no_token(void) {
    TEST("validate_config - no token fails");

    ghb_context ctx = create_test_context();
    backup_config config;
    memset(&config, 0, sizeof(backup_config));

    snprintf(config.owner, sizeof(config.owner), "my-org");
    snprintf(config.repos[0], MAX_REPO_NAME_LEN, "repo1");
    config.repo_count = 1;

    if (validate_config(&ctx, &config) == 0) { FAIL("should fail with no token"); return; }
    PASS();
}


static void test_validate_config_no_owner(void) {
    TEST("validate_config - no owner fails");

    ghb_context ctx = create_test_context();
    backup_config config;
    memset(&config, 0, sizeof(backup_config));

    snprintf(config.token, sizeof(config.token), "ghp_test123");
    snprintf(config.repos[0], MAX_REPO_NAME_LEN, "repo1");
    config.repo_count = 1;

    if (validate_config(&ctx, &config) == 0) { FAIL("should fail with no owner"); return; }
    PASS();
}


static void test_validate_config_standalone_token(void) {
    TEST("validate_config - standalone GITHUB_TOKEN (no base_url) passes");

    ghb_context ctx = create_test_context();
    backup_config config;
    memset(&config, 0, sizeof(backup_config));

    snprintf(config.token, sizeof(config.token), "ghp_standalone_token");
    snprintf(config.owner, sizeof(config.owner), "my-org");
    snprintf(config.repos[0], MAX_REPO_NAME_LEN, "repo1");
    config.repo_count = 1;

    if (validate_config(&ctx, &config) != 0) { FAIL("standalone token should pass"); return; }
    PASS();
}


/* ─── Ensure Dir Exists Tests ──────────────────────────────── */

static void test_ensure_dir_creates(void) {
    TEST("ensure_dir_exists - creates new directory");

    const char *path = "/tmp/ghb_test_config_newdir/sub/";
    /* Ensure clean state */
    rmdir(path);
    rmdir("/tmp/ghb_test_config_newdir");

    int result = ensure_dir_exists(path);
    if (result != 0) { FAIL("failed to create directory"); return; }

    /* Verify it exists */
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        FAIL("directory not created");
        rmdir(path);
        rmdir("/tmp/ghb_test_config_newdir");
        return;
    }

    rmdir(path);
    rmdir("/tmp/ghb_test_config_newdir");
    PASS();
}


static void test_ensure_dir_already_exists(void) {
    TEST("ensure_dir_exists - already existing directory returns 0");

    const char *path = "/tmp/ghb_test_config_exist/";
    mkdir(path, 0755);  /* Pre-create */

    int result = ensure_dir_exists(path);
    if (result != 0) { FAIL("failed on existing directory"); rmdir(path); return; }

    rmdir(path);
    PASS();
}


static void test_ensure_dir_empty_path(void) {
    TEST("ensure_dir_exists - empty path returns -1");

    int result = ensure_dir_exists("");
    if (result == 0) { FAIL("should fail for empty path"); return; }
    PASS();
}


/* ─── Parse Env File Integration Tests ─────────────────────── */

/** Helper: write a temp .env file with given content. */
static void write_temp_env(const char *dir, const char *content) {
    char path[MAX_URL_LEN];
    snprintf(path, sizeof(path), "%s.env", dir);

    mkdir(dir, 0755);
    FILE *fp = fopen(path, "w");
    if (fp) { fputs(content, fp); fclose(fp); }
}

/** Helper: remove temp .env and its directory. */
static void cleanup_temp_env(const char *dir) {
    char path[MAX_URL_LEN];
    snprintf(path, sizeof(path), "%s.env", dir);
    remove(path);
    rmdir(dir);
}


static void test_parse_env_valid(void) {
    TEST("parse_env_file - valid .env with all fields");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_valid/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_BASE_URL=https://ghp_AbCdEf123@github.com/test-org/\n"
        "REPOS=repo-alpha,repo-beta\n"
        "BACKUP_DIR=/tmp/backups/\n"
        "CYCLE_INTERVAL_SECONDS=1800\n"
        "HTTP_TIMEOUT_MS=60000\n"
        "CONNECTIVITY_CHECK_TIMEOUT_MS=10000\n"
        "LOG_MAX_SIZE_BYTES=2097152\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed"); cleanup_temp_env(dir); return; }

    if (strcmp(config.token, "ghp_AbCdEf123") != 0) { FAIL("wrong token"); cleanup_temp_env(dir); return; }
    if (strcmp(config.owner, "test-org") != 0) { FAIL("wrong owner"); cleanup_temp_env(dir); return; }
    if (config.repo_count != 2) { FAIL("wrong repo count"); cleanup_temp_env(dir); return; }
    if (strcmp(config.repos[0], "repo-alpha") != 0) { FAIL("wrong repo[0]"); cleanup_temp_env(dir); return; }
    if (strcmp(config.backup_dir, "/tmp/backups/") != 0) { FAIL("wrong backup_dir"); cleanup_temp_env(dir); return; }
    if (config.cycle_interval != 1800) { FAIL("wrong cycle_interval"); cleanup_temp_env(dir); return; }
    if (config.http_timeout != 60000) { FAIL("wrong http_timeout"); cleanup_temp_env(dir); return; }
    if (config.connectivity_timeout != 10000) { FAIL("wrong connectivity_timeout"); cleanup_temp_env(dir); return; }
    if (config.log_max_size != 2097152) { FAIL("wrong log_max_size"); cleanup_temp_env(dir); return; }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_missing_mandatory(void) {
    TEST("parse_env_file - missing REPOS fails validation");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_norepos/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_BASE_URL=https://ghp_test@github.com/org/\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result == 0) { FAIL("should fail with missing REPOS"); cleanup_temp_env(dir); return; }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_defaults_applied(void) {
    TEST("parse_env_file - optional values get defaults when missing");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_defaults/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_BASE_URL=https://ghp_test@github.com/org/\n"
        "REPOS=my-repo\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed"); cleanup_temp_env(dir); return; }

    /* Optional values should have defaults */
    if (config.cycle_interval != 3600) { FAIL("wrong default cycle_interval"); cleanup_temp_env(dir); return; }
    if (config.http_timeout != 30000) { FAIL("wrong default http_timeout"); cleanup_temp_env(dir); return; }
    if (config.connectivity_timeout != 5000) { FAIL("wrong default connectivity_timeout"); cleanup_temp_env(dir); return; }
    if (config.log_max_size != 1048576) { FAIL("wrong default log_max_size"); cleanup_temp_env(dir); return; }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_standalone_token(void) {
    TEST("parse_env_file - GITHUB_TOKEN + GITHUB_OWNER instead of base URL");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_token/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_TOKEN=ghp_standalone_token\n"
        "GITHUB_OWNER=my-org\n"
        "REPOS=repo1,repo2\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed"); cleanup_temp_env(dir); return; }

    if (strcmp(config.token, "ghp_standalone_token") != 0) { FAIL("wrong token"); cleanup_temp_env(dir); return; }
    if (strcmp(config.owner, "my-org") != 0) { FAIL("wrong owner"); cleanup_temp_env(dir); return; }
    if (config.repo_count != 2) { FAIL("wrong repo count"); cleanup_temp_env(dir); return; }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_comments_and_blanks(void) {
    TEST("parse_env_file - comments and blank lines are ignored");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_comments/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "# This is a comment\n"
        "\n"
        "GITHUB_BASE_URL=https://ghp_test@github.com/org/\n"
        "  # Another comment\n"
        "REPOS=repo1\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed with comments"); cleanup_temp_env(dir); return; }

    if (config.repo_count != 1) { FAIL("wrong repo count"); cleanup_temp_env(dir); return; }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_invalid_integer(void) {
    TEST("parse_env_file - invalid integer value uses default (0 → default)");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_badint/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_BASE_URL=https://ghp_test@github.com/org/\n"
        "REPOS=repo1\n"
        "CYCLE_INTERVAL_SECONDS=not_a_number\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse should still succeed"); cleanup_temp_env(dir); return; }

    /* Invalid integer parsed as 0 → apply_defaults sets 3600 */
    if (config.cycle_interval != 3600) { FAIL("wrong default for invalid int"); cleanup_temp_env(dir); return; }

    cleanup_temp_env(dir);
    PASS();
}


/* ─── Input Validation Tests ─────────────────────────────── */

static void test_parse_env_bad_token_prefix(void) {
    TEST("parse_env_file - GITHUB_TOKEN without ghp_ prefix is rejected");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_badtoken/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_TOKEN=invalid_token_no_prefix\n"
        "GITHUB_OWNER=my-org\n"
        "REPOS=repo1\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result == 0) { FAIL("should reject token without ghp_ prefix"); cleanup_temp_env(dir); return; }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_valid_token_prefix(void) {
    TEST("parse_env_file - GITHUB_TOKEN with ghp_ prefix is accepted");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_goodtoken/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_TOKEN=ghp_valid_token_here\n"
        "GITHUB_OWNER=my-org\n"
        "REPOS=repo1\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("should accept token with ghp_ prefix"); cleanup_temp_env(dir); return; }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_cycle_interval_too_low(void) {
    TEST("parse_env_file - CYCLE_INTERVAL_SECONDS below 60 is reset to default");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_lowcycle/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_BASE_URL=https://ghp_test@github.com/org/\n"
        "REPOS=repo1\n"
        "CYCLE_INTERVAL_SECONDS=10\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed"); cleanup_temp_env(dir); return; }

    /* 10 is below 60 — should be reset to 3600 */
    if (config.cycle_interval != 3600) { FAIL("cycle_interval not reset to default"); cleanup_temp_env(dir); return; }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_cycle_interval_zero(void) {
    TEST("parse_env_file - CYCLE_INTERVAL_SECONDS=0 is reset to default");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_zerocycle/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_BASE_URL=https://ghp_test@github.com/org/\n"
        "REPOS=repo1\n"
        "CYCLE_INTERVAL_SECONDS=0\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed"); cleanup_temp_env(dir); return; }

    /* 0 is below 60 — should be reset to 3600 */
    if (config.cycle_interval != 3600) { FAIL("cycle_interval not reset to default"); cleanup_temp_env(dir); return; }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_backup_dir_trailing_slash(void) {
    TEST("parse_env_file - BACKUP_DIR without trailing slash gets one added");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_noslash/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_BASE_URL=https://ghp_test@github.com/org/\n"
        "REPOS=repo1\n"
        "BACKUP_DIR=/tmp/backups\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed"); cleanup_temp_env(dir); return; }

    /* Should have trailing slash added */
    size_t len = strlen(config.backup_dir);
    if (config.backup_dir[len - 1] != '/') {
        FAIL("trailing slash not added to BACKUP_DIR");
        cleanup_temp_env(dir);
        return;
    }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_base_url_no_https(void) {
    TEST("parse_env_file - GITHUB_BASE_URL without https:// is rejected");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_nourl/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_BASE_URL=http://ghp_test@github.com/org/\n"
        "REPOS=repo1\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result == 0) { FAIL("should reject base URL without https://"); cleanup_temp_env(dir); return; }

    cleanup_temp_env(dir);
    PASS();
}

static void test_parse_env_base_url_no_trailing_slash(void) {
    TEST("parse_env_file - GITHUB_BASE_URL without trailing / is rejected");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_notrailing/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_BASE_URL=https://ghp_test@github.com/org\n"
        "REPOS=repo1\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result == 0) { FAIL("should reject base URL without trailing slash"); cleanup_temp_env(dir); return; }

    cleanup_temp_env(dir);
    PASS();
}

static void test_parse_env_base_url_with_trailing_slash(void) {
    TEST("parse_env_file - GITHUB_BASE_URL with trailing / is accepted");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_withtrailing/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_BASE_URL=https://ghp_test@github.com/org/\n"
        "REPOS=repo1\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("should accept base URL with trailing slash"); cleanup_temp_env(dir); return; }

    cleanup_temp_env(dir);
    PASS();
}


/* ─── Precedence and Edge Case Tests ─────────────────────── */

static void test_parse_env_token_precedence(void) {
    TEST("parse_env_file - GITHUB_TOKEN takes precedence over URL token");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_precedence/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_BASE_URL=https://ghp_url_token@github.com/org/\n"
        "GITHUB_TOKEN=ghp_standalone_token\n"
        "REPOS=repo1\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed"); cleanup_temp_env(dir); return; }

    /* Standalone GITHUB_TOKEN should take precedence over URL-embedded token */
    if (strcmp(config.token, "ghp_standalone_token") != 0) {
        FAIL("standalone token should take precedence");
        cleanup_temp_env(dir); return;
    }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_shutdown_interval(void) {
    TEST("parse_env_file - SHUTDOWN_CHECK_INTERVAL_MS parsed correctly");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_shutdown/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_BASE_URL=https://ghp_test@github.com/org/\n"
        "REPOS=repo1\n"
        "SHUTDOWN_CHECK_INTERVAL_MS=500\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed"); cleanup_temp_env(dir); return; }

    if (config.shutdown_check_interval != 500) {
        FAIL("shutdown_check_interval not parsed correctly");
        cleanup_temp_env(dir); return;
    }

    cleanup_temp_env(dir);
    PASS();
}


static void test_extract_token_empty_between_separators(void) {
    TEST("extract_token - empty token between :// and @ fails");

    char token[MAX_TOKEN_LEN] = {0};
    int result = extract_token("https://@github.com/org/", token);

    if (result == 0) { FAIL("should fail for empty token"); return; }
    PASS();
}


static void test_extract_owner_with_path_after(void) {
    TEST("extract_owner - URL with additional path segments");

    char owner[MAX_REPO_NAME_LEN] = {0};
    int result = extract_owner(
        "https://ghp_test@github.com/my-org/some/extra/path",
        owner
    );

    if (result != 0) { FAIL("returned error"); return; }
    if (strcmp(owner, "my-org") != 0) { FAIL("wrong owner - should stop at first /"); return; }
    PASS();
}


static void test_parse_env_base_url_and_owner(void) {
    TEST("parse_env_file - GITHUB_OWNER fills owner when base_url lacks it");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_urlowner/";
    cleanup_temp_env(dir);

    /* GITHUB_BASE_URL has token but owner extraction might fail for unusual URLs.
     * GITHUB_OWNER should be used as fallback. This tests the extraction
     * precedence: standalone fields fill in when URL extraction leaves gaps. */
    write_temp_env(dir,
        "GITHUB_TOKEN=ghp_test_token\n"
        "GITHUB_OWNER=standalone-owner\n"
        "REPOS=repo1\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed"); cleanup_temp_env(dir); return; }

    if (strcmp(config.owner, "standalone-owner") != 0) {
        FAIL("owner should be from GITHUB_OWNER");
        cleanup_temp_env(dir); return;
    }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_missing_env_file(void) {
    TEST("parse_env_file - nonexistent .env file returns error");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_missing/";
    cleanup_temp_env(dir);
    /* Don't write any .env file */

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result == 0) { FAIL("should fail for missing .env"); cleanup_temp_env(dir); return; }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_both_base_url_and_standalone(void) {
    TEST("parse_env_file - both base_url and standalone fields: standalone wins");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_env_both/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_BASE_URL=https://ghp_urltoken@github.com/url-org/\n"
        "GITHUB_TOKEN=ghp_standalone_token\n"
        "GITHUB_OWNER=standalone-org\n"
        "REPOS=repo1\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed"); cleanup_temp_env(dir); return; }

    /* Standalone GITHUB_TOKEN takes precedence for token.
     * GITHUB_OWNER takes precedence for owner since it was set
     * before extract_owner runs (which skips if owner already set). */
    if (strcmp(config.token, "ghp_standalone_token") != 0) {
        FAIL("standalone token should win");
        cleanup_temp_env(dir); return;
    }
    if (strcmp(config.owner, "standalone-org") != 0) {
        FAIL("standalone owner should win");
        cleanup_temp_env(dir); return;
    }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_negative_cycle_interval(void) {
    TEST("parse_env_file - negative CYCLE_INTERVAL_SECONDS resets to default");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_neg_cycle/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_TOKEN=ghp_testtoken123\n"
        "GITHUB_OWNER=test-org\n"
        "REPOS=repo1\n"
        "CYCLE_INTERVAL_SECONDS=-1\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed"); cleanup_temp_env(dir); return; }

    /* validate_cycle_interval resets values below 60 to 3600 */
    if (config.cycle_interval != 3600) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "expected 3600, got %d", config.cycle_interval);
        FAIL(msg);
        cleanup_temp_env(dir); return;
    }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_empty_backups_dir(void) {
    TEST("parse_env_file - empty BACKUP_DIR gets default with trailing separator");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_empty_backupdir/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_TOKEN=ghp_testtoken123\n"
        "GITHUB_OWNER=test-org\n"
        "REPOS=repo1\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed"); cleanup_temp_env(dir); return; }

    /* With empty BACKUP_DIR, apply_defaults sets DEFAULT_BACKUP_DIR which has trailing slash */
    if (config.backup_dir[0] == '\0') {
        FAIL("backup_dir should not be empty after apply_defaults");
        cleanup_temp_env(dir); return;
    }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_large_cycle_interval(void) {
    TEST("parse_env_file - large CYCLE_INTERVAL_SECONDS accepted as-is");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_large_cycle/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_TOKEN=ghp_testtoken123\n"
        "GITHUB_OWNER=test-org\n"
        "REPOS=repo1\n"
        "CYCLE_INTERVAL_SECONDS=86400\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed"); cleanup_temp_env(dir); return; }

    if (config.cycle_interval != 86400) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "expected 86400, got %d", config.cycle_interval);
        FAIL(msg);
        cleanup_temp_env(dir); return;
    }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_whitespace_in_values(void) {
    TEST("parse_env_file - whitespace around values is trimmed");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_ws_values/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_TOKEN = ghp_testtoken123 \n"
        "GITHUB_OWNER = test-org \n"
        "REPOS = repo1 , repo2 \n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed"); cleanup_temp_env(dir); return; }

    if (strcmp(config.token, "ghp_testtoken123") != 0) {
        FAIL("token not trimmed correctly");
        cleanup_temp_env(dir); return;
    }

    cleanup_temp_env(dir);
    PASS();
}


static void test_parse_env_http_timeout_zero(void) {
    TEST("parse_env_file - HTTP_TIMEOUT_MS=0 gets default from apply_defaults");

    ghb_context ctx = create_test_context();
    const char *dir = "/tmp/ghb_test_http_zero/";
    cleanup_temp_env(dir);

    write_temp_env(dir,
        "GITHUB_TOKEN=ghp_testtoken123\n"
        "GITHUB_OWNER=test-org\n"
        "REPOS=repo1\n"
        "HTTP_TIMEOUT_MS=0\n"
    );

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", dir);

    int result = parse_env_file(&ctx, &config);
    if (result != 0) { FAIL("parse failed"); cleanup_temp_env(dir); return; }

    /* apply_defaults sets http_timeout=30000 when it's 0 */
    if (config.http_timeout != 30000) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "expected 30000, got %d", config.http_timeout);
        FAIL(msg);
        cleanup_temp_env(dir); return;
    }

    cleanup_temp_env(dir);
    PASS();
}


/* ─── Main ─────────────────────────────────────────────────── */

int main(void) {
    printf("=== Config Module Tests ===\n\n");

    /* Token extraction */
    printf("-- Token extraction tests --\n");
    test_token_extraction_valid();
    test_token_extraction_no_at();
    test_token_extraction_no_scheme();

    /* Owner extraction */
    printf("\n-- Owner extraction tests --\n");
    test_owner_extraction_valid();
    test_owner_extraction_no_trailing_slash();
    test_owner_extraction_no_prefix();

    /* Repo parsing */
    printf("\n-- Repo parsing tests --\n");
    test_parse_repos_multiple();
    test_parse_repos_single();
    test_parse_repos_comments_and_blanks();

    /* Path construction */
    printf("\n-- Path construction tests --\n");
    test_build_env_path();
    test_build_log_path();

    /* Apply defaults */
    printf("\n-- Apply defaults tests --\n");
    test_apply_defaults_empty();
    test_apply_defaults_preserves_set();
    test_apply_defaults_partial();

    /* Validate config */
    printf("\n-- Validate config tests --\n");
    test_validate_config_valid();
    test_validate_config_no_auth();
    test_validate_config_no_repos();
    test_validate_config_no_token();
    test_validate_config_no_owner();
    test_validate_config_standalone_token();

    /* Ensure dir exists */
    printf("\n-- Ensure dir exists tests --\n");
    test_ensure_dir_creates();
    test_ensure_dir_already_exists();
    test_ensure_dir_empty_path();

    /* Parse env file integration */
    printf("\n-- Parse env file integration tests --\n");
    test_parse_env_valid();
    test_parse_env_missing_mandatory();
    test_parse_env_defaults_applied();
    test_parse_env_standalone_token();
    test_parse_env_comments_and_blanks();
    test_parse_env_invalid_integer();

    /* Input validation */
    printf("\n-- Input validation tests --\n");
    test_parse_env_bad_token_prefix();
    test_parse_env_valid_token_prefix();
    test_parse_env_cycle_interval_too_low();
    test_parse_env_cycle_interval_zero();
    test_parse_env_backup_dir_trailing_slash();
    test_parse_env_base_url_no_https();
    test_parse_env_base_url_no_trailing_slash();
    test_parse_env_base_url_with_trailing_slash();

    /* Precedence and edge cases */
    printf("\n-- Precedence and edge case tests --\n");
    test_parse_env_token_precedence();
    test_parse_env_shutdown_interval();
    test_extract_token_empty_between_separators();
    test_extract_owner_with_path_after();
    test_parse_env_base_url_and_owner();
    test_parse_env_missing_env_file();
    test_parse_env_both_base_url_and_standalone();

    /* Additional validation edge cases */
    printf("\n-- Additional validation edge cases --\n");
    test_parse_env_negative_cycle_interval();
    test_parse_env_empty_backups_dir();
    test_parse_env_large_cycle_interval();
    test_parse_env_whitespace_in_values();
    test_parse_env_http_timeout_zero();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
