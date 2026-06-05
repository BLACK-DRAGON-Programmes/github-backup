/**
 * console.c - Console output implementation for the GitHub Backup Script.
 *
 * Implements ANSI color-coded log output and the log viewer tailing loop
 * for viewer mode. The viewer is a disposable process - closing it or
 * pressing Ctrl+C kills only the viewer, not the daemon.
 *
 * Viewer controls (Spec Section 10c, 11a):
 *   q       - Signal daemon to shut down gracefully, then exit viewer
 *   Ctrl+C  - Close viewer only (daemon continues running)
 *   Alt+F4  - Close viewer only (daemon continues running)
 *   X button - Close viewer only (daemon continues running)
 */

#include "console.h"
#include "logger.h"
#include "constants.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
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

    /*
     * Startup banner - updated for two-process model.
     * Shows controls and behavior hints per Spec Section 10c.
     */
    fprintf(stdout, "\n");
    fprintf(stdout, "%s%s=============================%s\n",
            ANSI_CYAN, ANSI_DIM, ANSI_RESET);
    fprintf(stdout, "%s%s  GitHub Backup - Log Viewer  %s\n",
            ANSI_CYAN, ANSI_DIM, ANSI_RESET);
    fprintf(stdout, "%s%s=============================%s\n",
            ANSI_CYAN, ANSI_DIM, ANSI_RESET);
    fprintf(stdout, "\n");
    fprintf(stdout, "%s  Press 'q' to shut down the backup daemon.%s\n",
            ANSI_YELLOW, ANSI_RESET);
    fprintf(stdout, "%s  Press Ctrl+C or close this window to disconnect.%s\n",
            ANSI_DIM, ANSI_RESET);
    fprintf(stdout, "%s  (The daemon keeps running when you close this window.)%s\n",
            ANSI_DIM, ANSI_RESET);
    fprintf(stdout, "\n");
    fflush(stdout);

    FILE *fp = fopen(log_path, "r");
    if (!fp) {
        fprintf(stdout, "%sCannot open log file: %s%s\n",
                ANSI_RED, log_path, ANSI_RESET);
        fprintf(stdout, "%sWaiting for daemon to create log file...%s\n",
                ANSI_DIM, ANSI_RESET);
        fflush(stdout);

        /*
         * If the log file doesn't exist yet (daemon just started),
         * wait for it to appear. Poll every 500ms for up to 10 seconds.
         */
        int waited = 0;
        while (waited < 20000) {
            Sleep(500);
            waited += 500;
            fp = fopen(log_path, "r");
            if (fp) break;
        }

        if (!fp) {
            fprintf(stdout, "%sLog file not found after 10 seconds - exiting.%s\n",
                    ANSI_RED, ANSI_RESET);
            fflush(stdout);
            return;
        }
    }

    /* Seek to end - only show new entries from here on */
    fseek(fp, 0, SEEK_END);

    /* Initial tail - read any entries that appear between seek and loop start */
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
     * Main viewer loop - poll for new log entries AND keyboard input.
     * Uses _kbhit() for non-blocking keyboard check on Windows.
     *
     * Exit conditions:
     *   - 'q' pressed: signal daemon shutdown, then exit
     *   - Ctrl+C: handled by default console handler → process terminates
     *   - Window closed (X button, Alt+F4): OS terminates process
     */
#ifdef _WIN32
    /*
     * Disable Ctrl+C breaking into the viewer - let the default handler
     * terminate the viewer process cleanly. The daemon is unaffected
     * because it runs as a separate process with its own console lifetime.
     */
    SetConsoleCtrlHandler(NULL, TRUE);

    int shutdown_signaled = 0;

    while (1) {
        /*
         * Check for new log entries.
         * Use a short sleep (200ms) for responsive log tailing while
         * still checking keyboard input frequently enough.
         */
        Sleep(200);

        /* Re-read new lines */
        int new_lines = 0;
        while (fgets(line, sizeof(line), fp)) {
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
            if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';
            if (len == 0) continue;

            log_level lvl = detect_level(line);
            const char *color = color_for_level(lvl);
            fprintf(stdout, "%s%s%s\n", color, line, ANSI_RESET);
            fflush(stdout);
            new_lines++;
        }

        if (ferror(fp)) break;

        /*
         * Check for 'q' key press.
         * _kbhit() returns non-zero if a key is available without blocking.
         * _getch() reads the key without echoing it.
         */
        if (_kbhit()) {
            int ch = _getch();
            if (ch == 'q' || ch == 'Q') {
                fprintf(stdout, "\n%s%sShutdown requested - signaling daemon...%s\n",
                        ANSI_YELLOW, ANSI_DIM, ANSI_RESET);
                fflush(stdout);

                /*
                 * Signal the daemon to shut down gracefully by setting
                 * the named shutdown event (Spec Section 11a).
                 */
                HANDLE h_event = OpenEventA(EVENT_MODIFY_STATE, FALSE,
                                            BACKUP_SHUTDOWN_EVENT_NAME);
                if (h_event != NULL) {
                    SetEvent(h_event);
                    CloseHandle(h_event);
                    shutdown_signaled = 1;
                    (void)shutdown_signaled;
                } else {
                    fprintf(stdout, "%sError: Cannot signal shutdown (daemon may already be stopped)%s\n",
                            ANSI_RED, ANSI_RESET);
                    fflush(stdout);
                    fclose(fp);
                    return;
                }

                /*
                 * Wait briefly for the daemon to exit (up to 5 seconds).
                 * Continue showing any log entries the daemon writes
                 * during its shutdown sequence.
                 */
                fprintf(stdout, "%sWaiting for daemon to finish...%s\n",
                        ANSI_DIM, ANSI_RESET);
                fflush(stdout);

                DWORD wait_start = GetTickCount();
                while (GetTickCount() - wait_start < 5000) {
                    Sleep(200);
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
                }

                fprintf(stdout, "%s%sDaemon shutdown signal sent - viewer exiting.%s\n",
                        ANSI_GREEN, ANSI_DIM, ANSI_RESET);
                fflush(stdout);
                break;
            }
        }
    }
#endif

    fclose(fp);
}

int console_is_active(void) {
    return g_console_active;
}

void console_cleanup(void) {
    g_console_active = 0;
}
