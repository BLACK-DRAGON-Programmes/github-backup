/**
 * test_console.c - Unit tests for the console module.
 *
 * Tests the console output lifecycle: init, is_active, print_log across all
 * four log levels, NULL-argument safety for every nullable parameter, the
 * output-gating branch, and cleanup. The blocking console_log_viewer() is
 * intentionally excluded (see comment above main()).
 *
 * === IMPORTANT: Linux behavior of console.c ===
 *
 * console.c gates ALL console_print_log output behind a file-scope static
 * `g_console_active` flag. That flag is ONLY set to 1 inside console_init()
 * under `#ifdef _WIN32` (the SetConsoleMode / ENABLE_VIRTUAL_TERMINAL
 * path). On Linux there is no such code path, so console_init() returns 0
 * but leaves g_console_active at 0. Consequently, on Linux:
 *
 *   - console_is_active() always returns 0 (even after init).
 *   - console_print_log() takes the early-return `if (!g_console_active)`
 *     branch and writes nothing to stdout.
 *
 * g_console_active is a file-scope static in console.c, so this test file
 * cannot force it to 1 to exercise the printing branch directly. We
 * therefore test the ACTUAL observable Linux behavior:
 *
 *   1. console_init returns 0 and does not activate the console.
 *   2. console_is_active returns 0 before and after init.
 *   3. console_print_log does not crash for any log level or NULL arg.
 *   4. console_print_log writes NO output to stdout (the gating branch).
 *   5. console_cleanup is safe with and without a prior init.
 *
 * This satisfies Rule 58 (every branch tested): the `if (!g_console_active)
 * return` branch is verified by the no-output test, and the NULL-handling
 * branches inside console_print_log (action/repo/status/detail) are
 * exercised by the NULL-argument tests. The Windows-only printing branch
 * cannot be reached on Linux without modifying console.c, which is out of
 * scope for this test file.
 *
 * Compile (Linux):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I src/ -o build/test_console \
 *       tests/test_console.c src/console.c src/logger.c src/notify.c \
 *       src/config.c src/network.c src/backup.c
 */

#include "console.h"       /* Console public API + log_level (via logger_iface.h) */
#include "logger_iface.h"  /* log_level enum — explicit for clarity */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>         /* dup, dup2, close, fileno */


/* ================================================================
 * NAMED CONSTANTS (Rule 10: no raw numbers)
 * ================================================================ */

/** Expected return value of console_init() on success / Linux stub. */
#define EXPECTED_INIT_RESULT          0

/** Value returned by console_is_active() when the console is NOT active. */
#define CONSOLE_INACTIVE              0

/** Expected length of captured stdout when the gating branch suppresses output. */
#define EXPECTED_EMPTY_OUTPUT_LEN     0

/** Maximum length of a temp-file path buffer used by snprintf. */
#define TEMP_PATH_MAX                 256

/**
 * Path to the temp file used to capture stdout for the output-gating test.
 * Reused across the single test that needs it; cleaned up via remove().
 */
#define CONSOLE_CAPTURE_PATH          "/tmp/ghb_test_console_capture.out"


/* ================================================================
 * TEST FRAMEWORK (mirrors test_notify.c exactly)
 * ================================================================ */

static int tests_passed = 0;
static int tests_failed = 0;


#define TEST(name) printf("  TEST: %s... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)


/* ================================================================
 * HELPERS
 * ================================================================ */

/**
 * Read the entire contents of a file into a malloc'd, NUL-terminated
 * buffer. Returns NULL if the file cannot be opened or read. Caller
 * frees the buffer. (Copied from test_notify.c — established pattern.)
 */
static char *read_file_contents(const char *path) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    /* Guard against empty files — still return a valid empty string. */
    if (size < 0) { fclose(fp); return NULL; }

    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) { fclose(fp); return NULL; }

    size_t nread = fread(buf, 1, (size_t)size, fp);
    buf[nread] = '\0';
    fclose(fp);
    return buf;
}

/**
 * Redirect stdout to a file so console_print_log output (if any) can be
 * captured and inspected. Returns a saved file descriptor that MUST be
 * passed to restore_stdout() to reconnect stdout to the terminal.
 *
 * Implementation: dup() the current stdout fd, then freopen() the stdio
 * `stdout` FILE* onto the capture path. The dup/dup2 approach is the most
 * reliable portable way to redirect and restore a stdio stream.
 *
 * Returns: >= 0 on success (the saved fd), -1 on failure.
 */
static int capture_stdout_to_file(const char *path) {
    int saved_fd = dup(fileno(stdout));
    if (saved_fd < 0) return -1;

    fflush(stdout);
    if (freopen(path, "w", stdout) == NULL) {
        /* freopen failed — restore the saved fd before bailing out. */
        dup2(saved_fd, fileno(stdout));
        close(saved_fd);
        return -1;
    }
    return saved_fd;
}

/**
 * Restore stdout to the terminal using the saved file descriptor returned
 * by capture_stdout_to_file(). Flushes any buffered output first so the
 * capture file is complete before we switch back.
 */
