/**
 * test_logger.c - Unit tests for the logger module.
 *
 * Tests log initialization, structured event writing, error shorthand,
 * log rotation, and log close.
 * Verifies file contents after writes to ensure correct format.
 *
 * Compile (Linux):  gcc -o test_logger tests/test_logger.c src/logger.c src/console.c src/config.c src/notify.c src/network.c src/backup.c -I src/
 */

#include "logger.h"
#include "logger_iface.h"
#include "context.h"
#include "config.h"
#include "notify.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/stat.h>


static int tests_passed = 0;
static int tests_failed = 0;


#define TEST(name) printf("  TEST: %s... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)


/* ─── Helpers ──────────────────────────────────────────────── */

/** Read the entire contents of a file into a malloc'd buffer. */
static char *read_file_contents(const char *path) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) { fclose(fp); return NULL; }

    size_t nread = fread(buf, 1, (size_t)size, fp);
    buf[nread] = '\0';
    fclose(fp);
    return buf;
}


/* ─── Log Init Tests ──────────────────────────────────────── */

static void test_log_init_valid(void) {
    TEST("log_init — valid path");

    const char *path = "/tmp/ghb_test_logger_init.log";
    remove(path);  /* Ensure clean state */

    int result = log_init(NULL, path);
    if (result != 0) { FAIL("log_init returned error for valid path"); log_close(NULL); return; }

    /* Verify file was created */
    FILE *fp = fopen(path, "r");
    if (fp == NULL) { FAIL("log file not created"); log_close(NULL); return; }
    fclose(fp);

    log_close(NULL);
    remove(path);
    PASS();
}


static void test_log_init_invalid_path(void) {
    TEST("log_init — invalid path returns -1");

    int result = log_init(NULL, "/nonexistent/dir/that/does/not/exist/test.log");
    if (result == 0) {
        FAIL("should return -1 for invalid path");
        log_close(NULL);
        return;
    }

    /* Should not crash — falls back to stderr */
    log_close(NULL);
    PASS();
}


/* ─── Log Event Tests ─────────────────────────────────────── */

static void test_log_event_info(void) {
    TEST("log_event — INFO level writes to file");

    const char *path = "/tmp/ghb_test_logger_info.log";
    remove(path);
    log_init(NULL, path);

    log_event(NULL, LOG_INFO, "test-action", NULL, "OK", "test detail");

    log_close(NULL);  /* Flush before reading */

    char *contents = read_file_contents(path);
    if (contents == NULL) { FAIL("could not read log file"); remove(path); return; }

    if (strstr(contents, "INFO") == NULL) { FAIL("missing INFO level"); free(contents); remove(path); return; }
    if (strstr(contents, "test-action") == NULL) { FAIL("missing action"); free(contents); remove(path); return; }
    if (strstr(contents, "OK") == NULL) { FAIL("missing status"); free(contents); remove(path); return; }
    if (strstr(contents, "test detail") == NULL) { FAIL("missing detail"); free(contents); remove(path); return; }

    free(contents);
    remove(path);
    PASS();
}


static void test_log_event_success(void) {
    TEST("log_event — SUCCESS level uses OK string");

    const char *path = "/tmp/ghb_test_logger_success.log";
    remove(path);
    log_init(NULL, path);

    log_event(NULL, LOG_SUCCESS, "backup", "my-repo", "OK", "completed");

    log_close(NULL);

    char *contents = read_file_contents(path);
    if (contents == NULL) { FAIL("could not read log file"); remove(path); return; }

    /* SUCCESS level maps to "OK" in the log output */
    if (strstr(contents, "OK") == NULL) { FAIL("missing OK level string"); free(contents); remove(path); return; }
    if (strstr(contents, "my-repo") == NULL) { FAIL("missing repo name"); free(contents); remove(path); return; }

    free(contents);
    remove(path);
    PASS();
}


