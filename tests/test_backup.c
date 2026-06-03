/**
 * test_backup.c — Unit tests for the backup module.
 *
 * Tests file verification, atomic write, and temp file cleanup.
 * Full backup cycle tests require a live GitHub API connection
 * and are performed on Windows during integration testing.
 *
 * These tests use the non-Windows code paths (POSIX file ops)
 * for verification and cleanup, and test the atomic write logic
 * with local files.
 *
 * Compile (Linux):  gcc -o test_backup tests/test_backup.c src/backup.c src/config.c src/network.c src/logger.c src/notify.c -I src/
 */

#include "backup.h"
#include "config.h"
#include "network.h"
#include "logger.h"
#include "notify.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif


static int tests_passed = 0;
static int tests_failed = 0;

/** Temporary test directory for file operation tests. */
#define TEST_DIR "/tmp/ghb_test_"


#define TEST(name) printf("  TEST: %s... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)


/**
 * Create a test directory and a file with specified content.
 * Returns the full path to the created file via file_path_out.
 */
static int create_test_file(const char *dir, const char *filename,
                            const char *content, char *file_path_out) {
    snprintf(file_path_out, MAX_URL_LEN, "%s%s", dir, filename);

    FILE *fp = fopen(file_path_out, "wb");
    if (fp == NULL) return -1;

    if (content != NULL) {
        fputs(content, fp);
    }
    fclose(fp);
    return 0;
}


/* ─── File Verification Tests ──────────────────────────────── */

static void test_verify_valid_file(void) {
    TEST("verify_downloaded_file — valid non-empty file");

    char path[MAX_URL_LEN];
    if (create_test_file(TEST_DIR "1/", "test.zip", "PK\x03\x04", path) != 0) {
        FAIL("could not create test file"); return;
    }

    int result = verify_downloaded_file(path);
    if (result != 0) { FAIL("should pass for valid file"); return; }
    PASS();

    remove(path);
}

static void test_verify_nonexistent_file(void) {
    TEST("verify_downloaded_file — nonexistent file");

    int result = verify_downloaded_file("/tmp/nonexistent_ghb_test_file.zip");
    if (result == 0) { FAIL("should fail for nonexistent file"); return; }
    PASS();
}

static void test_verify_empty_file(void) {
    TEST("verify_downloaded_file — empty file");

    char path[MAX_URL_LEN];
    if (create_test_file(TEST_DIR "2/", "empty.zip", "", path) != 0) {
        FAIL("could not create test file"); return;
    }

    int result = verify_downloaded_file(path);
    if (result == 0) { FAIL("should fail for empty file"); return; }
    PASS();

    remove(path);
}

static void test_verify_null_path(void) {
    TEST("verify_downloaded_file — NULL path");

    int result = verify_downloaded_file(NULL);
    if (result == 0) { FAIL("should fail for NULL path"); return; }
    PASS();
}

static void test_verify_empty_string_path(void) {
    TEST("verify_downloaded_file — empty string path");

    int result = verify_downloaded_file("");
    if (result == 0) { FAIL("should fail for empty path"); return; }
    PASS();
}


/* ─── Temp File Cleanup Tests ──────────────────────────────── */

static void test_cleanup_existing_file(void) {
    TEST("cleanup_temp_file — deletes existing file");

    char path[MAX_URL_LEN];
    if (create_test_file(TEST_DIR "3/", "temp.zip.tmp", "data", path) != 0) {
        FAIL("could not create test file"); return;
    }

    /* Verify it exists before cleanup */
    FILE *check = fopen(path, "rb");
    if (check == NULL) { FAIL("file not created"); return; }
    fclose(check);

    cleanup_temp_file(path);

    /* Verify it's gone */
    check = fopen(path, "rb");
    if (check != NULL) {
        fclose(check);
        FAIL("file still exists after cleanup"); return;
    }
    PASS();
}

static void test_cleanup_nonexistent_file(void) {
    TEST("cleanup_temp_file — nonexistent file (no crash)");

    /* Should not crash — just log a warning */
    cleanup_temp_file("/tmp/nonexistent_ghb_cleanup_test.tmp");
    PASS();
}


/* ─── Atomic Write Tests ────────────────────────────────────── */

