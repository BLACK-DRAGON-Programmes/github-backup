/**
 * main.c - Entry point for the GitHub Backup Script.
 *
 * This is the last source file in the build sequence because it depends
 * on every other module. It provides the program entry point, performs
 * startup validation (.env exists and is valid), enters the main loop
 * (connectivity check → backup cycle → sleep → repeat), and handles
 * graceful shutdown.
 *
 * main.c does not introduce new functionality - it wires together the
 * modules built in earlier stages:
 *   - config:  loads settings from .env
 *   - logger:  initializes and manages the log file
 *   - notify:  initializes COM for toast notifications
 *   - network: checks internet connectivity
 *   - backup:  runs the per-repo backup cycle
 *   - console: ANSI color output, log viewer, instance detection
 *
 * Runtime modes (Spec Sections 10, 11):
 *   backup.exe              → viewer mode (or spawn daemon then viewer)
 *   backup.exe --daemon     → headless daemon mode (CREATE_NO_WINDOW)
 *   backup.exe --shutdown   → signal running daemon to exit gracefully
 *
 * Two-process architecture (Spec Section 10):
 *   The daemon is the headless backup worker - owns the mutex, runs the
 *   main loop, has no console. Viewers are disposable console processes
 *   that tail the log file. Closing a viewer does not affect the daemon.
 *   Pressing 'q' in a viewer signals the daemon to shut down gracefully.
 *
 * Startup sequence (daemon mode):
 *   1. Parse command-line arguments (--daemon, --shutdown)
 *   2. Initialize COM (notify_init)
 *   3. Attempt CreateMutex - daemon must create it; fail if already exists
 *   4. Determine where the exe lives (get_exe_dir)
 *   5. Find .env next to the exe, parse it
 *   6. Create BACKUP_DIR if it doesn't exist
 *   7. Initialize log file in BACKUP_DIR
 *   8. Initialize network session
 *   9. Create shutdown event
 *   10. Fire "service started" toast
 *   11. Enter main loop (with shutdown event polling)
 *
 * Startup sequence (viewer mode):
 *   1. Parse command-line arguments (--shutdown)
 *   2. Initialize COM (notify_init) - needed for potential toast on error
 *   3. Attempt CreateMutex - if created, no daemon running; spawn daemon,
 *      release mutex, then enter viewer mode
 *   4. If mutex exists, enter viewer mode directly
 *   5. Viewer tails backup.log with ANSI colors until q pressed or closed
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

/** Handle to the named mutex. Held for the lifetime of the daemon. */
static HANDLE g_backup_mutex = NULL;
#endif


/* ================================================================
 * SLEEP HELPER (with shutdown polling)
 * ================================================================ */

/**
 * Sleep for a specified number of seconds, checking the shutdown
 * event at a configurable interval. Returns early if shutdown is requested.
 *
 * @param seconds  Number of seconds to sleep
 * @param config   Backup config (for shutdown_check_interval)
 */
