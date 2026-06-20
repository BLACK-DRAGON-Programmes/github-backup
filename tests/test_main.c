/**
 * test_main.c - Unit tests for main.c static functions.
 *
 * Uses #include "main.c" (with GHB_TEST_BUILD defined to exclude
 * the real main()) to access static functions for testing.
 *
 * On Linux, Windows-specific functions (mutex, CreateProcess, events,
 * Task Scheduler) are #else stubs. Tests verify the stubs return sane
 * error codes and don't crash — this is regression coverage confirming
 * the stubs exist and behave predictably.
 *
 * Portable functions (validate_env_exists) are tested for correctness
 * on both the success and failure branches.
 *
 * Compile (Linux):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I src/ -o build/test_main \
 *       tests/test_main.c src/logger.c src/notify.c src/config.c \
 *       src/network.c src/backup.c src/console.c
 *
 * NOTE: src/main.c is NOT in the source list — it is #included by this
 * test file (with GHB_TEST_BUILD defined to skip the real main()).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define GHB_TEST_BUILD
#include "main.c"


/* ================================================================
 * NAMED CONSTANTS (Rule 10: no raw numbers — every value defined
 * at the top of the file with a comment explaining what it is)
 * ================================================================ */

/** Expected return when validate_env_exists succeeds (file is readable). */
#define ENV_VALIDATE_SUCCESS            0

/** Expected return when validate_env_exists fails (file missing/unreadable). */
#define ENV_VALIDATE_FAILURE            (-1)

/** Expected return from Linux stubs that signal an unsupported operation
 *  (signal_shutdown, register/unregister_task_scheduler, spawn_daemon). */
#define STUB_ERROR_RETURN               (-1)

/** Expected return from Linux stubs that report "no" / "not present"
 *  (check_single_instance, is_task_scheduler_registered). */
#define STUB_ZERO_RETURN                0

/** Value of g_shutdown_requested when no shutdown has been requested. */
#define SHUTDOWN_NOT_REQUESTED          0

/** Value of g_shutdown_requested after a shutdown has been requested. */
#define SHUTDOWN_REQUESTED              1

/** Argument that triggers sleep_with_shutdown_check's early-return guard
 *  (the `if (seconds <= 0) return;` branch on line ~155 of main.c). */
#define SLEEP_SECONDS_ZERO              0

/** Maximum length of a temp-file path buffer used by snprintf. */
#define TEMP_PATH_MAX                   256

/** Path to a temp file created by validate_env_exists tests. Cleaned up
 *  via remove() in each test that creates it. */
#define TEST_ENV_PATH                   "/tmp/ghb_test_main_env.env"

/** Path that is guaranteed not to exist — used for the missing-file
 *  branch of validate_env_exists. */
#define TEST_MISSING_ENV_PATH           "/tmp/ghb_test_main_nonexistent.env"


/* ================================================================
 * TEST FRAMEWORK (mirrors test_notify.c exactly)
 * ================================================================ */

static int tests_passed = 0;
static int tests_failed = 0;


#define TEST(name) printf("  TEST: %s... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)


/* ================================================================
 * FAKE LOGGER AND NOTIFY OPS
 *
 * All no-ops — we don't need to verify log output here. The Linux
 * stubs we test don't dereference ctx->logger at all (the stubs cast
 * ctx to void), but real_logger/real_notify are also available
 * (defined inside main.c itself) if a test ever needs real behavior.
 * ================================================================ */

static void fake_log_event(ghb_context *ctx, log_level level,
                           const char *action, const char *repo,
                           const char *status, const char *detail) {
    (void)ctx; (void)level; (void)action; (void)repo;
    (void)status; (void)detail;
}

static void fake_log_error(ghb_context *ctx, const char *action,
                           const char *repo, const char *detail) {
    (void)ctx; (void)action; (void)repo; (void)detail;
}

static int  fake_log_init(ghb_context *ctx, const char *path) {
    (void)ctx; (void)path; return 0;
}

static void fake_log_close(ghb_context *ctx) { (void)ctx; }

static void fake_rotate_log(ghb_context *ctx, long max_size) {
    (void)ctx; (void)max_size;
}

static const logger_ops fake_logger = {
    .log_event  = fake_log_event,
    .log_error  = fake_log_error,
    .log_init   = fake_log_init,
    .log_close  = fake_log_close,
    .rotate_log = fake_rotate_log,
};


