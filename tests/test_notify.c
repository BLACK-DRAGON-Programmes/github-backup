/**
 * test_notify.c - Unit tests for the notify module.
 *
 * Tests the notification lifecycle: init, toast functions (info, success,
 * error), and cleanup. On Linux, toasts are stubs that log events —
 * verify the log entries are written correctly.
 *
 * Compile (Linux):  gcc -o test_notify tests/test_notify.c src/notify.c src/logger.c src/console.c src/config.c src/network.c src/backup.c -I src/
 */

#include "context.h"
#include "logger_iface.h"
#include "logger.h"
#include "notify_iface.h"
#include "notify.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>


static int tests_passed = 0;
static int tests_failed = 0;


#define TEST(name) printf("  TEST: %s... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)


/* ─── Fake and Real Logger Ops for Testing ─────────────────── */

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

/* Real logger ops — points to actual logger functions for log-verification tests */
static const logger_ops real_logger = {
    .log_event = log_event,
    .log_error = log_error,
    .log_init = log_init,
    .log_close = log_close,
    .rotate_log = rotate_log,
};


static ghb_context create_test_context(void) {
    ghb_context ctx;
    ctx.logger = &fake_logger;
    ctx.notify = NULL;  /* Will be set after notify_init */
    ctx.network = NULL;
    ctx.should_stop = NULL;
    return ctx;
}


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


/* ─── Init/Cleanup Lifecycle Tests ────────────────────────── */

static void test_notify_init(void) {
    TEST("notify_init — returns 0 on Linux stub");

    ghb_context ctx = create_test_context();
    int result = notify_init(&ctx);
    if (result != 0) { FAIL("notify_init returned error on Linux stub"); return; }

    notify_cleanup(&ctx);
    PASS();
}


static void test_notify_cleanup_no_init(void) {
    TEST("notify_cleanup — safe without init (no crash)");

    ghb_context ctx = create_test_context();
    /* Call cleanup without init — should not crash */
    notify_cleanup(&ctx);
    PASS();
}


static void test_notify_init_cleanup_cycle(void) {
    TEST("notify_init/cleanup — full lifecycle (init, use, cleanup)");

    ghb_context ctx = create_test_context();
    int result = notify_init(&ctx);
    if (result != 0) { FAIL("init failed"); return; }

    /* Use the module briefly */
    toast_info(&ctx, "Test", "Lifecycle test");

    notify_cleanup(&ctx);
    PASS();
}


/* ─── Toast Function Tests (Linux stubs) ──────────────────── */

static void test_toast_info_writes_log(void) {
    TEST("toast_info — logs INFO event on Linux");

    ghb_context ctx = create_test_context();
    ctx.logger = &real_logger;  /* Need real logger to verify log output */

    const char *path = "/tmp/ghb_test_notify_info.log";
    remove(path);
    log_init(&ctx, path);
    notify_init(&ctx);

    toast_info(&ctx, "Test Title", "Test info message");

    log_close(&ctx);

    char *contents = read_file_contents(path);
    if (contents == NULL) { FAIL("could not read log file"); notify_cleanup(&ctx); remove(path); return; }

    if (strstr(contents, "INFO") == NULL) { FAIL("missing INFO level"); free(contents); notify_cleanup(&ctx); remove(path); return; }
    if (strstr(contents, "toast") == NULL) { FAIL("missing toast action"); free(contents); notify_cleanup(&ctx); remove(path); return; }
    if (strstr(contents, "Test info message") == NULL) { FAIL("missing message"); free(contents); notify_cleanup(&ctx); remove(path); return; }

    free(contents);
    notify_cleanup(&ctx);
    remove(path);
    PASS();
}


static void test_toast_success_writes_log(void) {
    TEST("toast_success — logs SUCCESS event with repo name");

    ghb_context ctx = create_test_context();
    ctx.logger = &real_logger;  /* Need real logger to verify log output */

    const char *path = "/tmp/ghb_test_notify_success.log";
    remove(path);
    log_init(&ctx, path);
    notify_init(&ctx);

    toast_success(&ctx, "my-repo", "Downloaded 500 KB");

    log_close(&ctx);

    char *contents = read_file_contents(path);
    if (contents == NULL) { FAIL("could not read log file"); notify_cleanup(&ctx); remove(path); return; }

    if (strstr(contents, "OK") == NULL) { FAIL("missing OK level"); free(contents); notify_cleanup(&ctx); remove(path); return; }
    if (strstr(contents, "my-repo") == NULL) { FAIL("missing repo name"); free(contents); notify_cleanup(&ctx); remove(path); return; }
    if (strstr(contents, "Downloaded 500 KB") == NULL) { FAIL("missing message"); free(contents); notify_cleanup(&ctx); remove(path); return; }

    free(contents);
    notify_cleanup(&ctx);
    remove(path);
    PASS();
}