static void test_atomic_write_no_existing(void) {
    TEST("atomic_write — no existing backup");

    char temp[MAX_URL_LEN], final[MAX_URL_LEN];
    create_test_file(TEST_DIR "4/", "repo.zip.tmp", "zipdata", temp);
    snprintf(final, sizeof(final), "%s%s", TEST_DIR "4/", "repo.zip");

    /* Ensure no existing final file */
    remove(final);

    int result = atomic_write(temp, final);
    if (result != 0) { FAIL("atomic_write failed"); return; }

    /* Verify temp is gone and final exists */
    FILE *temp_check = fopen(temp, "rb");
    if (temp_check != NULL) {
        fclose(temp_check);
        FAIL("temp file still exists after atomic write"); return;
    }

    FILE *final_check = fopen(final, "rb");
    if (final_check == NULL) {
        FAIL("final file not created"); return;
    }
    fclose(final_check);
    PASS();

    remove(final);
}

static void test_atomic_write_replace_existing(void) {
    TEST("atomic_write — replaces existing backup");

    char temp[MAX_URL_LEN], final[MAX_URL_LEN];
    snprintf(final, sizeof(final), "%s%s", TEST_DIR "5/", "repo.zip");

    /* Create existing "old" backup */
    create_test_file(TEST_DIR "5/", "repo.zip", "old_backup_data", final);
    /* Create new temp file */
    create_test_file(TEST_DIR "5/", "repo.zip.tmp", "new_backup_data", temp);

    int result = atomic_write(temp, final);
    if (result != 0) { FAIL("atomic_write failed"); return; }

    /* Verify temp is gone */
    FILE *temp_check = fopen(temp, "rb");
    if (temp_check != NULL) {
        fclose(temp_check);
        FAIL("temp file still exists"); return;
    }

    /* Verify final exists and has new content */
    char buf[32] = {0};
    FILE *fp = fopen(final, "rb");
    if (fp == NULL) { FAIL("final file missing"); return; }
    fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);

    if (strcmp(buf, "new_backup_data") != 0) {
        FAIL("final file has wrong content — not the new backup");
        return;
    }
    PASS();

    remove(final);
}

static void test_atomic_write_null_paths(void) {
    TEST("atomic_write — NULL paths return error");

    int r1 = atomic_write(NULL, "/tmp/test.txt");
    int r2 = atomic_write("/tmp/test.txt", NULL);

    if (r1 == 0 || r2 == 0) { FAIL("should return error for NULL"); return; }
    PASS();
}


/* ─── Backup Result Code Tests ─────────────────────────────── */

static void test_backup_result_values(void) {
    TEST("backup_result enum — OK is zero, others non-zero");

    if (BACKUP_OK != 0) {
        FAIL("BACKUP_OK should be 0"); return;
    }
    if (BACKUP_NOT_FOUND == 0 || BACKUP_AUTH_ERROR == 0 ||
        BACKUP_NETWORK_ERROR == 0 || BACKUP_DISK_FULL == 0) {
        FAIL("error codes should be non-zero"); return;
    }
    PASS();
}


/* ─── Main ─────────────────────────────────────────────────── */

int main(void) {
    log_init("/tmp/test_backup.log");
    notify_init();

    /* Create test directories */
    mkdir(TEST_DIR "1/", 0755);
    mkdir(TEST_DIR "2/", 0755);
    mkdir(TEST_DIR "3/", 0755);
    mkdir(TEST_DIR "4/", 0755);
    mkdir(TEST_DIR "5/", 0755);

    printf("=== Backup Module Tests ===\n\n");

    /* File verification */
    printf("-- File verification tests --\n");
    test_verify_valid_file();
    test_verify_nonexistent_file();
    test_verify_empty_file();
    test_verify_null_path();
    test_verify_empty_string_path();

    /* Temp file cleanup */
    printf("\n-- Temp file cleanup tests --\n");
    test_cleanup_existing_file();
    test_cleanup_nonexistent_file();

    /* Atomic write */
    printf("\n-- Atomic write tests --\n");
    test_atomic_write_no_existing();
    test_atomic_write_replace_existing();
    test_atomic_write_null_paths();

    /* Result codes */
    printf("\n-- Result code tests --\n");
    test_backup_result_values();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    /* Cleanup test directories */
    rmdir(TEST_DIR "1/");
    rmdir(TEST_DIR "2/");
    rmdir(TEST_DIR "3/");
    rmdir(TEST_DIR "4/");
    rmdir(TEST_DIR "5/");

    log_close();
    notify_cleanup();

    return tests_failed > 0 ? 1 : 0;
}