static void sleep_with_shutdown_check(int seconds, const backup_config *config) {
    if (seconds <= 0) return;

#ifdef _WIN32
    int total_ms = seconds * 1000;
    int remaining_ms = total_ms;

    while (remaining_ms > 0) {
        /* Check both the flag (Ctrl+C) and the event (--shutdown / q key) */
        if (g_shutdown_requested) return;

        if (g_shutdown_event != NULL) {
            DWORD interval_ms = (DWORD)config->shutdown_check_interval;
            DWORD wait_ms = ((DWORD)remaining_ms < interval_ms)
                            ? (DWORD)remaining_ms
                            : interval_ms;
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
 * Signal the running backup daemon to exit gracefully.
 * Opens the named shutdown event and sets it.
 * Returns 0 on success, -1 if the daemon is not running or event fails.
 */
static int signal_shutdown(void) {
#ifdef _WIN32
    HANDLE h_event = OpenEventA(EVENT_MODIFY_STATE, FALSE,
                                BACKUP_SHUTDOWN_EVENT_NAME);
    if (h_event == NULL) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            printf("No backup daemon is running.\n");
        } else {
            printf("Error: Cannot open shutdown event (code %lu).\n",
                   GetLastError());
        }
        return -1;
    }

    if (!SetEvent(h_event)) {
        printf("Error: Cannot signal shutdown (code %lu).\n",
               GetLastError());
        CloseHandle(h_event);
        return -1;
    }

    CloseHandle(h_event);
    printf("Shutdown signal sent. The backup daemon will exit gracefully.\n");
    return 0;
#else
    printf("Shutdown signal not supported on this platform.\n");
    return -1;
#endif
}


/* ================================================================
 * INSTANCE DETECTION
 * ================================================================ */

/**
 * Attempt to create the named mutex for single-instance detection.
 * Returns:
 *   0  - mutex created successfully (this is the first/only instance)
 *   1  - mutex already exists (another instance is running)
 *  -1  - error
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
 * DAEMON SPAWNER
 * ================================================================ */

/**
 * Spawn backup.exe --daemon as a detached background process.
 * The daemon runs with CREATE_NO_WINDOW - it never has a console.
 * This is called when the user double-clicks backup.exe and no daemon
 * is running yet. The caller then enters viewer mode.
 *
 * Returns 0 on success, -1 on failure.
 */
static int spawn_daemon(void) {
#ifdef _WIN32
    /*
     * Get the path to the current executable - we'll launch it again
     * with the --daemon flag.
     */
    char exe_path[MAX_URL_LEN];
    DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_URL_LEN);
    if (len == 0 || len >= MAX_URL_LEN) {
        return -1;
    }

    /*
     * Build command line: "exe_path" --daemon
     * The executable path is quoted to handle spaces.
     */
    char cmd_line[MAX_URL_LEN + 32];
    snprintf(cmd_line, sizeof(cmd_line), "\"%s\" --daemon", exe_path);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    /* No desktop, no window - completely headless */
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    BOOL created = CreateProcessA(
        NULL,               /* Application name (NULL = use cmd line) */
        cmd_line,           /* Command line: "backup.exe" --daemon */
        NULL,               /* Process security attributes */
        NULL,               /* Thread security attributes */
        FALSE,              /* Inherit handles */
        CREATE_NO_WINDOW,   /* Headless - no console window ever */
        NULL,               /* Use parent's environment */
        NULL,               /* Use parent's working directory */
        &si,                /* Startup info */
        &pi                 /* Process info (output) */
    );

    if (!created) {
        return -1;
    }

    /* Close handles - we don't need to track the daemon process */
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    /*
     * Brief pause to let the daemon initialize (create mutex, parse .env,
     * open log file). Without this, the viewer might start before the
     * daemon has created the log file.
     */
    Sleep(1000);

    return 0;
#else
    return -1;
#endif
}


/* ================================================================
 * VIEWER MODE
 * ================================================================ */

/**
 * Enter viewer mode. Tails the daemon's log file with ANSI color output.
 * The viewer polls for 'q' key press - pressing q signals the daemon
 * to shut down gracefully, then the viewer exits after a brief wait.
 *
 * Ctrl+C and closing the terminal kill only the viewer - the daemon
 * continues running unaffected.
 */
static void enter_viewer_mode(void) {
#ifdef _WIN32
    /* Initialize console for ANSI output */
    console_init();

    /* Find the log file path - need to parse .env for BACKUP_DIR */
    char exe_dir[MAX_URL_LEN];
    get_exe_dir(exe_dir, sizeof(exe_dir));

    backup_config cfg;
    memset(&cfg, 0, sizeof(backup_config));
    snprintf(cfg.backup_dir, sizeof(cfg.backup_dir), "%s", exe_dir);
    apply_defaults(&cfg);
    parse_env_file(&cfg);

    char log_path[MAX_URL_LEN];
    build_log_path(cfg.backup_dir, log_path);

    /* Enter the log viewer - blocks until q pressed, Ctrl+C, or window closed */
    console_log_viewer(log_path);

    console_cleanup();
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
        /* Check for shutdown request (shutdown event or --shutdown signal) */
        if (g_shutdown_requested) {
            log_event(LOG_INFO, "main", NULL, "STOPPING",
                      "Shutdown requested - exiting gracefully");
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
                      "No internet detected - cycle skipped");
            toast_info("No Internet",
                       "No internet detected - cycle skipped");
            sleep_with_shutdown_check(config->cycle_interval, config);
            continue;
        }

        /*
         * Step 2: Fresh config read every cycle.
         */
        backup_config cycle_config;
        memset(&cycle_config, 0, sizeof(backup_config));
        snprintf(cycle_config.backup_dir, sizeof(cycle_config.backup_dir),
                 "%s", exe_dir);

        fprintf(stderr, "[DBG] main_loop: Re-reading .env...\n");
        fflush(stderr);
        if (parse_env_file(&cycle_config) != 0) {
            log_error("main", NULL,
                      "Failed to parse .env - skipping this cycle");
            toast_error("Config Error",
                        "Failed to parse .env - skipping this cycle");
            sleep_with_shutdown_check(config->cycle_interval, config);
            continue;
        }

        /* Ensure BACKUP_DIR exists */
        if (ensure_dir_exists(cycle_config.backup_dir) != 0) {
            log_error("main", NULL,
                      "Cannot create or access BACKUP_DIR - skipping this cycle");
            toast_error("Directory Error",
                        "Cannot create BACKUP_DIR - check permissions");
            sleep_with_shutdown_check(config->cycle_interval, config);
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
        sleep_with_shutdown_check(cycle_config.cycle_interval, &cycle_config);
    }
}


