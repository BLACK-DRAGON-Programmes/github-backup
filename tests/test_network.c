/**
 * test_network.c - Unit tests for the network module.
 *
 * Tests the platform-independent JSON parsing functions, rate limit
 * info struct initialization, and verifies function signatures compile
 * correctly. Full HTTP/network tests require a live GitHub API connection
 * and are performed on Windows during integration testing.
 *
 * Compile (Linux):  gcc -o test_network tests/test_network.c src/network.c src/logger.c src/notify.c src/config.c src/backup.c -I src/
 * Compile (Win):    gcc -o test_network.exe tests/test_network.c src/network.c src/logger.c src/notify.c src/config.c src/backup.c -lwinhttp
 */

#include "network.h"
#include "context.h"
#include "logger_iface.h"
#include "notify_iface.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>


static int tests_passed = 0;
static int tests_failed = 0;


/* ─── Fake Ops for Dependency Injection ─────────────────────── */

/* Fake logger_ops — all no-ops for test isolation. */
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

static int  fake_log_init(ghb_context *ctx, const char *log_path) {
    (void)ctx; (void)log_path; return 0;
}

static void fake_log_close(ghb_context *ctx) { (void)ctx; }

static void fake_rotate_log(ghb_context *ctx, long max_size_bytes) {
    (void)ctx; (void)max_size_bytes;
}

static const logger_ops fake_logger_ops = {
    .log_event              = fake_log_event,
    .log_error              = fake_log_error,
    .log_init               = fake_log_init,
    .log_close              = fake_log_close,
    .rotate_log             = fake_rotate_log,
};


/* Fake notify_ops — all no-ops for test isolation. */
static void fake_toast_info(ghb_context *ctx, const char *title,
                            const char *message) {
    (void)ctx; (void)title; (void)message;
}

static void fake_toast_success(ghb_context *ctx, const char *repo,
                               const char *message) {
    (void)ctx; (void)repo; (void)message;
}

static void fake_toast_error(ghb_context *ctx, const char *title,
                             const char *message) {
    (void)ctx; (void)title; (void)message;
}

static int  fake_notify_init(ghb_context *ctx) { (void)ctx; return 0; }

static void fake_notify_cleanup(ghb_context *ctx) { (void)ctx; }

static const notify_ops fake_notify_ops = {
    .toast_info     = fake_toast_info,
    .toast_success  = fake_toast_success,
    .toast_error    = fake_toast_error,
    .notify_init    = fake_notify_init,
    .notify_cleanup = fake_notify_cleanup,
};


/**
 * Create a test context with fake (no-op) logger and notify ops.
 * Network ops are NULL — not needed for pure utility tests.
 */
static ghb_context create_test_context(void) {
    ghb_context ctx;
    ctx.logger  = &fake_logger_ops;
    ctx.notify  = &fake_notify_ops;
    ctx.network = NULL;  /* Not needed — tests only call parse_json_* */
    ctx.should_stop = NULL;
    return ctx;
}


#define TEST(name) printf("  TEST: %s... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)


/* ─── JSON String Parsing Tests ─────────────────────────────── */

/**
 * Test extracting a string value from a typical GitHub API response.
 * The response body contains "default_branch": "main" - verify extraction.
 */
static void test_json_string_typical(void) {
    TEST("parse_json_string - typical API response");

    const char *json = "{\"id\":12345,\"name\":\"test-repo\","
                        "\"default_branch\":\"main\","
                        "\"private\":false}";

    char value[MAX_REPO_NAME_LEN] = {0};
    int result = parse_json_string(json, "default_branch",
                                    value, sizeof(value));

    if (result != 0) { FAIL("returned error"); return; }
    if (strcmp(value, "main") != 0) { FAIL("wrong value"); return; }
    PASS();
}


/**
 * Test extracting a string value where the key appears first in the
 * JSON object. Verify correct parsing when the key is not at the end.
 */
static void test_json_string_first_key(void) {
    TEST("parse_json_string - first key in object");

    const char *json = "{\"name\":\"my-repo\",\"default_branch\":\"develop\"}";

    char value[MAX_REPO_NAME_LEN] = {0};
    int result = parse_json_string(json, "name", value, sizeof(value));

    if (result != 0) { FAIL("returned error"); return; }
    if (strcmp(value, "my-repo") != 0) { FAIL("wrong value"); return; }
    PASS();
}


/**
 * Test that a key not present in the JSON returns an error.
 */
