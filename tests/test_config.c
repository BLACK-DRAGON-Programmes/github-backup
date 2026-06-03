/**
 * test_config.c — Unit tests for the configuration module.
 *
 * Tests env file parsing, token/owner extraction, repo list parsing,
 * default value application, and validation.
 *
 * Compile: gcc -o test_config tests/test_config.c src/config.c src/logger.c src/notify.c
 */

#include "config.h"
#include "logger.h"
#include "notify.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>


static int tests_passed = 0;
static int tests_failed = 0;


#define TEST(name) printf("  TEST: %s... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)


/* ─── Token Extraction Tests ──────────────────────────────── */

static void test_token_extraction_valid(void) {
    TEST("extract_token — valid URL");

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
    TEST("extract_token — no @ sign");

    char token[MAX_TOKEN_LEN] = {0};
    int result = extract_token("https://github.com/owner/", token);

    if (result == 0) { FAIL("should have failed"); return; }
    PASS();
}


static void test_token_extraction_no_scheme(void) {
    TEST("extract_token — no scheme");

    char token[MAX_TOKEN_LEN] = {0};
    int result = extract_token("ghp_token@github.com/owner/", token);

    if (result == 0) { FAIL("should have failed"); return; }
    PASS();
}


/* ─── Owner Extraction Tests ───────────────────────────────── */

static void test_owner_extraction_valid(void) {
    TEST("extract_owner — valid URL with trailing slash");

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
    TEST("extract_owner — no trailing slash");

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
    TEST("extract_owner — no github.com prefix");

    char owner[MAX_REPO_NAME_LEN] = {0};
    int result = extract_owner("https://ghp_token@gitlab.com/owner/", owner);

    if (result == 0) { FAIL("should have failed"); return; }
    PASS();
}


/* ─── Repo Parsing Tests ──────────────────────────────────── */

static void test_parse_repos_multiple(void) {
    TEST("parse_repos — multiple repos");

    char repos[MAX_REPOS][MAX_REPO_NAME_LEN] = {0};
    int count = 0;
    int result = parse_repos("repo-one, repo-two,repo-three", repos, &count);

    if (result != 0) { FAIL("returned error"); return; }
    if (count != 3) { FAIL("wrong count"); return; }
    if (strcmp(repos[0], "repo-one") != 0) { FAIL("repo[0] mismatch"); return; }
    if (strcmp(repos[1], "repo-two") != 0) { FAIL("repo[1] mismatch"); return; }
    if (strcmp(repos[2], "repo-three") != 0) { FAIL("repo[2] mismatch"); return; }
    PASS();
}


static void test_parse_repos_single(void) {
    TEST("parse_repos — single repo");

    char repos[MAX_REPOS][MAX_REPO_NAME_LEN] = {0};
    int count = 0;
    int result = parse_repos("my-repo", repos, &count);

    if (result != 0) { FAIL("returned error"); return; }
    if (count != 1) { FAIL("wrong count"); return; }
    if (strcmp(repos[0], "my-repo") != 0) { FAIL("repo mismatch"); return; }
    PASS();
}


static void test_parse_repos_comments_and_blanks(void) {
    TEST("parse_repos — skip comments and blanks");

    char repos[MAX_REPOS][MAX_REPO_NAME_LEN] = {0};
    int count = 0;
    int result = parse_repos("#comment,repo-a,,repo-b,# another comment", repos, &count);

    if (result != 0) { FAIL("returned error"); return; }
    if (count != 2) { FAIL("wrong count"); return; }
    if (strcmp(repos[0], "repo-a") != 0) { FAIL("repo[0] mismatch"); return; }
    if (strcmp(repos[1], "repo-b") != 0) { FAIL("repo[1] mismatch"); return; }
    PASS();
}


/* ─── Path Construction Tests ─────────────────────────────── */

static void test_build_env_path(void) {
    TEST("build_env_path");

    char path[MAX_URL_LEN] = {0};
    build_env_path("D:\\BACKUP\\", path);

    if (strcmp(path, "D:\\BACKUP\\.env") != 0) {
        FAIL("wrong path");
        return;
    }
    PASS();
}


static void test_build_log_path(void) {
    TEST("build_log_path");

    char path[MAX_URL_LEN] = {0};
    build_log_path("D:\\BACKUP\\", path);

    if (strcmp(path, "D:\\BACKUP\\backup.log") != 0) {
        FAIL("wrong path");
        return;
    }
    PASS();
}


/* ─── Main ─────────────────────────────────────────────────── */

int main(void) {
    log_init("/tmp/test_config.log");
    notify_init();

    printf("=== Config Module Tests ===\n\n");

    test_token_extraction_valid();
    test_token_extraction_no_at();
    test_token_extraction_no_scheme();
    test_owner_extraction_valid();
    test_owner_extraction_no_trailing_slash();
    test_owner_extraction_no_prefix();
    test_parse_repos_multiple();
    test_parse_repos_single();
    test_parse_repos_comments_and_blanks();
    test_build_env_path();
    test_build_log_path();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    log_close();
    notify_cleanup();

    return tests_failed > 0 ? 1 : 0;
}
