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
 *   backup.exe --register   → register Task Scheduler auto-start
 *   backup.exe --unregister → remove Task Scheduler auto-start
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
#include "context.h"

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
 * COMPOSITION ROOT — Real Implementations
 *
 * These static const structs wire the concrete implementations into
 * the ghb_context. The composition root (main.c) is the ONLY file
 * that knows about all the concrete implementations — every other
 * module depends only on the interface headers.
 *
 * To use a different backend (e.g., GitLab instead of GitHub),
 * replace the function pointers in network_ops with the alternative
 * implementation's functions. No changes needed in any other file.
 * ================================================================ */

static const logger_ops real_logger = {
    .log_event              = log_event,
    .log_error              = log_error,
    .log_init               = log_init,
    .log_close              = log_close,
    .rotate_log             = rotate_log,
};

static const notify_ops real_notify = {
    .toast_info     = toast_info,
    .toast_success  = toast_success,
    .toast_error    = toast_error,
    .notify_init    = notify_init,
    .notify_cleanup = notify_cleanup,
};

static const network_ops real_network = {
    .get_default_branch = get_default_branch,
    .download_repo_zip  = download_repo_zip,
    .check_connectivity = check_connectivity,
    .network_init       = network_init,
    .network_cleanup    = network_cleanup,
};


/* ================================================================
 * SHUTDOWN STATE
 * ================================================================ */

/**
 * Flag set by the SIGINT handler (Ctrl+C) to request graceful shutdown.
 * Checked at the start of each main loop iteration and between repos
 * in run_backup_cycle() via the should_stop function pointer.
 */
static volatile int g_shutdown_requested = 0;

#ifdef _WIN32
/** Handle to the named shutdown event. Checked by check_shutdown_requested()
 * (non-blocking poll) and sleep_with_shutdown_check() (blocking poll). */
static HANDLE g_shutdown_event = NULL;

/** Handle to the named mutex. Held for the lifetime of the daemon. */
static HANDLE g_backup_mutex = NULL;
#endif

/**
 * Shutdown check function for the DI context.
 * Returns non-zero when a graceful shutdown has been requested.
 * Wired into ghb_context.should_stop in main().
 *
 * Polls the named shutdown event (non-blocking) AND checks the flag.
 * The event is signaled by the viewer's 'q' key or --shutdown flag.
 * Without polling the event here, the flag is only updated during sleep
 * intervals, leaving downloads unresponsive to shutdown (R154 fix).
 */
static int check_shutdown_requested(void) {
#ifdef _WIN32
    /* Non-blocking poll of the named shutdown event.
     * WaitForSingleObject with 0 timeout returns immediately:
     *   WAIT_OBJECT_0 (0) = event is signaled
     *   WAIT_TIMEOUT (258) = event is not signaled
     *   WAIT_FAILED = error (treat as not signaled) */
    if (g_shutdown_event != NULL) {
        DWORD result = WaitForSingleObject(g_shutdown_event, 0);
        if (result == WAIT_OBJECT_0) {
            g_shutdown_requested = 1;
        }
    }
#endif
    return g_shutdown_requested;
}




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
 * TASK SCHEDULER REGISTRATION (Windows only)
 *
 * Spec Section 3: "Windows Task Scheduler. A scheduled task triggers
 * backup.exe --daemon at system startup (runs even if nobody is logged in)."
 *
 * Uses schtasks.exe to create/delete a scheduled task. The task runs
 * backup.exe --daemon at system startup with highest privileges.
 * ================================================================ */

/** Name of the scheduled task in Windows Task Scheduler. */
static const char *TASK_NAME = "GitHubBackupDaemon";

/**
 * Register a Windows Task Scheduler entry to launch backup.exe --daemon
 * at system startup. The task runs with highest privileges and does not
 * require a user to be logged in.
 *
 * @param ctx  Context for logging
 * @return 0 on success, -1 on failure
 */
