/**
 * logger.c - Logging implementation for the GitHub Backup Script.
 *
 * Writes structured log entries to an append-only file. Each entry
 * follows the format:
 *
 *   [YYYY-MM-DD HH:MM:SS] LEVEL | action | repo | status | detail
 *
 * The file is opened once at startup (log_init), written to throughout
 * the program's lifetime (log_event), and closed on shutdown (log_close).
 * Rotation (rotate_log) deletes the file and reopens when it exceeds
 * the configured size threshold (Decision 003).
 */

#include "console.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif


/** Global log file handle. NULL before log_init is called. */
static FILE *g_log_file = NULL;

/** Path to the current log file (needed for rotation reopen). */
static char g_log_path[MAX_URL_LEN] = {0};

/** Whether to also print log entries to the console with ANSI colors. */
static int g_console_output = 0;


/* ─── Timestamp ──────────────────────────────────────────────── */

/**
 * Write the current time as ISO 8601 into the provided buffer.
 * Format: YYYY-MM-DD HH:MM:SS
 *
 * @param buf      Output buffer (at least 20 bytes)
 * @param buf_len  Buffer length
 */
static void get_timestamp(char *buf, int buf_len) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, (size_t)buf_len, "%Y-%m-%d %H:%M:%S", tm_info);
}


/* ─── Level to String ───────────────────────────────────────── */

static const char *level_string(log_level level) {
    switch (level) {
        case LOG_INFO:    return "INFO";
        case LOG_SUCCESS: return "OK";
        case LOG_WARNING: return "WARN";
        case LOG_ERROR:   return "ERROR";
        default:          return "UNKNOWN";
    }
}


/* ─── Public Functions ──────────────────────────────────────── */

int log_init(const char *log_path) {
    strncpy(g_log_path, log_path, MAX_URL_LEN - 1);
    g_log_path[MAX_URL_LEN - 1] = '\0';

    g_log_file = fopen(g_log_path, "a");
    if (g_log_file == NULL) {
        /*
         * Cannot open log file. Fall back to stderr so errors are not
         * completely silent. In background mode (Task Scheduler), stderr
         * is discarded, but at least we don't crash.
         */
        g_log_file = stderr;
        return -1;
    }

    return 0;
}


void log_event(log_level level, const char *action, const char *repo,
               const char *status, const char *detail) {
    if (g_log_file == NULL) {
        return;
    }

    char timestamp[20];
    get_timestamp(timestamp, sizeof(timestamp));

    fprintf(g_log_file, "[%s] %s | %s", timestamp, level_string(level), action);

    if (repo != NULL) {
        fprintf(g_log_file, " | %s", repo);
    }

    fprintf(g_log_file, " | %s", status);

    if (detail != NULL) {
        fprintf(g_log_file, " | %s", detail);
    }

    fprintf(g_log_file, "\n");
    fflush(g_log_file);

    /* DEV PHASE: Dual output to stderr for terminal visibility during testing.
     * Remove this block once the project exits the testing phase. */
    fprintf(stderr, "[%s] %s | %s", timestamp, level_string(level), action);
    if (repo != NULL) fprintf(stderr, " | %s", repo);
    fprintf(stderr, " | %s", status);
    if (detail != NULL) fprintf(stderr, " | %s", detail);
    fprintf(stderr, "\n");
    fflush(stderr);

    /* Bridge to ANSI console output when available */
    if (g_console_output) {
        console_print_log(level, action, repo, status, detail);
    }
}


void log_error(const char *action, const char *repo, const char *detail) {
    log_event(LOG_ERROR, action, repo, "FAILED", detail);
}


void rotate_log(long max_size_bytes) {
    if (max_size_bytes <= 0) {
        return;  /* Rotation disabled */
    }

    if (g_log_file == NULL || g_log_path[0] == '\0') {
        return;  /* Logger not initialized */
    }

    /*
     * Close the file first so we can check its size reliably.
     * On Windows, fflush before fclose ensures all buffered data
     * is written.
     */
    fflush(g_log_file);
    fclose(g_log_file);
    g_log_file = NULL;

    long file_size = 0;

    #ifdef _WIN32
    {
        HANDLE h = CreateFileA(
            g_log_path,
            GENERIC_READ,
            FILE_SHARE_READ,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );
        if (h != INVALID_HANDLE_VALUE) {
            file_size = (long)GetFileSize(h, NULL);
            CloseHandle(h);
        }
    }
    #else
    {
        struct stat st;
        if (stat(g_log_path, &st) == 0) {
            file_size = (long)st.st_size;
        }
    }
    #endif

    if (file_size > max_size_bytes) {
        /* Delete the old log file and start fresh (Decision 003). */
        remove(g_log_path);
    }

    /* Reopen the log file (either the deleted-then-recreated one,
       or the original if it didn't exceed the threshold). */
    g_log_file = fopen(g_log_path, "a");
    if (g_log_file == NULL) {
        g_log_file = stderr;
    }
}


void log_set_console_output(int enabled) {
    g_console_output = enabled;
}

int log_get_console_output(void) {
    return g_console_output;
}

void log_close(void) {
    if (g_log_file != NULL && g_log_file != stderr) {
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = NULL;
    }
}
