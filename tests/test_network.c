/**
 * test_network.c — Unit tests for the network module.
 *
 * Tests the platform-independent JSON parsing functions and verifies
 * function signatures compile correctly. Full HTTP/network tests require
 * a live GitHub API connection and are performed on Windows during
 * integration testing.
 *
 * Compile (Linux):  gcc -o test_network tests/test_network.c src/network.c src/logger.c src/notify.c
 * Compile (Win):    gcc -o test_network.exe tests/test_network.c src/network.c src/logger.c src/notify.c -lwinhttp
 */

#include "network.h"
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


/* ─── JSON String Parsing Tests ─────────────────────────────── */

/**
 * Test extracting a string value from a typical GitHub API response.
 * The response body contains "default_branch": "main" — verify extraction.
 */
static void test_json_string_typical(void) {
    TEST("parse_json_string — typical API response");

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
    TEST("parse_json_string — first key in object");

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
    TEST("parse_json_string — key not found");

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
    TEST("parse_json_string — empty string value");

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
    TEST("parse_json_string — whitespace between tokens");

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
    TEST("parse_json_string — buffer truncation");

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
 * (hyphens, dots, underscores) — typical of branch and repo names.
 */
static void test_json_string_special_chars(void) {
    TEST("parse_json_string — special characters in value");

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


/* ─── JSON Integer Parsing Tests ────────────────────────────── */

/**
 * Test extracting a positive integer from a JSON response.
 */
static void test_json_int_positive(void) {
    TEST("parse_json_int — positive integer");

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
    TEST("parse_json_int — negative integer");

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
    TEST("parse_json_int — key not found");

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
    TEST("parse_json_int — whitespace between tokens");

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
    TEST("parse_json_int — large timestamp value");

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
    TEST("parse_json_int — value in middle of object");

    const char *json = "{\"a\":1,\"remaining\":58,\"b\":2}";

    int value = 0;
    int result = parse_json_int(json, "remaining", &value);

    if (result != 0) { FAIL("returned error"); return; }
    if (value != 58) { FAIL("wrong value"); return; }
    PASS();
}


/* ─── Null Pointer Safety Tests ──────────────────────────────── */

/**
 * Test that parse_json_string handles NULL inputs safely.
 */
static void test_json_string_null_inputs(void) {
    TEST("parse_json_string — NULL inputs");

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
    TEST("parse_json_int — NULL inputs");

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


/* ─── Main ─────────────────────────────────────────────────── */

int main(void) {
    log_init("/tmp/test_network.log");
    notify_init();

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

    /* JSON integer parsing */
    printf("\n-- parse_json_int tests --\n");
    test_json_int_positive();
    test_json_int_negative();
    test_json_int_key_not_found();
    test_json_int_with_whitespace();
    test_json_int_large_value();
    test_json_int_mid_object();

    /* Null pointer safety */
    printf("\n-- Null pointer safety tests --\n");
    test_json_string_null_inputs();
    test_json_int_null_inputs();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    log_close();
    notify_cleanup();

    return tests_failed > 0 ? 1 : 0;
}