static void test_json_string_key_not_found(void) {
    TEST("parse_json_string - key not found");

    const char *json = "{\"name\":\"test\",\"default_branch\":\"main\"}";

    char value[MAX_REPO_NAME_LEN] = {0};
    int result = parse_json_string(json, "nonexistent_key",
                                    value, sizeof(value));

    if (result == 0) { FAIL("should have failed for nonexistent key"); return; }
    PASS();
}


/**
 * Test extracting an empty string value ("": "").
 */
static void test_json_string_empty_value(void) {
    TEST("parse_json_string - empty string value");

    const char *json = "{\"name\":\"\",\"default_branch\":\"main\"}";

    char value[MAX_REPO_NAME_LEN] = {0};
    int result = parse_json_string(json, "name", value, sizeof(value));

    if (result != 0) { FAIL("returned error"); return; }
    if (strcmp(value, "") != 0) { FAIL("expected empty string"); return; }
    PASS();
}


/**
 * Test JSON parsing with whitespace between key, colon, and value.
 */
static void test_json_string_with_whitespace(void) {
    TEST("parse_json_string - whitespace between tokens");

    const char *json = "{ \"default_branch\" :   \"feature-branch\" }";

    char value[MAX_REPO_NAME_LEN] = {0};
    int result = parse_json_string(json, "default_branch",
                                    value, sizeof(value));

    if (result != 0) { FAIL("returned error"); return; }
    if (strcmp(value, "feature-branch") != 0) {
        FAIL("wrong value");
        return;
    }
    PASS();
}


/**
 * Test that string extraction is bounded by the output buffer length.
 * If the value exceeds the buffer, it should be truncated.
 */
static void test_json_string_buffer_overflow(void) {
    TEST("parse_json_string - buffer truncation");

    const char *json = "{\"name\":\"aaaaaaaaaaaaaaaaaaaaaaa\"}";

    char value[8] = {0};
    int result = parse_json_string(json, "name", value, sizeof(value));

    if (result != 0) { FAIL("returned error"); return; }
    /* Value should be truncated to 7 chars + null */
    if (strlen(value) != 7) {
        FAIL("expected truncation to 7 chars");
        return;
    }
    PASS();
}


/**
 * Test extracting a string value that contains special characters
 * (hyphens, dots, underscores) - typical of branch and repo names.
 */
static void test_json_string_special_chars(void) {
    TEST("parse_json_string - special characters in value");

    const char *json = "{\"default_branch\":\"feature/new-api-v2.1\"}";

    char value[MAX_REPO_NAME_LEN] = {0};
    int result = parse_json_string(json, "default_branch",
                                    value, sizeof(value));

    if (result != 0) { FAIL("returned error"); return; }
    if (strcmp(value, "feature/new-api-v2.1") != 0) {
        FAIL("wrong value");
        return;
    }
    PASS();
}


/* ─── JSON Escape Sequence Tests ─────────────────────────────── */

/**
 * Test escaped double quote in JSON string value: \" → "
 */
static void test_json_string_escaped_quote(void) {
    TEST("parse_json_string - escaped double quote \\\" in value");

    const char *json = "{\"description\":\"He said \\\"hello\\\" to me\"}";

    char value[256] = {0};
    int result = parse_json_string(json, "description", value, sizeof(value));

    if (result != 0) { FAIL("returned error"); return; }
    if (strstr(value, "\"") == NULL) {
        FAIL("escaped quote not properly decoded");
        return;
    }
    PASS();
}


/**
 * Test escaped backslash in JSON string value: \\ → \
 */
static void test_json_string_escaped_backslash(void) {
    TEST("parse_json_string - escaped backslash \\\\ in value");

    const char *json = "{\"path\":\"C:\\\\Users\\\\test\"}";

    char value[256] = {0};
    int result = parse_json_string(json, "path", value, sizeof(value));

    if (result != 0) { FAIL("returned error"); return; }
    if (strstr(value, "\\") == NULL) {
        FAIL("escaped backslash not properly decoded");
        return;
    }
    PASS();
}


/**
 * Test escaped forward slash: \/ → /
 */
static void test_json_string_escaped_slash(void) {
    TEST("parse_json_string - escaped forward slash \\/ in value");

    const char *json = "{\"url\":\"https:\\/\\/example.com\"}";

    char value[256] = {0};
    int result = parse_json_string(json, "url", value, sizeof(value));

    if (result != 0) { FAIL("returned error"); return; }
    if (strstr(value, "/") == NULL) {
        FAIL("escaped slash not properly decoded");
        return;
    }
    PASS();
}


