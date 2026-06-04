/**
 * main.c — Entry point for the GitHub Backup Script.
 *
 * This is the last source file in the build sequence because it depends
 * on every other module. It provides the program entry point, performs
 * startup validation (.env exists and is valid), enters the main loop
 * (connectivity check → backup cycle → sleep → repeat), and handles
 * graceful shutdown.
 *
 * main.c does not introduce new functionality — it wires together the
 * modules built in earlier stages:
 *   - config:  loads settings from .env
 *   - logger:  initializes and manages the log file
 *   - notify:  initializes COM for toast notifications
 *   - network: checks internet connectivity
 *   - backup:  runs the per-repo backup cycle
 *   - console: ANSI color output, log viewer, instance detection
 *
 * Runtime modes (Spec Sections 10, 11):
 *   backup.exe              → backup mode (or log viewer if already running)
 *   backup.exe --background → backup mode, detach console after init
 *   backup.exe --shutdown   → signal running instance to exit gracefully
 *
 * Startup sequence:
 *   1. Parse command-line arguments (--shutdown, --background)
 *   2. Initialize COM (notify_init)
 *   3. Attempt CreateMutex — if exists, enter log viewer; if created, continue
 *   4. If --shutdown: signal shutdown event and exit
 *   5. Determine where the exe lives (get_exe_dir)
 *   6. Find .env next to the exe, parse it
 *   7. Create BACKUP_DIR if it doesn't exist
 *   8. Initialize log file in BACKUP_DIR
 *   9. Initialize console (ANSI output)
 *   10. Initialize network session
 *   11. Create shutdown event
 *   12. Register SIGINT handler (Ctrl+C → graceful shutdown)
 *   13. If --background: FreeConsole()
 *   14. Fire "service started" toast
 *   15. Enter main loop (with shutdown event polling)
 */

#include "constants.h"
#include "config.h"
#include "logger.h"
#include "notify.h"
#include "network.h"
#include "backup.h"
#include "console.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif


/* ================================================================
 * SHUTDOWN STATE
 * ================================================================ */

/**
 * Flag set by the SIGINT handler (Ctrl+C) to request graceful shutdown.
 * Checked at the start of each main loop iteration.
 */
static volatile int g_shutdown_requested = 0;

#ifdef _WIN32
/** Handle to the named shutdown event. Checked during sleep intervals. */
static HANDLE g_shutdown_event = NULL;

/** Handle to the named mutex. Held for the lifetime of the process. */
static HANDLE g_backup_mutex = NULL;
#endif

/**
 * SIGINT handler for Ctrl+C. Sets the shutdown flag.
 * On Windows, this is registered via SetConsoleCtrlHandler.
 */
#ifdef _WIN32
static BOOL WINAPI ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        g_shutdown_requested = 1;
        return TRUE;  /* We handled it — don't terminate */
    }
    return FALSE;  /* Let default handler deal with it */
}
#else
static void sigint_handler(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
}
#endif


/* ================================================================
 * SLEEP HELPER (with shutdown polling)
 * ================================================================ */

/**
 * Sleep for a specified number of seconds, checking the shutdown
 * event every SHUTDOWN_CHECK_INTERVAL_MS milliseconds.
 * Returns early if shutdown is requested.
 *
 * @param seconds  Number of seconds to sleep
 */
static void sleep_with_shutdown_check(int seconds) {
    if (seconds <= 0) return;

#ifdef _WIN32
    int total_ms = seconds * 1000;
    int remaining_ms = total_ms;

    while (remaining_ms > 0) {
        /* Check both the flag (Ctrl+C) and the event (--shutdown) */
        if (g_shutdown_requested) return;

        if (g_shutdown_event != NULL) {
            DWORD wait_ms = (remaining_ms < SHUTDOWN_CHECK_INTERVAL_MS)
                            ? (DWORD)remaining_ms
                            : SHUTDOWN_CHECK_INTERVAL_MS;
            DWORD result = WaitForSingleObject(g_shutdown_event, wait_ms);
            if (result == WAIT_OBJECT_0) {
                g_shutdown_requested = 1;
                return;
            }
            remaining_ms -= (int)wait_ms;
        } else {
            Sleep((DWORD)remaining_ms);
            return;
        }
    }
#else
    int checked = 0;
    while (checked < seconds) {
        if (g_shutdown_requested) return;
        sleep(1);
        checked++;
    }
#endif
}


/* ================================================================
 * SHUTDOWN MODE
 * ================================================================ */

/**
 * Signal the running backup instance to exit gracefully.
 * Opens the named shutdown event and sets it.
 * Returns 0 on success, -1 if the instance is not running or event fails.
 */