/* ================================================================
 * DAEMON MODE ENTRY POINT
 * ================================================================ */

/**
 * Run as the headless daemon. This is the backup worker process.
 * It has no console - all output goes to the log file and toasts.
 * The daemon must already own the mutex when this function is called.
 *
 * Returns 0 on clean exit, 1 on startup failure.
 */
static int run_daemon(void) {
    /*
     * Step 1: Determine where the executable lives.
     */
    char exe_dir[MAX_URL_LEN];
    fprintf(stderr, "[DBG] main: Resolving exe directory...\n");
    fflush(stderr);
    get_exe_dir(exe_dir, sizeof(exe_dir));

    if (exe_dir[0] == '\0') {
        fprintf(stderr, "Error: Cannot determine executable directory\n");
        toast_error("Startup Error",
                    "Cannot determine executable directory - exiting");
        notify_cleanup();
        return 1;
    }

    /*
     * Step 2: Find and validate .env - it sits next to the exe.
     */
    char env_path[MAX_URL_LEN];
    build_env_path(exe_dir, env_path);
    fprintf(stderr, "[DBG] main: Validating .env at %s\n", env_path);
    fflush(stderr);

    if (validate_env_exists(env_path) != 0) {
        toast_error("Config Error",
                    "Config file (.env) not found next to executable - exiting");
        notify_cleanup();
        return 1;
    }

    /*
     * Step 3: Parse .env.
     */
    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", exe_dir);

    fprintf(stderr, "[DBG] main: Parsing .env...\n");
    fflush(stderr);
    if (parse_env_file(&config) != 0) {
        toast_error("Config Error",
                    "Config validation failed - exiting (requires manual intervention)");
        notify_cleanup();
        return 1;
    }

    fprintf(stderr, "[DBG] main: Parsed %d repos, owner=%s, timeout=%dms, cycle=%ds\n",
            config.repo_count, config.owner, config.http_timeout, config.cycle_interval);
    fflush(stderr);

    /*
     * Step 4: Create BACKUP_DIR if it doesn't exist.
     */
    fprintf(stderr, "[DBG] main: Ensuring BACKUP_DIR exists: %s\n", config.backup_dir);
    fflush(stderr);
    if (ensure_dir_exists(config.backup_dir) != 0) {
        toast_error("Directory Error",
                    "Cannot create BACKUP_DIR - check permissions and path");
        notify_cleanup();
        return 1;
    }

    /*
     * Step 5: Initialize the log file in BACKUP_DIR.
     */
    char log_path[MAX_URL_LEN];
    build_log_path(config.backup_dir, log_path);
    fprintf(stderr, "[DBG] main: Initializing log file at %s\n", log_path);
    fflush(stderr);
    if (log_init(log_path) != 0) {
        toast_error("Log Error",
                    "Cannot open log file - continuing without file logging");
    }

    /*
     * Step 6: Initialize the network session.
     */
    fprintf(stderr, "[DBG] main: Initializing network (WinHTTP)...\n");
    fflush(stderr);
    if (network_init() != 0) {
        log_error("startup", NULL,
                  "Network initialization failed - cannot proceed");
        toast_error("Network Error",
                    "Failed to initialize HTTP session - exiting");
        log_close();
        notify_cleanup();
        return 1;
    }

    /*
     * Step 7: Create the shutdown event.
     * This event is checked during sleep intervals and by --shutdown / q key.
     */
#ifdef _WIN32
    fprintf(stderr, "[DBG] main: Creating shutdown event...\n");
    fflush(stderr);
    g_shutdown_event = CreateEventA(NULL, TRUE, FALSE,
                                   BACKUP_SHUTDOWN_EVENT_NAME);
    if (g_shutdown_event == NULL) {
        log_event(LOG_WARNING, "main", NULL, "EVENT_WARN",
                  "Cannot create shutdown event - --shutdown will not work");
    }
#endif

    /*
     * Step 8: Fire "service started" toast.
     */
    log_event(LOG_INFO, "main", NULL, "STARTED",
              "GitHub Backup daemon started successfully");
    toast_info("Service Started",
               "GitHub Backup daemon is running in the background");

    /*
     * Step 9: Enter the main backup loop.
     */
    run_main_loop(&config, exe_dir);

    /*
     * Cleanup.
     */
    log_event(LOG_INFO, "main", NULL, "STOPPED",
              "GitHub Backup daemon shutting down");
    log_close();
    network_cleanup();
    notify_cleanup();

#ifdef _WIN32
    if (g_shutdown_event) CloseHandle(g_shutdown_event);
    if (g_backup_mutex) CloseHandle(g_backup_mutex);
#endif

    return 0;
}