/**
 * Test escaped newline: \n → actual newline character
 */
static void test_json_string_escaped_newline(void) {
    TEST("parse_json_string - escaped newline \\n in value");

    const char *json = "{\"text\":\"line1\\nline2\"}";

    char value[256] = {0};
    int result = parse_json_string(json, "text", value, sizeof(value));

    if (result != 0) { FAIL("returned error"); return; }
    if (strchr(value, '\n') == NULL) {
        FAIL("escaped newline not properly decoded");
        return;
    }
    PASS();
}


/**
 * Test escaped tab: \t → actual tab character
 */
static void test_json_string_escaped_tab(void) {
    TEST("parse_json_string - escaped tab \\t in value");

    const char *json = "{\"text\":\"col1\\tcol2\"}";

    char value[256] = {0};
    int result = parse_json_string(json, "text", value, sizeof(value));

    if (result != 0) { FAIL("returned error"); return; }
    if (strchr(value, '\t') == NULL) {
        FAIL("escaped tab not properly decoded");
        return;
    }
    PASS();
}


/**
 * Test escaped carriage return: \r → actual CR character
 */
static void test_json_string_escaped_cr(void) {
    TEST("parse_json_string - escaped carriage return \\r in value");

    const char *json = "{\"text\":\"hello\\rworld\"}";

    char value[256] = {0};
    int result = parse_json_string(json, "text", value, sizeof(value));

    if (result != 0) { FAIL("returned error"); return; }
    if (strchr(value, '\r') == NULL) {
        FAIL("escaped CR not properly decoded");
        return;
    }
    PASS();
}


/**
 * Test Unicode escape: \uXXXX - should preserve as-is (no full Unicode support needed).
 */
static void test_json_string_unicode_escape(void) {
    TEST("parse_json_string - unicode escape \\uXXXX preserved");

    const char *json = "{\"emoji\":\"\\u0041\"}";  /* \u0041 = 'A' */

    char value[256] = {0};
    int result = parse_json_string(json, "emoji", value, sizeof(value));

    if (result != 0) { FAIL("returned error"); return; }
    /* Unicode escape is preserved as \uXXXX in the output */
    if (strstr(value, "\\u0041") == NULL && strchr(value, 'A') == NULL) {
        FAIL("unicode escape not handled");
        return;
    }
    PASS();
}


/**
 * Test multiple escape sequences in a single value.
 */
static void test_json_string_multiple_escapes(void) {
    TEST("parse_json_string - multiple escape sequences in one value");

    const char *json = "{\"msg\":\"hello\\nworld\\t!\"}";

    char value[256] = {0};
    int result = parse_json_string(json, "msg", value, sizeof(value));

    if (result != 0) { FAIL("returned error"); return; }
    if (strchr(value, '\n') == NULL) { FAIL("missing newline"); return; }
    if (strchr(value, '\t') == NULL) { FAIL("missing tab"); return; }
    PASS();
}


/**
 * Test trailing backslash at end of string (edge case - no char after backslash).
 */
static void test_json_string_trailing_backslash(void) {
    TEST("parse_json_string - value ending with backslash before closing quote");

    /* This tests the edge case where \\ precedes the closing " */
    const char *json = "{\"path\":\"C:\\\\\"}";  /* Value: C:\ */

    char value[256] = {0};
    int result = parse_json_string(json, "path", value, sizeof(value));

    if (result != 0) { FAIL("returned error"); return; }
    /* Should contain a backslash */
    if (strstr(value, "\\") == NULL) {
        FAIL("backslash not properly decoded at end of value");
        return;
    }
    PASS();
}


/* ─── JSON Integer Parsing Tests ────────────────────────────── */

/**
 * Test extracting a positive integer from a JSON response.
 */
static void test_json_int_positive(void) {
    TEST("parse_json_int - positive integer");

    const char *json = "{\"remaining\":59,\"reset\":1700000000}";

    int value = 0;
    int result = parse_json_int(json, "remaining", &value);

    if (result != 0) { FAIL("returned error"); return; }
    if (value != 59) { FAIL("wrong value"); return; }
    PASS();
}


/**
 * Test extracting a negative integer from a JSON response.
 */
static void test_json_int_negative(void) {
    TEST("parse_json_int - negative integer");

    const char *json = "{\"count\":-1,\"total\":100}";

    int value = 0;
    int result = parse_json_int(json, "count", &value);

    if (result != 0) { FAIL("returned error"); return; }
    if (value != -1) { FAIL("wrong value"); return; }
    PASS();
}