static void test_log_event_warning(void) {
    TEST("log_event — WARNING level writes WARN string");

    const char *path = "/tmp/ghb_test_logger_warn.log";
    remove(path);
    log_init(NULL, path);

    log_event(NULL, LOG_WARNING, "network", NULL, "SKIPPED", "no internet");

    log_close(NULL);

    char *contents = read_file_contents(path);
    if (contents == NULL) { FAIL("could not read log file"); remove(path); return; }

    if (strstr(contents, "WARN") == NULL) { FAIL("missing WARN level string"); free(contents); remove(path); return; }

    free(contents);
    remove(path);
    PASS();
}


static void test_log_event_error(void) {
    TEST("log_event — ERROR level writes ERROR string");

    const char *path = "/tmp/ghb_test_logger_error.log";
    remove(path);
    log_init(NULL, path);

    log_event(NULL, LOG_ERROR, "config", NULL, "FAILED", "missing .env");

    log_close(NULL);

    char *contents = read_file_contents(path);
    if (contents == NULL) { FAIL("could not read log file"); remove(path); return; }

    if (strstr(contents, "ERROR") == NULL) { FAIL("missing ERROR level string"); free(contents); remove(path); return; }

    free(contents);
    remove(path);
    PASS();
}


static void test_log_event_no_repo(void) {
    TEST("log_event — NULL repo omits repo column");

    const char *path = "/tmp/ghb_test_logger_norepo.log";
    remove(path);
    log_init(NULL, path);

    log_event(NULL, LOG_INFO, "startup", NULL, "OK", "service started");

    log_close(NULL);

    char *contents = read_file_contents(path);
    if (contents == NULL) { FAIL("could not read log file"); remove(path); return; }

    /* Entry should exist with action and status even without repo */
    if (strstr(contents, "startup") == NULL) { FAIL("missing action"); free(contents); remove(path); return; }
    if (strstr(contents, "service started") == NULL) { FAIL("missing detail"); free(contents); remove(path); return; }

    free(contents);
    remove(path);
    PASS();
}


static void test_log_event_no_detail(void) {
    TEST("log_event — NULL detail omits detail column");

    const char *path = "/tmp/ghb_test_logger_nodetail.log";
    remove(path);
    log_init(NULL, path);

    log_event(NULL, LOG_INFO, "cycle", "repo1", "OK", NULL);

    log_close(NULL);

    char *contents = read_file_contents(path);
    if (contents == NULL) { FAIL("could not read log file"); remove(path); return; }

    if (strstr(contents, "cycle") == NULL) { FAIL("missing action"); free(contents); remove(path); return; }
    if (strstr(contents, "repo1") == NULL) { FAIL("missing repo"); free(contents); remove(path); return; }

    free(contents);
    remove(path);
    PASS();
}


static void test_log_event_timestamp_format(void) {
    TEST("log_event — entry starts with ISO 8601 timestamp [YYYY-MM-DD HH:MM:SS]");

    const char *path = "/tmp/ghb_test_logger_ts.log";
    remove(path);
    log_init(NULL, path);

    log_event(NULL, LOG_INFO, "test", NULL, "OK", NULL);

    log_close(NULL);

    char *contents = read_file_contents(path);
    if (contents == NULL) { FAIL("could not read log file"); remove(path); return; }

    /* Check timestamp format: [YYYY-MM-DD HH:MM:SS] */
    if (contents[0] != '[') { FAIL("entry does not start with ["); free(contents); remove(path); return; }
    /* Check dash separators in date */
    if (contents[5] != '-' || contents[8] != '-') {
        FAIL("date not in YYYY-MM-DD format");
        free(contents); remove(path); return;
    }

    free(contents);
    remove(path);
    PASS();
}