/* ================================================================
 * ENTRY POINT
 * ================================================================ */

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /*
     * Step 1: Parse command-line arguments.
     * Check for --daemon and --shutdown flags.
     */
    int want_daemon = 0;
    int want_shutdown = 0;

    fprintf(stderr, "[DBG] main: Startup - parsing arguments\n");
    fflush(stderr);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--daemon") == 0) {
            want_daemon = 1;
        } else if (strcmp(argv[i], "--shutdown") == 0) {
            want_shutdown = 1;
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
     * Step 3: Handle --shutdown early - just signal and exit.
     */
    if (want_shutdown) {
        int result = signal_shutdown();
        notify_cleanup();
        return (result == 0) ? 0 : 1;
    }

    /*
     * Step 4: Check for single instance via named mutex.
     */
    fprintf(stderr, "[DBG] main: Checking single instance (mutex)...\n");
    fflush(stderr);
    int instance_result = check_single_instance();

    if (want_daemon) {
        /*
         * DAEMON MODE: The daemon MUST be the first instance (create the mutex).
         * If the mutex already exists, another daemon is running - exit with error.
         */
        if (instance_result == 1) {
            fprintf(stderr, "Error: Another backup daemon is already running.\n");
            toast_error("Daemon Error",
                        "Another backup daemon is already running - exiting");
            notify_cleanup();
            return 1;
        }
        if (instance_result < 0) {
            fprintf(stderr, "Error: Cannot create mutex (daemon startup failed)\n");
            toast_error("Daemon Error",
                        "Cannot create mutex - exiting");
            notify_cleanup();
            return 1;
        }

        /* Mutex created successfully - we are the daemon. Run the backup loop. */
        int result = run_daemon();
        notify_cleanup();
        return result;
    }

    if (instance_result == 1) {
        /*
         * VIEWER MODE (daemon already running):
         * Mutex exists - the daemon is running. Enter viewer mode directly.
         */
        enter_viewer_mode();
        notify_cleanup();
        return 0;
    }

    if (instance_result == 0) {
        /*
         * FIRST LAUNCH (no daemon running):
         * We created the mutex, but we're not the daemon. We need to:
         *   1. Release the mutex
         *   2. Spawn backup.exe --daemon as a detached background process
         *   3. Enter viewer mode
         */
#ifdef _WIN32
        if (g_backup_mutex) {
            CloseHandle(g_backup_mutex);
            g_backup_mutex = NULL;
        }
#endif

        if (spawn_daemon() != 0) {
            fprintf(stderr, "Error: Failed to spawn backup daemon.\n");
            toast_error("Startup Error",
                        "Failed to start backup daemon - check permissions");
            notify_cleanup();
            return 1;
        }

        /* Daemon is now running. Enter viewer mode. */
        enter_viewer_mode();
        notify_cleanup();
        return 0;
    }

    /* Mutex check error */
    fprintf(stderr, "Error: Cannot check single instance (mutex creation failed)\n");
    notify_cleanup();
    return 1;
}
