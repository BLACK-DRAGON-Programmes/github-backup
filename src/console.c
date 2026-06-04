/**
 * console.c — Console output implementation for the GitHub Backup Script.
 *
 * Implements ANSI color-coded log output for foreground mode and
 * the log viewer tailing loop for the second-instance mode.
 */

#include "console.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#define Sleep(ms) usleep((ms)*1000)
#endif

/* ─── ANSI Escape Sequences ───────────────────────────────────── */

#define ANSI_RESET       "\033[0m"
#define ANSI_DIM         "\033[2m"
#define ANSI_RED         "\033[31m"
#define ANSI_GREEN       "\033[32m"
#define ANSI_YELLOW      "\033[33m"
#define ANSI_CYAN        "\033[36m"
#define ANSI_WHITE       "\033[37m"

/** Console output is active (ANSI enabled, not background). */
static int g_console_active = 0;

/* ─── Timestamp ──────────────────────────────────────────────── */

static void get_timestamp(char *buf, int buf_len) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, (size_t)buf_len, "%Y-%m-%d %H:%M:%S", tm_info);
}

/* ─── Color for Log Level ─────────────────────────────────────── */

static const char *color_for_level(log_level level) {
    switch (level) {
        case LOG_INFO:    return ANSI_WHITE;
        case LOG_SUCCESS: return ANSI_GREEN;
        case LOG_WARNING: return ANSI_YELLOW;
        case LOG_ERROR:   return ANSI_RED;
        default:          return ANSI_WHITE;
    }
}

static const char *level_string(log_level level) {
    switch (level) {
        case LOG_INFO:    return "INFO ";
        case LOG_SUCCESS: return "OK   ";
        case LOG_WARNING: return "WARN ";
        case LOG_ERROR:   return "ERROR";
        default:          return "?????";
    }
}

/* ─── Detect log level from a plain-text log line ────────────── */

static log_level detect_level(const char *line) {
    if (strstr(line, "] ERROR") || strstr(line, "] FAILED"))
        return LOG_ERROR;
    if (strstr(line, "] WARN"))
        return LOG_WARNING;
    if (strstr(line, "] OK") || strstr(line, "] BACKED_UP") ||
        strstr(line, "] CONNECTED"))
        return LOG_SUCCESS;
    return LOG_INFO;
}

/* ─── Public Functions ──────────────────────────────────────── */

int console_init(void) {
#ifdef _WIN32
    HANDLE h_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h_stdout == INVALID_HANDLE_VALUE || h_stdout == NULL) {
        return -1;  /* No console (background mode) */
    }

    DWORD mode = 0;
    if (!GetConsoleMode(h_stdout, &mode)) {
        return -1;
    }

    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(h_stdout, mode)) {
        return -1;
    }

    g_console_active = 1;
#endif
    return 0;
}

void console_print_log(log_level level, const char *action,
                       const char *repo, const char *status,
                       const char *detail) {
    if (!g_console_active) return;

    char timestamp[20];
    get_timestamp(timestamp, sizeof(timestamp));

    /*
     * Format: [timestamp]  LEVEL  | action | repo | status | detail
     * Columns: timestamp=19, space+level=7, action=12, repo=12, rest
     */
    fprintf(stdout, "%s%s[%s]%s  %s%s%s%s | ",
            ANSI_DIM, ANSI_RESET, timestamp, ANSI_DIM,
            color_for_level(level), level_string(level), ANSI_RESET, ANSI_DIM);

    if (action) {
        fprintf(stdout, "%-12s", action);
    } else {
        fprintf(stdout, "%-12s", "");
    }
    fprintf(stdout, " | ");

    if (repo) {
        fprintf(stdout, "%s%-12s%s", ANSI_CYAN, repo, ANSI_DIM);
    } else {
        fprintf(stdout, "%-12s", "");
    }
    fprintf(stdout, " | ");

    if (status) {
        fprintf(stdout, "%-12s", status);
    } else {
        fprintf(stdout, "%-12s", "");
    }

    if (detail) {
        fprintf(stdout, " | %s", detail);
    }

    fprintf(stdout, "%s\n", ANSI_RESET);
    fflush(stdout);
}

void console_log_viewer(const char *log_path) {
    if (!g_console_active) return;

    fprintf(stdout, "%s%sGitHub Backup — Log Viewer%s\n"
                    "%sPress Ctrl+C to exit. The backup process continues running.%s\n\n",
            ANSI_CYAN, ANSI_DIM, ANSI_RESET,
            ANSI_DIM, ANSI_RESET);
    fflush(stdout);

    FILE *fp = fopen(log_path, "r");
    if (!fp) {
        fprintf(stdout, "%sCannot open log file: %s%s\n",
                ANSI_RED, log_path, ANSI_RESET);
        fflush(stdout);
        return;
    }

    /* Seek to end — only show new entries from here on */
    fseek(fp, 0, SEEK_END);

    /* Tail loop */
    char line[MAX_CONSOLE_LINE_LEN];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }
        if (len > 0 && line[len - 1] == '\r') {
            line[len - 1] = '\0';
        }

        if (len == 0) continue;

        log_level lvl = detect_level(line);
        const char *color = color_for_level(lvl);
        fprintf(stdout, "%s%s%s\n", color, line, ANSI_RESET);
        fflush(stdout);
    }

    /* Check if we reached EOF normally (not error) */
    if (!feof(fp)) {
        fclose(fp);
        return;
    }

    /*
     * Now wait for new content. Poll every 500ms.
     * Exit on Ctrl+C (SIGINT) — the default signal handler
     * will terminate the process.
     */
#ifdef _WIN32
    SetConsoleCtrlHandler(NULL, TRUE); /* Default handler */
#endif

    while (1) {
        Sleep(500);
        /* Re-check for new lines */
        while (fgets(line, sizeof(line), fp)) {
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
            if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';
            if (len == 0) continue;

            log_level lvl = detect_level(line);
            const char *color = color_for_level(lvl);
            fprintf(stdout, "%s%s%s\n", color, line, ANSI_RESET);
            fflush(stdout);
        }

        if (ferror(fp)) break;
    }

    fclose(fp);
}

int console_is_active(void) {
    return g_console_active;
}

void console_cleanup(void) {
    g_console_active = 0;
}