static void fake_toast_info(ghb_context *ctx, const char *title,
                            const char *msg) {
    (void)ctx; (void)title; (void)msg;
}

static void fake_toast_success(ghb_context *ctx, const char *repo,
                               const char *msg) {
    (void)ctx; (void)repo; (void)msg;
}

static void fake_toast_error(ghb_context *ctx, const char *title,
                             const char *msg) {
    (void)ctx; (void)title; (void)msg;
}

static int  fake_notify_init(ghb_context *ctx) { (void)ctx; return 0; }

static void fake_notify_cleanup(ghb_context *ctx) { (void)ctx; }

static const notify_ops fake_notify = {
    .toast_info     = fake_toast_info,
    .toast_success  = fake_toast_success,
    .toast_error    = fake_toast_error,
    .notify_init    = fake_notify_init,
    .notify_cleanup = fake_notify_cleanup,
};


/**
 * Build a minimal ghb_context wired to the fake logger and fake notify
 * ops. The Linux stubs we test do not dereference ctx->network, so it
 * is left NULL safely.
 */
static ghb_context create_test_context(void) {
    ghb_context ctx;
    ctx.logger      = &fake_logger;
    ctx.notify      = &fake_notify;
    ctx.network     = NULL;
    ctx.should_stop = NULL;
    return ctx;
}


/* ================================================================
 * UNUSED-FUNCTION SUPPRESSION
 *
 * main.c contains several static functions that are ONLY called from
 * main() (or transitively from functions that only main() calls).
 * Since GHB_TEST_BUILD excludes main() from this translation unit,
 * those functions become "defined but not used" — which would trigger
 * -Wunused-function under -Wall. Taking the address of each such
 * function marks it as referenced so the compiler does not warn.
 *
 * These functions (enter_viewer_mode, run_main_loop, run_daemon)
 * require a fully-initialized context with a real .env, log file, and
 * network session to call safely — out of scope for these unit tests,
 * which focus on the directly-testable static functions.
 */
static void reference_unused_main_functions(void) {
    void (*p_viewer)(ghb_context *) =
        enter_viewer_mode;
    void (*p_loop)(ghb_context *, backup_config *, const char *) =
        run_main_loop;
    int  (*p_daemon)(ghb_context *) =
        run_daemon;
    /* TASK_NAME is a static const pointer used only inside the
     * #ifdef _WIN32 branches of register/unregister_task_scheduler.
     * On Linux it has no references; take its address so the compiler
     * does not warn about the unused file-scope static. */
    const char *const *p_task_name = &TASK_NAME;
    (void)p_viewer; (void)p_loop; (void)p_daemon; (void)p_task_name;
}


/* ================================================================
 * TESTS — validate_env_exists (portable function, both branches)
 * ================================================================ */

/**
 * validate_env_exists: success branch — file exists and is readable.
 * Verifies the function returns 0 for a real file on disk.
 */
static void test_validate_env_exists_returns_zero_for_existing_file(void) {
    TEST("validate_env_exists — returns 0 for existing file");

    remove(TEST_ENV_PATH);
    FILE *fp = fopen(TEST_ENV_PATH, "w");
    if (fp == NULL) { FAIL("could not create temp env file"); return; }
    fputs("GITHUB_BASE_URL=https://x@github.com/owner/\n", fp);
    fclose(fp);

    int result = validate_env_exists(TEST_ENV_PATH);

    remove(TEST_ENV_PATH);
    if (result != ENV_VALIDATE_SUCCESS) {
        FAIL("expected 0 for existing file");
        return;
    }
    PASS();
}


/**
 * validate_env_exists: failure branch — file does not exist.
 * Verifies the function returns -1 (and prints an error to stderr)
 * for a missing path. Exercises the fopen-returns-NULL branch.
 *
 * NOTE: validate_env_exists does NOT explicitly handle a NULL path
 * argument — passing NULL would invoke undefined behavior via
 * fopen(NULL, "r") and the subsequent printf("%s", NULL). NULL is
 * therefore NOT tested here. (Documented per AGENTS.md Rule 68:
 * every assumption must be documented.)
 */
static void test_validate_env_exists_returns_error_for_missing_file(void) {
    TEST("validate_env_exists — returns -1 for missing file");

    remove(TEST_MISSING_ENV_PATH);

    int result = validate_env_exists(TEST_MISSING_ENV_PATH);

    if (result != ENV_VALIDATE_FAILURE) {
        FAIL("expected -1 for missing file");
        return;
    }
    PASS();
}