static void test_log_event_separator(void) {
    TEST("log_event — uses ASCII pipe separator (no mojibake on Windows)");

    const char *path = "/tmp/ghb_test_logger_sep.log";
    remove(path);
    log_init(NULL, path);

    log_event(NULL, LOG_INFO, "test", NULL, "OK", NULL);

    log_close(NULL);

    char *contents = read_file_contents(path);
    if (contents == NULL) { FAIL("could not read log file"); remove(path); return; }

    /* ASCII pipe | is used instead of Unicode box-drawing │ (U+2502)
     * because the Windows console (CP437) renders UTF-8 box-drawing
     * chars as mojibake (Γöé). See R95/R150. */
    if (strstr(contents, " | ") == NULL) {
        FAIL("missing pipe separator");
        free(contents); remove(path); return;
    }

    free(contents);
    remove(path);
    PASS();
}


/* ─── Log Error Shorthand Tests ────────────────────────────── */

static void test_log_error_shorthand(void) {
    TEST("log_error — writes ERROR level with FAILED status");

    const char *path = "/tmp/ghb_test_logerr.log";
    remove(path);
    log_init(NULL, path);

    log_error(NULL, "backup", "my-repo", "disk full error");

    log_close(NULL);

    char *contents = read_file_contents(path);
    if (contents == NULL) { FAIL("could not read log file"); remove(path); return; }

    if (strstr(contents, "ERROR") == NULL) { FAIL("missing ERROR level"); free(contents); remove(path); return; }
    if (strstr(contents, "FAILED") == NULL) { FAIL("missing FAILED status"); free(contents); remove(path); return; }
    if (strstr(contents, "disk full error") == NULL) { FAIL("missing detail"); free(contents); remove(path); return; }

    free(contents);
    remove(path);
    PASS();
}


/* ─── Rotate Log Tests ────────────────────────────────────── */

static void test_rotate_log_disabled(void) {
    TEST("rotate_log — max_size=0 disables rotation");

    const char *path = "/tmp/ghb_test_logger_rotate0.log";
    remove(path);
    log_init(NULL, path);

    log_event(NULL, LOG_INFO, "test", NULL, "OK", "some data");

    /* Rotation with 0 = disabled — file should still exist after */
    rotate_log(NULL, 0);

    FILE *fp = fopen(path, "r");
    if (fp == NULL) { FAIL("file deleted despite rotation disabled"); log_close(NULL); return; }
    fclose(fp);

    log_close(NULL);
    remove(path);
    PASS();
}


static void test_rotate_log_under_threshold(void) {
    TEST("rotate_log — file under threshold is kept");

    const char *path = "/tmp/ghb_test_logger_rotsmall.log";
    remove(path);
    log_init(NULL, path);

    log_event(NULL, LOG_INFO, "test", NULL, "OK", "small content");

    log_close(NULL);  /* Close to ensure data is flushed */

    /* Re-init so rotate_log can measure size */
    log_init(NULL, path);

    /* Threshold is 1MB — our tiny file should be well under */
    rotate_log(NULL, 1048576);

    FILE *fp = fopen(path, "r");
    if (fp == NULL) { FAIL("file deleted despite being under threshold"); log_close(NULL); return; }
    fclose(fp);

    log_close(NULL);
    remove(path);
    PASS();
}


static void test_rotate_log_over_threshold(void) {
    TEST("rotate_log — file over threshold is deleted and recreated");

    const char *path = "/tmp/ghb_test_logger_rotbig.log";
    remove(path);
    log_init(NULL, path);

    /* Write enough data to exceed a small threshold */
    for (int i = 0; i < 200; i++) {
        log_event(NULL, LOG_INFO, "fill", NULL, "OK",
                  "padding-content-to-exceed-threshold-limit");
    }

    log_close(NULL);

    /* Re-init and rotate with a tiny threshold (1 byte) */
    log_init(NULL, path);
    rotate_log(NULL, 1);

    /* File should have been recreated (empty or minimal) */
    FILE *fp = fopen(path, "r");
    if (fp == NULL) { FAIL("file not recreated after rotation"); log_close(NULL); return; }
    fclose(fp);

    log_close(NULL);
    remove(path);
    PASS();
}


/* ─── Log Close Tests ─────────────────────────────────────── */