static void test_toast_error_writes_log(void) {
    TEST("toast_error — logs ERROR event with FAILED status");

    ghb_context ctx = create_test_context();
    ctx.logger = &real_logger;  /* Need real logger to verify log output */

    const char *path = "/tmp/ghb_test_notify_error.log";
    remove(path);
    log_init(&ctx, path);
    notify_init(&ctx);

    toast_error(&ctx, "Download Failed", "HTTP 404");

    log_close(&ctx);

    char *contents = read_file_contents(path);
    if (contents == NULL) { FAIL("could not read log file"); notify_cleanup(&ctx); remove(path); return; }

    if (strstr(contents, "ERROR") == NULL) { FAIL("missing ERROR level"); free(contents); notify_cleanup(&ctx); remove(path); return; }
    if (strstr(contents, "FAILED") == NULL) { FAIL("missing FAILED status"); free(contents); notify_cleanup(&ctx); remove(path); return; }
    if (strstr(contents, "HTTP 404") == NULL) { FAIL("missing error detail"); free(contents); notify_cleanup(&ctx); remove(path); return; }

    free(contents);
    notify_cleanup(&ctx);
    remove(path);
    PASS();
}


static void test_toast_info_null_title(void) {
    TEST("toast_info — NULL title does not crash (Linux stub)");

    ghb_context ctx = create_test_context();

    notify_init(&ctx);

    /* Should not crash — Linux stub ignores the title parameter */
    toast_info(&ctx, NULL, "message with null title");

    notify_cleanup(&ctx);
    PASS();
}


static void test_toast_error_null_title(void) {
    TEST("toast_error — NULL title does not crash (Linux stub)");

    ghb_context ctx = create_test_context();

    notify_init(&ctx);

    /* Should not crash */
    toast_error(&ctx, NULL, "error with null title");

    notify_cleanup(&ctx);
    PASS();
}


/* ─── Multiple Toasts Test ─────────────────────────────────── */

static void test_multiple_toasts(void) {
    TEST("Multiple toasts — all logged correctly");

    ghb_context ctx = create_test_context();
    ctx.logger = &real_logger;  /* Need real logger to verify log output */

    const char *path = "/tmp/ghb_test_notify_multi.log";
    remove(path);
    log_init(&ctx, path);
    notify_init(&ctx);

    toast_info(&ctx, "Cycle Start", "3 repos to back up");
    toast_success(&ctx, "repo-a", "Downloaded 100 KB");
    toast_success(&ctx, "repo-b", "Downloaded 200 KB");
    toast_error(&ctx, "Download Failed", "repo-c: HTTP 500");

    log_close(&ctx);

    char *contents = read_file_contents(path);
    if (contents == NULL) { FAIL("could not read log file"); notify_cleanup(&ctx); remove(path); return; }

    if (strstr(contents, "3 repos") == NULL) { FAIL("missing info toast"); free(contents); notify_cleanup(&ctx); remove(path); return; }
    if (strstr(contents, "repo-a") == NULL) { FAIL("missing success toast a"); free(contents); notify_cleanup(&ctx); remove(path); return; }
    if (strstr(contents, "repo-b") == NULL) { FAIL("missing success toast b"); free(contents); notify_cleanup(&ctx); remove(path); return; }
    if (strstr(contents, "repo-c") == NULL) { FAIL("missing error toast"); free(contents); notify_cleanup(&ctx); remove(path); return; }

    free(contents);
    notify_cleanup(&ctx);
    remove(path);
    PASS();
}


/* ─── Main ─────────────────────────────────────────────────── */

int main(void) {
    printf("=== Notify Module Tests ===\n\n");

    /* Lifecycle */
    printf("-- Init/cleanup lifecycle tests --\n");
    test_notify_init();
    test_notify_cleanup_no_init();
    test_notify_init_cleanup_cycle();

    /* Toast functions */
    printf("\n-- Toast function tests (Linux stubs) --\n");
    test_toast_info_writes_log();
    test_toast_success_writes_log();
    test_toast_error_writes_log();
    test_toast_info_null_title();
    test_toast_error_null_title();

    /* Multiple toasts */
    printf("\n-- Multiple toasts test --\n");
    test_multiple_toasts();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