static int signal_shutdown(void) {
#ifdef _WIN32
    HANDLE h_event = OpenEventA(EVENT_MODIFY_STATE, FALSE,
                                BACKUP_SHUTDOWN_EVENT_NAME);
    if (h_event == NULL) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            fprintf(stderr, "No backup instance is running.\n");
        } else {
            fprintf(stderr, "Error: Cannot open shutdown event (code %lu).\n",
                    GetLastError());
        }
        return -1;
    }

    if (!SetEvent(h_event)) {
        fprintf(stderr, "Error: Cannot signal shutdown (code %lu).\n",
                GetLastError());
        CloseHandle(h_event);
        return -1;
    }

    CloseHandle(h_event);
    printf("Shutdown signal sent. The backup instance will exit gracefully.\n");
    return 0;
#else
    fprintf(stderr, "Shutdown signal not supported on this platform.\n");
    return -1;
#endif
}


/* ================================================================
 * INSTANCE DETECTION
 * ================================================================ */

/**
 * Attempt to create the named mutex for single-instance detection.
 * Returns:
 *   0  — mutex created successfully (this is the first/only instance)
 *   1  — mutex already exists (another instance is running)
 *  -1  — error
 */
static int check_single_instance(void) {
#ifdef _WIN32
    g_backup_mutex = CreateMutexA(NULL, FALSE, BACKUP_MUTEX_NAME);
    if (g_backup_mutex == NULL) {
        return -1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* Another instance owns the mutex */
        CloseHandle(g_backup_mutex);
        g_backup_mutex = NULL;
        return 1;
    }
    return 0;
#else
    return 0;  /* No mutex on non-Windows */
#endif
}


/* ================================================================
 * STARTUP VALIDATION
 * ================================================================ */

static int validate_env_exists(const char *env_path) {
    FILE *fp = fopen(env_path, "r");
    if (fp == NULL) {
        fprintf(stderr, "Error: Config file (.env) not found at: %s\n", env_path);
        return -1;
    }
    fclose(fp);
    return 0;
}


/* ================================================================
 * MAIN LOOP
 * ================================================================ */

static void run_main_loop(backup_config *config, const char *exe_dir) {
    for (;;) {
        /* Check for shutdown request (Ctrl+C or --shutdown signal) */
        if (g_shutdown_requested) {
            log_event(LOG_INFO, "main", NULL, "STOPPING",
                      "Shutdown requested — exiting gracefully");
            toast_info("Shutting Down",
                       "GitHub Backup is shutting down gracefully");
            break;
        }

        /*
         * Step 1: Check internet connectivity.
         * If no internet, skip the cycle and sleep.
         */
        fprintf(stderr, "[DBG] main_loop: Checking connectivity...\n");
        fflush(stderr);

        if (!check_connectivity(config->connectivity_timeout)) {
            log_event(LOG_WARNING, "main", NULL, "SKIPPED",
                      "No internet detected — cycle skipped");
            toast_info("No Internet",
                       "No internet detected — cycle skipped");
            sleep_with_shutdown_check(config->cycle_interval);
            continue;
        }

        /*
         * Step 2: Fresh config read every cycle.
         */
        fprintf(stderr, "[DBG] main_loop: Re-reading .env...\n");
        fflush(stderr);

        backup_config cycle_config;
        memset(&cycle_config, 0, sizeof(backup_config));
        snprintf(cycle_config.backup_dir, sizeof(cycle_config.backup_dir),
                 "%s", exe_dir);

        if (parse_env_file(&cycle_config) != 0) {
            log_error("main", NULL,
                      "Failed to parse .env — skipping this cycle");
            toast_error("Config Error",
                        "Failed to parse .env — skipping this cycle");
            sleep_with_shutdown_check(config->cycle_interval);
            continue;
        }

        /* Ensure BACKUP_DIR exists */
        if (ensure_dir_exists(cycle_config.backup_dir) != 0) {
            log_error("main", NULL,
                      "Cannot create or access BACKUP_DIR — skipping this cycle");
            toast_error("Directory Error",
                        "Cannot create BACKUP_DIR — check permissions");
            sleep_with_shutdown_check(config->cycle_interval);
            continue;
        }

        /*
         * Step 3: Fire cycle-start toast.
         */
        char start_msg[MAX_URL_LEN];
        snprintf(start_msg, sizeof(start_msg),
                 "Starting backup cycle for %d repositories",
                 cycle_config.repo_count);
        log_event(LOG_INFO, "main", NULL, "CYCLE_START", start_msg);
        toast_info("Backup Cycle", start_msg);

        /*
         * Step 4: Run the backup cycle.
         */
        int succeeded = 0;
        int failed = 0;

        int cycle_result = run_backup_cycle(&cycle_config,
                                            &succeeded, &failed);

        /*
         * Step 5: Fire cycle-complete toast with summary.
         */
        char complete_msg[MAX_URL_LEN];
        if (cycle_result != 0) {
            snprintf(complete_msg, sizeof(complete_msg),
                     "Backup cycle ABORTED: %d succeeded, %d failed (disk full)",
                     succeeded, failed);
        } else {
            snprintf(complete_msg, sizeof(complete_msg),
                     "Backup cycle complete: %d succeeded, %d failed",
                     succeeded, failed);
        }
        log_event(LOG_INFO, "main", NULL, "CYCLE_COMPLETE", complete_msg);
        toast_info("Cycle Complete", complete_msg);

        /*
         * Step 6: Rotate log if it exceeds the size threshold.
         */
        rotate_log(cycle_config.log_max_size);

        /*
         * Step 7: Sleep until the next cycle (with shutdown polling).
         */
        log_event(LOG_INFO, "main", NULL, "SLEEP",
                  "Sleeping until next cycle");
        sleep_with_shutdown_check(cycle_config.cycle_interval);
    }
}