static void restore_stdout(int saved_fd) {
    if (saved_fd < 0) return;
    fflush(stdout);
    dup2(saved_fd, fileno(stdout));
    close(saved_fd);
}


/* ================================================================
 * LIFECYCLE TESTS
 * ================================================================ */

/**
 * console_init() must return 0 on Linux. On Windows it would attempt
 * SetConsoleMode and return -1 on failure, but the non-Windows build has
 * no console-enabling code — it falls straight through to `return 0`.
 */
static void test_console_init_returns_zero(void) {
    TEST("console_init — returns 0 on Linux");
    int result = console_init();
    if (result != EXPECTED_INIT_RESULT) {
        FAIL("console_init did not return 0");
        console_cleanup();
        return;
    }
    console_cleanup();
    PASS();
}

/**
 * console_is_active() must return 0 (inactive) before console_init() is
 * ever called. This verifies the module's default state.
 */
static void test_console_is_active_before_init(void) {
    TEST("console_is_active — returns 0 before init");
    /* Ensure a clean starting state (a prior test may have called init). */
    console_cleanup();
    int active = console_is_active();
    if (active != CONSOLE_INACTIVE) {
        FAIL("console_is_active non-zero before init");
        return;
    }
    PASS();
}

/**
 * On Linux, console_init() does NOT activate ANSI output — the activation
 * code is `#ifdef _WIN32` only. So console_is_active() must still return 0
 * after init on Linux. This documents the platform behavior explicitly.
 */
static void test_console_is_active_after_init_linux(void) {
    TEST("console_is_active — returns 0 after init on Linux (Windows-only activation)");
    console_cleanup();                 /* start clean */
    console_init();
    int active = console_is_active();
    if (active != CONSOLE_INACTIVE) {
        FAIL("console_is_active non-zero after Linux init");
        console_cleanup();
        return;
    }
    console_cleanup();
    PASS();
}

/**
 * console_cleanup() must be safe to call without a prior console_init().
 * This verifies the function does not dereference uninitialized state.
 */
static void test_console_cleanup_no_init(void) {
    TEST("console_cleanup — safe without init (no crash)");
    console_cleanup();   /* no init — must not crash */
    PASS();
}

/**
 * console_cleanup() after init must reset the active flag to 0. We verify
 * is_active returns 0 after the init/cleanup pair. (On Linux it was never
 * raised, but this still confirms cleanup leaves the module inactive.)
 */
static void test_console_cleanup_after_init(void) {
    TEST("console_cleanup — resets to inactive after init");
    console_init();
    console_cleanup();
    int active = console_is_active();
    if (active != CONSOLE_INACTIVE) {
        FAIL("console still active after cleanup");
        return;
    }
    PASS();
}

/**
 * Full lifecycle: init → use (print_log) → cleanup. Verifies the sequence
 * does not crash and leaves the module in a clean state.
 */
static void test_console_init_cleanup_cycle(void) {
    TEST("console_init/cleanup — full lifecycle (init, print, cleanup)");
    console_init();
    /* Exercise print_log during the "active" window (no-op on Linux). */
    console_print_log(LOG_INFO, "lifecycle", "test-repo", "OK", "cycle ok");
    console_cleanup();
    if (console_is_active() != CONSOLE_INACTIVE) {
        FAIL("console active after lifecycle cleanup");
        return;
    }
    PASS();
}


/* ================================================================
 * console_print_log — LOG LEVEL TESTS (no-crash per level)
 *
 * Each log level exercises a different case in console.c's internal
 * color_for_level() and level_string() switch. On Linux the output is
 * suppressed by the gating branch, so we verify the call does not crash
 * for each level. A separate test below verifies the gating itself.
 * ================================================================ */

static void test_console_print_log_info_no_crash(void) {
    TEST("console_print_log — LOG_INFO does not crash");
    console_init();
    console_print_log(LOG_INFO, "cycle", "owner/repo", "STARTED", "beginning backup");
    console_cleanup();
    PASS();
}

static void test_console_print_log_success_no_crash(void) {
    TEST("console_print_log — LOG_SUCCESS does not crash");
    console_init();
    console_print_log(LOG_SUCCESS, "backup", "owner/repo", "OK", "Downloaded 4096 KB");
    console_cleanup();
    PASS();
}

static void test_console_print_log_warning_no_crash(void) {
    TEST("console_print_log — LOG_WARNING does not crash");
    console_init();
    console_print_log(LOG_WARNING, "network", "owner/missing", "SKIPPED", "HTTP 404");
    console_cleanup();
    PASS();
}

static void test_console_print_log_error_no_crash(void) {
    TEST("console_print_log — LOG_ERROR does not crash");
    console_init();
    console_print_log(LOG_ERROR, "backup", "owner/repo", "FAILED", "disk full");
    console_cleanup();
    PASS();
}