static int register_task_scheduler(ghb_context *ctx) {
#ifdef _WIN32
    char exe_path[MAX_URL_LEN];
    DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_URL_LEN);
    if (len == 0 || len >= MAX_URL_LEN) {
        ctx->logger->log_error(ctx, "main", NULL,
                  "Cannot determine exe path for Task Scheduler registration");
        return -1;
    }

    /*
     * Build schtasks command:
     *   schtasks /create /tn "GitHubBackupDaemon" /tr "\"exe_path\" --daemon"
     *   /sc onstart /ru SYSTEM /rl HIGHEST /f
     *
     * /sc onstart   - trigger at system startup
     * /ru SYSTEM    - run as SYSTEM (works even when nobody is logged in)
     * /rl HIGHEST   - run with highest privileges
     * /f            - force create (overwrite if exists)
     *
     * The exe path is double-quoted inside the /tr argument to handle
     * paths containing spaces.
     */
    char cmd[MAX_URL_LEN * 2];
    snprintf(cmd, sizeof(cmd),
             "schtasks /create /tn \"%s\" /tr \"\\\"%s\\\" --daemon\" "
             "/sc onstart /ru SYSTEM /rl HIGHEST /f",
             TASK_NAME, exe_path);

    DBG("register_task_scheduler: Running: %s", cmd);

    int result = system(cmd);
    if (result != 0) {
        ctx->logger->log_error(ctx, "main", NULL,
                  "schtasks /create failed - Task Scheduler registration failed");
        fprintf(stderr, "Error: Failed to register Task Scheduler task.\n");
        fprintf(stderr, "Try running as administrator.\n");
        return -1;
    }

    ctx->logger->log_event(ctx, LOG_SUCCESS, "main", NULL, "OK",
              "Task Scheduler task registered - daemon will start at system startup");
    printf("Task Scheduler: Registered '%s' to run at system startup.\n", TASK_NAME);
    return 0;
#else
    (void)ctx;
    printf("Task Scheduler registration is only available on Windows.\n");
    return -1;
#endif
}

/**
 * Unregister the Windows Task Scheduler entry.
 *
 * @param ctx  Context for logging
 * @return 0 on success, -1 on failure
 */
static int unregister_task_scheduler(ghb_context *ctx) {
#ifdef _WIN32
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "schtasks /delete /tn \"%s\" /f", TASK_NAME);

    DBG("unregister_task_scheduler: Running: %s", cmd);

    int result = system(cmd);
    if (result != 0) {
        ctx->logger->log_error(ctx, "main", NULL,
                  "schtasks /delete failed - Task Scheduler unregistration failed");
        fprintf(stderr, "Error: Failed to unregister Task Scheduler task.\n");
        fprintf(stderr, "The task may not exist. Try running as administrator.\n");
        return -1;
    }

    ctx->logger->log_event(ctx, LOG_SUCCESS, "main", NULL, "OK",
              "Task Scheduler task removed - daemon will no longer start at system startup");
    printf("Task Scheduler: Unregistered '%s'.\n", TASK_NAME);
    return 0;
#else
    (void)ctx;
    printf("Task Scheduler unregistration is only available on Windows.\n");
    return -1;
#endif
}

/**
 * Check whether the Task Scheduler task is registered.
 *
 * @param ctx  Context for logging
 * @return 1 if registered, 0 if not registered, -1 on error
 */
static int is_task_scheduler_registered(ghb_context *ctx) {
#ifdef _WIN32
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "schtasks /query /tn \"%s\" >nul 2>&1", TASK_NAME);

    int result = system(cmd);
    if (result == 0) {
        ctx->logger->log_event(ctx, LOG_INFO, "main", NULL, "OK",
                  "Task Scheduler task is registered");
        return 1;
    }
    return 0;