/* ================================================================
 * ENTRY POINT
 * ================================================================ */

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /*
     * Step 1: Parse command-line arguments.
     * Check for --shutdown and --background flags.
     */
    fprintf(stderr, "[DBG] main: Startup — parsing arguments\n");
    fflush(stderr);
    int want_shutdown = 0;
    int want_background = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shutdown") == 0) {
            want_shutdown = 1;
        } else if (strcmp(argv[i], "--background") == 0) {
            want_background = 1;
        }
    }

    /*
     * Step 2: Initialize notification subsystem (COM on Windows).
     * Must happen first so that startup errors can fire toasts.
     */
    fprintf(stderr, "[DBG] main: Initializing COM (notifications)...\n");
    fflush(stderr);

    if (notify_init() != 0) {
        fprintf(stderr, "Warning: Toast notifications unavailable\n");
    }

    /*
     * Step 3: Check for single instance.
     * If another instance is running, enter log viewer or shutdown mode.
     */
    fprintf(stderr, "[DBG] main: Checking single instance (mutex)...\n");
    fflush(stderr);

    int instance_result = check_single_instance();

    if (instance_result == 1) {
        /* Another instance is running */
        if (want_shutdown) {
            /* --shutdown flag: signal the running instance */
            int result = signal_shutdown();
            notify_cleanup();
            return (result == 0) ? 0 : 1;
        }

        /* No --shutdown: enter log viewer mode */
        console_init();

        /* Find the log file path — need to parse .env for BACKUP_DIR */
        char exe_dir[MAX_URL_LEN];
        get_exe_dir(exe_dir, sizeof(exe_dir));

        backup_config cfg;
        memset(&cfg, 0, sizeof(backup_config));
        snprintf(cfg.backup_dir, sizeof(cfg.backup_dir), "%s", exe_dir);
        apply_defaults(&cfg);
        parse_env_file(&cfg);

        char log_path[MAX_URL_LEN];
        build_log_path(cfg.backup_dir, log_path);

        console_log_viewer(log_path);
        console_cleanup();
        notify_cleanup();
        return 0;
    }

    if (instance_result < 0) {
        fprintf(stderr, "Error: Cannot check single instance (mutex creation failed)\n");
        notify_cleanup();
        return 1;
    }

    /*
     * Step 4: Determine where this executable lives.
     */
    fprintf(stderr, "[DBG] main: Resolving exe directory...\n");
    fflush(stderr);

    char exe_dir[MAX_URL_LEN];
    get_exe_dir(exe_dir, sizeof(exe_dir));

    if (exe_dir[0] == '\0') {
        fprintf(stderr, "Error: Cannot determine executable directory\n");
        if (g_backup_mutex) CloseHandle(g_backup_mutex);
        notify_cleanup();
        return 1;
    }

    fprintf(stderr, "Info: Exe directory: %s\n", exe_dir);

    /*
     * Step 5: Find and validate .env — it sits next to the exe.
     */
    char env_path[MAX_URL_LEN];
    build_env_path(exe_dir, env_path);
    fprintf(stderr, "[DBG] main: Validating .env at %s\n", env_path);
    fflush(stderr);

    if (validate_env_exists(env_path) != 0) {
        toast_error("Config Error",
                    "Config file (.env) not found next to executable — exiting");
        if (g_backup_mutex) CloseHandle(g_backup_mutex);
        notify_cleanup();
        return 1;
    }

    /*
     * Step 6: Parse .env.
     */
    fprintf(stderr, "[DBG] main: Parsing .env...\n");
    fflush(stderr);

    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", exe_dir);

    if (parse_env_file(&config) != 0) {
        fprintf(stderr, "Error: Config validation failed — exiting\n");
        toast_error("Config Error",
                    "Config validation failed — exiting (requires manual intervention)");
        if (g_backup_mutex) CloseHandle(g_backup_mutex);
        notify_cleanup();
        return 1;
    }

    fprintf(stderr, "Info: BACKUP_DIR: %s\n", config.backup_dir);
    fprintf(stderr, "[DBG] main: Parsed %d repos, owner=%s, timeout=%dms, cycle=%ds\n",
            config.repo_count, config.owner, config.http_timeout, config.cycle_interval);
    fflush(stderr);

    /*
     * Step 7: Create BACKUP_DIR if it doesn't exist.
     */
    fprintf(stderr, "[DBG] main: Ensuring BACKUP_DIR exists: %s\n", config.backup_dir);
    fflush(stderr);

    if (ensure_dir_exists(config.backup_dir) != 0) {
        fprintf(stderr, "Error: Cannot create BACKUP_DIR: %s\n", config.backup_dir);
        toast_error("Directory Error",
                    "Cannot create BACKUP_DIR — check permissions and path");
        if (g_backup_mutex) CloseHandle(g_backup_mutex);
        notify_cleanup();
        return 1;
    }

    /*
     * Step 8: Initialize the log file in BACKUP_DIR.
     */
    char log_path[MAX_URL_LEN];
    build_log_path(config.backup_dir, log_path);
    fprintf(stderr, "[DBG] main: Initializing log file at %s\n", log_path);
    fflush(stderr);

    if (log_init(log_path) != 0) {
        toast_error("Log Error",
                    "Cannot open log file — continuing without file logging");
    }

    /*
     * Step 9: Initialize console for ANSI output.
     */
    fprintf(stderr, "[DBG] main: Initializing console (ANSI)...\n");
    fflush(stderr);
    console_init();
    log_set_console_output(console_is_active());

    /*
     * Step 10: Initialize the network session.
     */
    fprintf(stderr, "[DBG] main: Initializing network (WinHTTP)...\n");
    fflush(stderr);

    if (network_init() != 0) {
        log_error("startup", NULL,
                  "Network initialization failed — cannot proceed");
        toast_error("Network Error",
                    "Failed to initialize HTTP session — exiting");
        log_close();
        if (g_backup_mutex) CloseHandle(g_backup_mutex);
        notify_cleanup();
        console_cleanup();
        return 1;
    }

    /*
     * Step 11: Create the shutdown event.
     * This event is checked during sleep intervals and by --shutdown.
     */
    fprintf(stderr, "[DBG] main: Creating shutdown event...\n");
    fflush(stderr);