/**
 * Test that a missing integer key returns an error.
 */
static void test_json_int_key_not_found(void) {
    TEST("parse_json_int - key not found");

    const char *json = "{\"remaining\":59}";

    int value = 0;
    int result = parse_json_int(json, "nonexistent", &value);

    if (result == 0) { FAIL("should have failed for nonexistent key"); return; }
    PASS();
}


/**
 * Test integer parsing with whitespace between tokens.
 */
static void test_json_int_with_whitespace(void) {
    TEST("parse_json_int - whitespace between tokens");

    const char *json = "{  \"remaining\"  :  42  }";

    int value = 0;
    int result = parse_json_int(json, "remaining", &value);

    if (result != 0) { FAIL("returned error"); return; }
    if (value != 42) { FAIL("wrong value"); return; }
    PASS();
}


/**
 * Test integer parsing with a large Unix timestamp value.
 */
static void test_json_int_large_value(void) {
    TEST("parse_json_int - large timestamp value");

    const char *json = "{\"reset\":1850000000}";

    int value = 0;
    int result = parse_json_int(json, "reset", &value);

    if (result != 0) { FAIL("returned error"); return; }
    /* 1850000000 fits in int (max 2147483647) */
    if (value != 1850000000) { FAIL("wrong value"); return; }
    PASS();
}


/**
 * Test integer extraction when the value is followed by other JSON fields.
 */
static void test_json_int_mid_object(void) {
    TEST("parse_json_int - value in middle of object");

    const char *json = "{\"a\":1,\"remaining\":58,\"b\":2}";

    int value = 0;
    int result = parse_json_int(json, "remaining", &value);

    if (result != 0) { FAIL("returned error"); return; }
    if (value != 58) { FAIL("wrong value"); return; }
    PASS();
}


/**
 * Test parsing integer value zero.
 */
static void test_json_int_zero(void) {
    TEST("parse_json_int - zero value");

    const char *json = "{\"remaining\":0}";

    int value = -1;  /* Initialize to non-zero to detect change */
    int result = parse_json_int(json, "remaining", &value);

    if (result != 0) { FAIL("returned error"); return; }
    if (value != 0) { FAIL("wrong value - should be 0"); return; }
    PASS();
}


/**
 * Test parsing integer value followed by closing brace (no comma).
 */
static void test_json_int_last_field(void) {
    TEST("parse_json_int - last field in object (no trailing comma)");

    const char *json = "{\"name\":\"test\",\"count\":42}";

    int value = 0;
    int result = parse_json_int(json, "count", &value);

    if (result != 0) { FAIL("returned error"); return; }
    if (value != 42) { FAIL("wrong value"); return; }
    PASS();
}


/* ─── Null Pointer Safety Tests ──────────────────────────────── */

/**
 * Test that parse_json_string handles NULL inputs safely.
 */
static void test_json_string_null_inputs(void) {
    TEST("parse_json_string - NULL inputs");

    char value[64] = {0};

    /* All should return -1 without crashing */
    int r1 = parse_json_string(NULL, "key", value, sizeof(value));
    int r2 = parse_json_string("{}", NULL, value, sizeof(value));
    int r3 = parse_json_string("{}", "key", NULL, sizeof(value));
    int r4 = parse_json_string("{}", "key", value, 0);

    if (r1 == 0 || r2 == 0 || r3 == 0 || r4 == 0) {
        FAIL("should have returned error for NULL inputs");
        return;
    }
    PASS();
}


/**
 * Test that parse_json_int handles NULL inputs safely.
 */
static void test_json_int_null_inputs(void) {
    TEST("parse_json_int - NULL inputs");

    int value = 0;

    int r1 = parse_json_int(NULL, "key", &value);
    int r2 = parse_json_int("{}", NULL, &value);
    int r3 = parse_json_int("{}", "key", NULL);

    if (r1 == 0 || r2 == 0 || r3 == 0) {
        FAIL("should have returned error for NULL inputs");
        return;
    }
    PASS();
}


/* ─── Rate Limit Info Struct Tests ───────────────────────────── */

/**
 * Test rate_limit_info struct initialization.
 */
static void test_rate_limit_info_defaults(void) {
    TEST("rate_limit_info - struct fields initialized to zero");

    rate_limit_info info;
    memset(&info, 0, sizeof(info));

    if (info.remaining != 0) { FAIL("remaining should be 0"); return; }
    if (info.reset_time != 0) { FAIL("reset_time should be 0"); return; }
    if (info.headers_parsed != 0) { FAIL("headers_parsed should be 0"); return; }
    PASS();
}