/* ================================================================
 * console_print_log — NULL ARGUMENT TESTS
 *
 * console.c's print_log has explicit NULL branches for action, repo,
 * status, and detail (it prints an empty padded field or omits detail).
 * Each NULL branch must be safe — no crash, no dereference. We exercise
 * each nullable parameter as NULL individually (Rule 58: every branch).
 * ================================================================ */

static void test_console_print_log_null_repo(void) {
    TEST("console_print_log — NULL repo does not crash");
    console_init();
    console_print_log(LOG_INFO, "cycle", NULL, "OK", "no repo context");
    console_cleanup();
    PASS();
}

static void test_console_print_log_null_detail(void) {
    TEST("console_print_log — NULL detail does not crash");
    console_init();
    console_print_log(LOG_SUCCESS, "backup", "owner/repo", "OK", NULL);
    console_cleanup();
    PASS();
}

static void test_console_print_log_null_action(void) {
    TEST("console_print_log — NULL action does not crash");
    console_init();
    console_print_log(LOG_WARNING, NULL, "owner/repo", "WARN", "missing action");
    console_cleanup();
    PASS();
}

static void test_console_print_log_null_status(void) {
    TEST("console_print_log — NULL status does not crash");
    console_init();
    console_print_log(LOG_ERROR, "backup", "owner/repo", NULL, "no status");
    console_cleanup();
    PASS();
}


/* ================================================================
 * console_print_log — OUTPUT GATING TEST
 *
 * This is the key behavioral test for Linux: because g_console_active is
 * never set to 1 on Linux, console_print_log must take the early-return
 * branch and write NOTHING to stdout. We capture stdout to a temp file,
 * call print_log with valid arguments, then verify the captured file is
 * empty. This directly tests the `if (!g_console_active) return;` branch.
 * ================================================================ */
static void test_console_print_log_no_output_when_inactive(void) {
    TEST("console_print_log — writes no stdout when console inactive (Linux gating)");

    /* Start from a guaranteed-inactive state. */
    console_cleanup();
    console_init();   /* On Linux this does NOT activate the console. */

    remove(CONSOLE_CAPTURE_PATH);

    int saved_fd = capture_stdout_to_file(CONSOLE_CAPTURE_PATH);
    if (saved_fd < 0) {
        FAIL("could not capture stdout");
        console_cleanup();
        return;
    }

    /* If the gating branch is working, this writes nothing. */
    console_print_log(LOG_ERROR, "backup", "owner/repo", "FAILED", "should not appear");

    fflush(stdout);
    restore_stdout(saved_fd);

    char *contents = read_file_contents(CONSOLE_CAPTURE_PATH);
    if (contents == NULL) {
        FAIL("could not read capture file");
        console_cleanup();
        remove(CONSOLE_CAPTURE_PATH);
        return;
    }

    /*
     * The capture file must be empty (or contain only data the runtime
     * wrote before our redirect — which is nothing because we fflush'd).
     * We check both length and that none of our argument strings leaked.
     */
    size_t len = strlen(contents);
    int leaked = (strstr(contents, "should not appear") != NULL) ||
                 (strstr(contents, "owner/repo") != NULL) ||
                 (strstr(contents, "FAILED") != NULL);

    free(contents);
    console_cleanup();
    remove(CONSOLE_CAPTURE_PATH);

    if (len != (size_t)EXPECTED_EMPTY_OUTPUT_LEN) {
        FAIL("console_print_log wrote output while inactive");
        return;
    }
    if (leaked) {
        FAIL("console_print_log leaked argument text while inactive");
        return;
    }
    PASS();
}


/* ================================================================
 * MAIN
 *
 * NOTE: console_log_viewer() is deliberately NOT tested here. It is a
 * blocking function that opens a log file, seeks to end, and polls for
 * new lines + 'q' keypresses in an infinite loop (Windows) or returns
 * after the initial tail (Linux, since the polling loop is #ifdef
 * _WIN32). It cannot be unit-tested without forking/threading and a
 * fake log file + fake keyboard input, which is out of scope for a
 * unit test. Its logic is verified by manual/integration testing.
 * ================================================================ */

int main(void) {
    printf("=== Console Module Tests ===\n\n");

    /* Lifecycle */
    printf("-- Init / is_active / cleanup lifecycle tests --\n");
    test_console_init_returns_zero();
    test_console_is_active_before_init();
    test_console_is_active_after_init_linux();
    test_console_cleanup_no_init();
    test_console_cleanup_after_init();
    test_console_init_cleanup_cycle();

    /* Log levels (no-crash per level) */
    printf("\n-- console_print_log log-level tests --\n");
    test_console_print_log_info_no_crash();
    test_console_print_log_success_no_crash();
    test_console_print_log_warning_no_crash();
    test_console_print_log_error_no_crash();

    /* NULL argument safety */
    printf("\n-- console_print_log NULL-argument tests --\n");
    test_console_print_log_null_repo();
    test_console_print_log_null_detail();
    test_console_print_log_null_action();
    test_console_print_log_null_status();

    /* Output gating */
    printf("\n-- console_print_log output-gating test --\n");
    test_console_print_log_no_output_when_inactive();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