static void test_log_close_safe(void) {
    TEST("log_close — safe to call after init");

    const char *path = "/tmp/ghb_test_logger_close.log";
    remove(path);
    log_init(NULL, path);

    log_event(NULL, LOG_INFO, "test", NULL, "OK", "before close");
    log_close(NULL);

    /* Verify data was written before close */
    char *contents = read_file_contents(path);
    if (contents == NULL) { FAIL("could not read log file"); remove(path); return; }
    if (strstr(contents, "before close") == NULL) { FAIL("data not flushed on close"); free(contents); remove(path); return; }

    free(contents);
    remove(path);
    PASS();
}


static void test_log_close_without_init(void) {
    TEST("log_close — safe to call without init (no crash)");

    /* Call close without init — should not crash */
    log_close(NULL);
    log_close(NULL);  /* Double close — also should not crash */

    PASS();
}


/* ─── Multiple Writes Test ─────────────────────────────────── */

static void test_log_multiple_entries(void) {
    TEST("log_event — multiple entries all written");

    const char *path = "/tmp/ghb_test_logger_multi.log";
    remove(path);
    log_init(NULL, path);

    log_event(NULL, LOG_INFO, "action1", "repo1", "OK", "detail1");
    log_event(NULL, LOG_SUCCESS, "action2", "repo2", "OK", "detail2");
    log_event(NULL, LOG_WARNING, "action3", NULL, "SKIPPED", "detail3");
    log_event(NULL, LOG_ERROR, "action4", "repo4", "FAILED", "detail4");

    log_close(NULL);

    char *contents = read_file_contents(path);
    if (contents == NULL) { FAIL("could not read log file"); remove(path); return; }

    if (strstr(contents, "action1") == NULL) { FAIL("missing action1"); free(contents); remove(path); return; }
    if (strstr(contents, "action2") == NULL) { FAIL("missing action2"); free(contents); remove(path); return; }
    if (strstr(contents, "action3") == NULL) { FAIL("missing action3"); free(contents); remove(path); return; }
    if (strstr(contents, "action4") == NULL) { FAIL("missing action4"); free(contents); remove(path); return; }

    free(contents);
    remove(path);
    PASS();
}


/* ─── Main ─────────────────────────────────────────────────── */

int main(void) {
    /* Build a minimal context with the real logger ops so
     * notify_init can call ctx->logger->log_event safely. */
    static const logger_ops real_logger_for_ctx = {
        .log_event              = log_event,
        .log_error              = log_error,
        .log_init               = log_init,
        .log_close              = log_close,
        .rotate_log             = rotate_log,
    };
    ghb_context ctx;
    ctx.logger  = &real_logger_for_ctx;
    ctx.notify  = NULL;
    ctx.network = NULL;
    ctx.should_stop = NULL;

    notify_init(&ctx);

    printf("=== Logger Module Tests ===\n\n");

    /* Init */
    printf("-- Log init tests --\n");
    test_log_init_valid();
    test_log_init_invalid_path();

    /* Event writing */
    printf("\n-- Log event tests --\n");
    test_log_event_info();
    test_log_event_success();
    test_log_event_warning();
    test_log_event_error();
    test_log_event_no_repo();
    test_log_event_no_detail();
    test_log_event_timestamp_format();
    test_log_event_separator();

    /* Error shorthand */
    printf("\n-- Log error shorthand tests --\n");
    test_log_error_shorthand();

    /* Rotation */
    printf("\n-- Log rotation tests --\n");
    test_rotate_log_disabled();
    test_rotate_log_under_threshold();
    test_rotate_log_over_threshold();

    /* Close */
    printf("\n-- Log close tests --\n");
    test_log_close_safe();
    test_log_close_without_init();

    /* Multiple writes */
    printf("\n-- Multiple writes test --\n");
    test_log_multiple_entries();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    notify_cleanup(&ctx);

    return tests_failed > 0 ? 1 : 0;
}