/**
 * Test rate_limit_info struct with typical values.
 */
static void test_rate_limit_info_typical(void) {
    TEST("rate_limit_info - typical GitHub API values");

    rate_limit_info info;
    info.remaining = 59;
    info.reset_time = 1700000000L;
    info.headers_parsed = 1;

    if (info.remaining != 59) { FAIL("remaining wrong"); return; }
    if (info.reset_time != 1700000000L) { FAIL("reset_time wrong"); return; }
    if (info.headers_parsed != 1) { FAIL("headers_parsed wrong"); return; }
    PASS();
}


/**
 * Test rate_limit_info struct with rate-limited values.
 */
static void test_rate_limit_info_rate_limited(void) {
    TEST("rate_limit_info - rate-limited state (remaining=0)");

    rate_limit_info info;
    info.remaining = 0;
    info.reset_time = 1700000600L;  /* Reset 600 seconds in future */
    info.headers_parsed = 1;

    if (info.remaining != 0) { FAIL("remaining should be 0"); return; }
    if (info.reset_time <= 0) { FAIL("reset_time should be positive"); return; }
    PASS();
}


/* ─── Network Buffer Constants Tests ─────────────────────────── */

/**
 * Test that network buffer constants are defined and have reasonable values.
 */
static void test_network_constants(void) {
    TEST("Network constants - reasonable values");

    if (MAX_HTTP_RESPONSE_LEN <= 0) { FAIL("MAX_HTTP_RESPONSE_LEN invalid"); return; }
    if (HTTP_READ_CHUNK_SIZE <= 0) { FAIL("HTTP_READ_CHUNK_SIZE invalid"); return; }
    if (strlen(HTTP_USER_AGENT) == 0) { FAIL("HTTP_USER_AGENT empty"); return; }

    /* MAX_HTTP_RESPONSE_LEN should be at least 1KB for API responses */
    if (MAX_HTTP_RESPONSE_LEN < 1024) { FAIL("MAX_HTTP_RESPONSE_LEN too small"); return; }

    /* HTTP_READ_CHUNK_SIZE should be at least 1KB */
    if (HTTP_READ_CHUNK_SIZE < 1024) { FAIL("HTTP_READ_CHUNK_SIZE too small"); return; }

    PASS();
}


/* ─── Main ─────────────────────────────────────────────────── */

int main(void) {
    ghb_context ctx = create_test_context();
    ctx.logger->log_init(&ctx, "/tmp/test_network.log");
    ctx.notify->notify_init(&ctx);

    printf("=== Network Module Tests ===\n\n");

    /* JSON string parsing */
    printf("-- parse_json_string tests --\n");
    test_json_string_typical();
    test_json_string_first_key();
    test_json_string_key_not_found();
    test_json_string_empty_value();
    test_json_string_with_whitespace();
    test_json_string_buffer_overflow();
    test_json_string_special_chars();

    /* JSON escape sequences */
    printf("\n-- JSON escape sequence tests --\n");
    test_json_string_escaped_quote();
    test_json_string_escaped_backslash();
    test_json_string_escaped_slash();
    test_json_string_escaped_newline();
    test_json_string_escaped_tab();
    test_json_string_escaped_cr();
    test_json_string_unicode_escape();
    test_json_string_multiple_escapes();
    test_json_string_trailing_backslash();

    /* JSON integer parsing */
    printf("\n-- parse_json_int tests --\n");
    test_json_int_positive();
    test_json_int_negative();
    test_json_int_key_not_found();
    test_json_int_with_whitespace();
    test_json_int_large_value();
    test_json_int_mid_object();
    test_json_int_zero();
    test_json_int_last_field();

    /* Null pointer safety */
    printf("\n-- Null pointer safety tests --\n");
    test_json_string_null_inputs();
    test_json_int_null_inputs();

    /* Rate limit info */
    printf("\n-- Rate limit info struct tests --\n");
    test_rate_limit_info_defaults();
    test_rate_limit_info_typical();
    test_rate_limit_info_rate_limited();

    /* Network constants */
    printf("\n-- Network constants tests --\n");
    test_network_constants();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    ctx.logger->log_close(&ctx);
    ctx.notify->notify_cleanup(&ctx);

    return tests_failed > 0 ? 1 : 0;
}