#else
    (void)ctx;
    return 0;
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
static void enter_viewer_mode(ghb_context *ctx) {
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
    parse_env_file(ctx, &cfg);

    char log_path[MAX_URL_LEN];
    build_log_path(ctx, cfg.backup_dir, log_path);

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

static void run_main_loop(ghb_context *ctx, backup_config *config, const char *exe_dir) {
    for (;;) {
        /* Check for shutdown request (shutdown event or --shutdown signal) */
        if (g_shutdown_requested) {
            ctx->logger->log_event(ctx, LOG_INFO, "main", NULL, "STOPPING",
                      "Shutdown requested - exiting gracefully");
            ctx->notify->toast_info(ctx, "Shutting Down",
                       "GitHub Backup is shutting down gracefully");
            break;
        }

        /*
         * Step 1: Check internet connectivity.
         * If no internet, skip the cycle and sleep.
         */
        DBG("main_loop: Checking connectivity...");
        if (!ctx->network->check_connectivity(ctx, config->connectivity_timeout)) {
            ctx->logger->log_event(ctx, LOG_WARNING, "main", NULL, "SKIPPED",
                      "No internet detected - cycle skipped");
            ctx->notify->toast_info(ctx, "No Internet",
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

        DBG("main_loop: Re-reading .env...");
        if (parse_env_file(ctx, &cycle_config) != 0) {
            ctx->logger->log_error(ctx, "main", NULL,
                      "Failed to parse .env - corrupt file requires manual intervention - exiting");
            ctx->notify->toast_error(ctx, "Config Error",
                        ".env file is corrupt or missing - exiting (requires manual intervention)");
            break;
        }

        /* Ensure BACKUP_DIR exists */
        if (ensure_dir_exists(cycle_config.backup_dir) != 0) {
            ctx->logger->log_error(ctx, "main", NULL,
                      "Cannot create or access BACKUP_DIR - skipping this cycle");
            ctx->notify->toast_error(ctx, "Directory Error",
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
        ctx->logger->log_event(ctx, LOG_INFO, "main", NULL, "CYCLE_START", start_msg);
        ctx->notify->toast_info(ctx, "Backup Cycle", start_msg);

        /*
         * Step 4: Run the backup cycle.
         */
        int succeeded = 0;
        int failed = 0;

        int cycle_result = run_backup_cycle(ctx, &cycle_config,
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
        ctx->logger->log_event(ctx, LOG_INFO, "main", NULL, "CYCLE_COMPLETE", complete_msg);
        ctx->notify->toast_info(ctx, "Cycle Complete", complete_msg);

        /*
         * Step 6: Rotate log if it exceeds the size threshold.
         */
        ctx->logger->rotate_log(ctx, cycle_config.log_max_size);

        /*
         * Step 7: Sleep until the next cycle (with shutdown polling).
         */
        ctx->logger->log_event(ctx, LOG_INFO, "main", NULL, "SLEEP",
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
static int run_daemon(ghb_context *ctx) {
    /*
     * Step 1: Determine where the executable lives.
     */
    char exe_dir[MAX_URL_LEN];
    DBG("main: Resolving exe directory...");
    get_exe_dir(exe_dir, sizeof(exe_dir));

    if (exe_dir[0] == '\0') {
        fprintf(stderr, "Error: Cannot determine executable directory\n");
        ctx->notify->toast_error(ctx, "Startup Error",
                    "Cannot determine executable directory - exiting");
        ctx->notify->notify_cleanup(ctx);
        return 1;
    }

    /*
     * Step 2: Find and validate .env - it sits next to the exe.
     */
    char env_path[MAX_URL_LEN];
    build_env_path(ctx, exe_dir, env_path);
    DBG("main: Validating .env at %s", env_path);

    if (validate_env_exists(env_path) != 0) {
        ctx->notify->toast_error(ctx, "Config Error",
                    "Config file (.env) not found next to executable - exiting");
        ctx->notify->notify_cleanup(ctx);
        return 1;
    }

    /*
     * Step 3: Parse .env.
     */
    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    snprintf(config.backup_dir, sizeof(config.backup_dir), "%s", exe_dir);

    DBG("main: Parsing .env...");
    if (parse_env_file(ctx, &config) != 0) {
        ctx->notify->toast_error(ctx, "Config Error",
                    "Config validation failed - exiting (requires manual intervention)");
        ctx->notify->notify_cleanup(ctx);
        return 1;
    }

    DBG("main: Parsed %d repos, owner=%s, timeout=%dms, cycle=%ds",
            config.repo_count, config.owner, config.http_timeout, config.cycle_interval);

    /*
     * Step 4: Create BACKUP_DIR if it doesn't exist.
     */
    DBG("main: Ensuring BACKUP_DIR exists: %s", config.backup_dir);
    if (ensure_dir_exists(config.backup_dir) != 0) {
        ctx->notify->toast_error(ctx, "Directory Error",
                    "Cannot create BACKUP_DIR - check permissions and path");
        ctx->notify->notify_cleanup(ctx);
        return 1;
    }

    /*
     * Step 5: Initialize the log file in BACKUP_DIR.
     */
    char log_path[MAX_URL_LEN];
    build_log_path(ctx, config.backup_dir, log_path);
    DBG("main: Initializing log file at %s", log_path);
    if (ctx->logger->log_init(ctx, log_path) != 0) {
        ctx->notify->toast_error(ctx, "Log Error",
                    "Cannot open log file - continuing without file logging");
    }

    /*
     * DEV PHASE: Write immediate startup progress to the log file.
     * This lets the viewer show the daemon is alive BEFORE the main loop.
     * Without these, the viewer sees an empty log file for 30+ seconds
     * while the daemon runs connectivity checks and downloads.
     */
    ctx->logger->log_event(ctx, LOG_INFO, "daemon", NULL, "STARTUP",
              "Log file initialized - daemon startup in progress");

    /*
     * Step 6: Initialize the network session.
     */
    DBG("main: Initializing network (WinHTTP)...");
    if (ctx->network->network_init(ctx) != 0) {
        ctx->logger->log_error(ctx, "startup", NULL,
                  "Network initialization failed - cannot proceed");
        ctx->notify->toast_error(ctx, "Network Error",
                    "Failed to initialize HTTP session - exiting");
        ctx->logger->log_close(ctx);
        ctx->notify->notify_cleanup(ctx);
        return 1;
    }

    ctx->logger->log_event(ctx, LOG_INFO, "daemon", NULL, "STARTUP",
              "Network initialized - checking connectivity next");

    /*
     * Step 7: Create the shutdown event.
     * This event is checked during sleep intervals and by --shutdown / q key.
     */
#ifdef _WIN32
    DBG("main: Creating shutdown event...");
    g_shutdown_event = CreateEventA(NULL, TRUE, FALSE,
                                   BACKUP_SHUTDOWN_EVENT_NAME);
    if (g_shutdown_event == NULL) {
        ctx->logger->log_event(ctx, LOG_WARNING, "main", NULL, "EVENT_WARN",
                  "Cannot create shutdown event - --shutdown will not work");
    }
#endif

    /*
     * Step 8: Fire "service started" toast.
     */
    ctx->logger->log_event(ctx, LOG_INFO, "main", NULL, "STARTED",
              "GitHub Backup daemon started successfully");
    ctx->notify->toast_info(ctx, "Service Started",
               "GitHub Backup daemon is running in the background");

    /*
     * DEV PHASE: Log the first connectivity check immediately.
     * This gives the viewer something to show within seconds of startup.
     */
    ctx->logger->log_event(ctx, LOG_INFO, "daemon", NULL, "PROGRESS",
              "About to check internet connectivity...");

    /*
     * Step 9: Enter the main backup loop.
     */
    run_main_loop(ctx, &config, exe_dir);

    /*
     * Cleanup.
     */
    ctx->logger->log_event(ctx, LOG_INFO, "main", NULL, "STOPPED",
              "GitHub Backup daemon shutting down");
    ctx->logger->log_close(ctx);
    ctx->network->network_cleanup(ctx);
    ctx->notify->notify_cleanup(ctx);

#ifdef _WIN32
    if (g_shutdown_event) CloseHandle(g_shutdown_event);
    if (g_backup_mutex) CloseHandle(g_backup_mutex);
#endif

    return 0;
}


/* ================================================================
 * ENTRY POINT
 * ================================================================ */

#ifndef GHB_TEST_BUILD
int main(int argc, char *argv[]) {
    /*
     * Step 0: Create the dependency injection context.
     * Wire up the real implementations — this is the composition root.
     */
    ghb_context ctx;
    ctx.logger = &real_logger;
    ctx.notify = &real_notify;
    ctx.network = &real_network;
    ctx.should_stop = check_shutdown_requested;

    /*
     * Step 1: Parse command-line arguments.
     * Check for --daemon and --shutdown flags.
     */
    int want_daemon = 0;
    int want_shutdown = 0;
    int want_register = 0;
    int want_unregister = 0;
    int want_status = 0;

    DBG("main: Startup - parsing arguments");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--daemon") == 0) {
            want_daemon = 1;
        } else if (strcmp(argv[i], "--shutdown") == 0) {
            want_shutdown = 1;
        } else if (strcmp(argv[i], "--register") == 0) {
            want_register = 1;
        } else if (strcmp(argv[i], "--unregister") == 0) {
            want_unregister = 1;
        } else if (strcmp(argv[i], "--status") == 0) {
            want_status = 1;
        }
    }

    /*
     * Step 2: Initialize notification subsystem (COM on Windows).
     * Must happen first so that startup errors can fire toasts.
     */
    DBG("main: Initializing COM (notifications)...");
    if (ctx.notify->notify_init(&ctx) != 0) {
        fprintf(stderr, "Warning: Toast notifications unavailable\n");
    }

    /*
     * Step 3: Handle --shutdown early - just signal and exit.
     */
    if (want_shutdown) {
        int result = signal_shutdown();
        ctx.notify->notify_cleanup(&ctx);
        return (result == 0) ? 0 : 1;
    }

    /*
     * Step 3b: Handle --register and --unregister for Task Scheduler.
     * These are one-shot operations that exit immediately.
     * Spec Section 3: "Windows Task Scheduler. A scheduled task triggers
     * backup.exe --daemon at system startup."
     */
    if (want_register) {
        int result = register_task_scheduler(&ctx);
        ctx.notify->notify_cleanup(&ctx);
        return (result == 0) ? 0 : 1;
    }

    if (want_unregister) {
        int result = unregister_task_scheduler(&ctx);
        ctx.notify->notify_cleanup(&ctx);
        return (result == 0) ? 0 : 1;
    }

    if (want_status) {
        int result = is_task_scheduler_registered(&ctx);
        if (result == 1) {
            printf("Task Scheduler: REGISTERED (task: %s)\n", TASK_NAME);
        } else {
            printf("Task Scheduler: NOT REGISTERED\n");
        }
        ctx.notify->notify_cleanup(&ctx);
        return 0;
    }

    /*
     * Step 4: Check for single instance via named mutex.
     */
    DBG("main: Checking single instance (mutex)...");
    int instance_result = check_single_instance();

    if (want_daemon) {
        /*
         * DAEMON MODE: The daemon MUST be the first instance (create the mutex).
         * If the mutex already exists, another daemon is running - exit with error.
         */
        if (instance_result == 1) {
            fprintf(stderr, "Error: Another backup daemon is already running.\n");
            ctx.notify->toast_error(&ctx, "Daemon Error",
                        "Another backup daemon is already running - exiting");
            ctx.notify->notify_cleanup(&ctx);
            return 1;
        }
        if (instance_result < 0) {
            fprintf(stderr, "Error: Cannot create mutex (daemon startup failed)\n");
            ctx.notify->toast_error(&ctx, "Daemon Error",
                        "Cannot create mutex - exiting");
            ctx.notify->notify_cleanup(&ctx);
            return 1;
        }

        /* Mutex created successfully - we are the daemon. Run the backup loop. */
        int result = run_daemon(&ctx);
        ctx.notify->notify_cleanup(&ctx);
        return result;
    }

    if (instance_result == 1) {
        /*
         * VIEWER MODE (daemon already running):
         * Mutex exists - the daemon is running. Enter viewer mode directly.
         */
        enter_viewer_mode(&ctx);
        ctx.notify->notify_cleanup(&ctx);
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
            ctx.notify->toast_error(&ctx, "Startup Error",
                        "Failed to start backup daemon - check permissions");
            ctx.notify->notify_cleanup(&ctx);
            return 1;
        }

        /* Daemon is now running. Enter viewer mode. */
        enter_viewer_mode(&ctx);
        ctx.notify->notify_cleanup(&ctx);
        return 0;
    }

    /* Mutex check error */
    fprintf(stderr, "Error: Cannot check single instance (mutex creation failed)\n");
    ctx.notify->notify_cleanup(&ctx);
    return 1;
}
#endif /* GHB_TEST_BUILD */