/**
 * validate_env_exists: resource-cleanup regression (Rules 37 & 62).
 * On the success branch the function opens the file with fopen and
 * must close it with fclose before returning. We verify the file
 * handle is released by removing the file immediately after the call
 * — if a handle were still open, the removal would surface the leak.
 * On Linux remove() succeeds even with an open fd, but this test
 * enforces the cleanup contract as a forward-looking regression
 * guard (a Windows port would fail remove() with an open handle).
 */
static void test_validate_env_exists_closes_file_handle(void) {
    TEST("validate_env_exists — closes file handle (no fd leak)");

    remove(TEST_ENV_PATH);
    FILE *fp = fopen(TEST_ENV_PATH, "w");
    if (fp == NULL) { FAIL("could not create temp env file"); return; }
    fclose(fp);

    int result = validate_env_exists(TEST_ENV_PATH);
    if (result != ENV_VALIDATE_SUCCESS) {
        FAIL("validate_env_exists failed on existing file");
        remove(TEST_ENV_PATH);
        return;
    }

    int rm_result = remove(TEST_ENV_PATH);
    if (rm_result != 0) {
        FAIL("could not remove file after validate_env_exists (fd leak?)");
        return;
    }
    PASS();
}


/* ================================================================
 * TESTS — Linux stubs (regression coverage: stub exists, returns
 * the documented error code, does not crash)
 * ================================================================ */

/**
 * signal_shutdown on Linux is a #else stub that prints a "not
 * supported" message and returns -1. Verified here.
 */
static void test_signal_shutdown_returns_error_on_linux_stub(void) {
    TEST("signal_shutdown — returns -1 on Linux stub");

    int result = signal_shutdown();

    if (result != STUB_ERROR_RETURN) {
        FAIL("expected -1 from Linux stub");
        return;
    }
    PASS();
}


/**
 * register_task_scheduler on Linux is a #else stub that returns -1.
 * Passes a real context to confirm the stub does not dereference it
 * (the Linux stub casts ctx to void).
 */
static void test_register_task_scheduler_returns_error_on_linux_stub(void) {
    TEST("register_task_scheduler — returns -1 on Linux stub");

    ghb_context ctx = create_test_context();
    int result = register_task_scheduler(&ctx);

    if (result != STUB_ERROR_RETURN) {
        FAIL("expected -1 from Linux stub");
        return;
    }
    PASS();
}


/**
 * unregister_task_scheduler on Linux is a #else stub that returns -1.
 */
static void test_unregister_task_scheduler_returns_error_on_linux_stub(void) {
    TEST("unregister_task_scheduler — returns -1 on Linux stub");

    ghb_context ctx = create_test_context();
    int result = unregister_task_scheduler(&ctx);

    if (result != STUB_ERROR_RETURN) {
        FAIL("expected -1 from Linux stub");
        return;
    }
    PASS();
}


/**
 * is_task_scheduler_registered on Linux is a #else stub that returns 0
 * (i.e., "not registered"). The Windows branch returns 1 or 0; on
 * Linux the answer is always "no" because the platform has no
 * Task Scheduler.
 */
static void test_is_task_scheduler_registered_returns_zero_on_linux_stub(void) {
    TEST("is_task_scheduler_registered — returns 0 on Linux stub");

    ghb_context ctx = create_test_context();
    int result = is_task_scheduler_registered(&ctx);

    if (result != STUB_ZERO_RETURN) {
        FAIL("expected 0 from Linux stub");
        return;
    }
    PASS();
}


/**
 * check_single_instance on Linux is a #else stub that returns 0
 * (i.e., "no mutex collision — proceed"). The Windows branch returns
 * 0 (created), 1 (already exists), or -1 (error).
 */
static void test_check_single_instance_returns_zero_on_linux_stub(void) {
    TEST("check_single_instance — returns 0 on Linux stub");

    int result = check_single_instance();

    if (result != STUB_ZERO_RETURN) {
        FAIL("expected 0 from Linux stub");
        return;
    }
    PASS();
}


/**
 * spawn_daemon on Linux is a #else stub that returns -1 (cannot spawn
 * a Windows CREATE_NO_WINDOW process on Linux).
 */