#ifdef _WIN32
    g_shutdown_event = CreateEventA(NULL, TRUE, FALSE,
                                   BACKUP_SHUTDOWN_EVENT_NAME);
    if (g_shutdown_event == NULL) {
        log_event(LOG_WARNING, "main", NULL, "EVENT_WARN",
                  "Cannot create shutdown event — --shutdown will not work");
    }
#endif

    /*
     * Step 12: Register Ctrl+C handler for graceful shutdown.
     */
    fprintf(stderr, "[DBG] main: Registering Ctrl+C handler...\n");
    fflush(stderr);

#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    signal(SIGINT, sigint_handler);
#endif

    /*
     * Step 13: If --background, detach from console.
     * Must happen AFTER console_init (so we know ANSI works) and
     * AFTER all startup messages have been printed.
     */
    if (want_background) {
        fprintf(stderr, "[DBG] main: Detaching console (--background)\n");
        fflush(stderr);
#ifdef _WIN32
        FreeConsole();
        log_set_console_output(0);  /* No console → no console output */
#endif
    }

    /*
     * Step 14: Fire "service started" toast.
     */
    log_event(LOG_INFO, "main", NULL, "STARTED",
              "GitHub Backup service started successfully");
    toast_info("Service Started",
               "GitHub Backup service is running in the background");

    /*
     * Step 15: Enter the main backup loop.
     */
    run_main_loop(&config, exe_dir);

    /*
     * Cleanup.
     */
    log_event(LOG_INFO, "main", NULL, "STOPPED",
              "GitHub Backup service shutting down");
    log_close();
    network_cleanup();
    notify_cleanup();
    console_cleanup();

#ifdef _WIN32
    if (g_shutdown_event) CloseHandle(g_shutdown_event);
    if (g_backup_mutex) CloseHandle(g_backup_mutex);
#endif

    return 0;
}