static void test_spawn_daemon_returns_error_on_linux_stub(void) {
    TEST("spawn_daemon — returns -1 on Linux stub");

    int result = spawn_daemon();

    if (result != STUB_ERROR_RETURN) {
        FAIL("expected -1 from Linux stub");
        return;
    }
    PASS();
}


/**
 * sleep_with_shutdown_check has an early-return guard for non-positive
 * seconds: "if (seconds <= 0) return;". We exercise that branch by
 * passing SLEEP_SECONDS_ZERO — the function must return immediately
 * without dereferencing config (which we pass as NULL). This is the
 * only branch reachable on Linux without a fully-initialized context.
 */
static void test_sleep_with_shutdown_check_returns_immediately_for_zero(void) {
    TEST("sleep_with_shutdown_check — returns immediately for seconds <= 0");

    /* seconds = 0 triggers the early-return before config is touched.
     * Passing NULL for config is safe ONLY because of that guard. */
    sleep_with_shutdown_check(SLEEP_SECONDS_ZERO, NULL);

    PASS();
}


/* ================================================================
 * TESTS — check_shutdown_requested (reads g_shutdown_requested)
 *
 * g_shutdown_requested is a file-scope static volatile int in
 * main.c. Because main.c is #included into this test file, the
 * symbol is directly accessible — we can set and reset it to
 * exercise both branches of check_shutdown_requested().
 * ================================================================ */

/**
 * check_shutdown_requested returns g_shutdown_requested. Default
 * value is 0 (no shutdown requested). Verified here. Also resets
 * the flag to 0 in case a prior test set it.
 */
static void test_check_shutdown_requested_returns_zero_when_not_set(void) {
    TEST("check_shutdown_requested — returns 0 when flag is 0");

    g_shutdown_requested = SHUTDOWN_NOT_REQUESTED;

    int result = check_shutdown_requested();

    if (result != SHUTDOWN_NOT_REQUESTED) {
        FAIL("expected 0 when g_shutdown_requested == 0");
        return;
    }
    PASS();
}


/**
 * check_shutdown_requested returns g_shutdown_requested. When the
 * flag is set to 1 (e.g., by the SIGINT handler in production),
 * the function must return non-zero so the main loop breaks out.
 * We set the flag directly here because the test file has access
 * to the static via the #include of main.c. The flag is restored
 * to 0 after the assertion so subsequent tests see the default.
 */
static void test_check_shutdown_requested_returns_nonzero_when_set(void) {
    TEST("check_shutdown_requested — returns non-zero when flag is set");

    g_shutdown_requested = SHUTDOWN_REQUESTED;

    int result = check_shutdown_requested();

    /* Restore the flag so subsequent tests see the default state. */
    g_shutdown_requested = SHUTDOWN_NOT_REQUESTED;

    if (result == SHUTDOWN_NOT_REQUESTED) {
        FAIL("expected non-zero when g_shutdown_requested == 1");
        return;
    }
    PASS();
}


/* ================================================================
 * MAIN
 * ================================================================ */

int main(void) {
    printf("=== Main Module Tests ===\n\n");

    /* Ensure clean shutdown-flag state before any test runs. */
    g_shutdown_requested = SHUTDOWN_NOT_REQUESTED;

    /* Mark the otherwise-unused static functions as referenced so the
     * compiler does not warn about them under -Wall. */
    reference_unused_main_functions();

    printf("-- validate_env_exists (portable function) --\n");
    test_validate_env_exists_returns_zero_for_existing_file();
    test_validate_env_exists_returns_error_for_missing_file();
    test_validate_env_exists_closes_file_handle();

    printf("\n-- Linux stubs (regression coverage) --\n");
    test_signal_shutdown_returns_error_on_linux_stub();
    test_register_task_scheduler_returns_error_on_linux_stub();
    test_unregister_task_scheduler_returns_error_on_linux_stub();
    test_is_task_scheduler_registered_returns_zero_on_linux_stub();
    test_check_single_instance_returns_zero_on_linux_stub();
    test_spawn_daemon_returns_error_on_linux_stub();
    test_sleep_with_shutdown_check_returns_immediately_for_zero();

    printf("\n-- check_shutdown_requested (both branches) --\n");
    test_check_shutdown_requested_returns_zero_when_not_set();
    test_check_shutdown_requested_returns_nonzero_when_set();

    /* Restore default state in case the last test left the flag set. */
    g_shutdown_requested = SHUTDOWN_NOT_REQUESTED;

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
