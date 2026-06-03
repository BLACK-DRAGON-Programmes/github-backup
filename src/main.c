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
 * modules built in Stages 1-5:
 *   - config:  loads settings from .env
 *   - logger:  initializes and manages the log file
 *   - notify:  initializes COM for toast notifications
 *   - network: checks internet connectivity
 *   - backup:  runs the per-repo backup cycle
 *
 * Startup sequence:
 *   1. Initialize COM (notify_init)
 *   2. Determine where the exe lives (get_exe_dir)
 *   3. Find .env next to the exe, parse it
 *   4. Create BACKUP_DIR if it doesn't exist
 *   5. Initialize log file in BACKUP_DIR
 *   6. Initialize network session
 *   7. Fire "service started" toast
 *   8. Enter main loop
 *
 * Main loop:
 *   1. Check internet connectivity
 *   2. If no internet: toast + log + sleep + loop
 *   3. Fire "cycle start" toast
 *   4. Parse .env (fresh read every cycle)
 *   5. Run backup cycle (iterate repos)
 *   6. Fire "cycle complete" toast
 *   7. Rotate log if needed
 *   8. Sleep for cycle interval
 *   9. Loop
 */

#include "constants.h"
#include "config.h"
#include "logger.h"
#include "notify.h"
#include "network.h"
#include "backup.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif


/* ================================================================
 * SLEEP HELPER
 * ================================================================ */

/**
 * Sleep for a specified number of seconds.
 *
 * @param seconds  Number of seconds to sleep (must be non-negative)
 */
static void sleep_seconds(int seconds) {
    if (seconds <= 0) {
        return;
    }

    #ifdef _WIN32
    Sleep((DWORD)seconds * 1000);
    #else
    sleep((unsigned int)seconds);
    #endif
}


/* ================================================================
 * STARTUP VALIDATION
 * ================================================================ */

/**
 * Perform pre-flight validation: verify .env exists and is readable.
 * Does NOT parse the file — just checks its presence.
 *
 * @param env_path  Path to the .env file
 * @return 0 if .env exists, -1 if missing or unreadable
 */
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

/**
 * Run the main backup loop. This function enters an infinite loop
 * that checks connectivity, runs backup cycles, and sleeps between
 * cycles. It only returns if the program is being shut down.
 *
 * @param config     The initial configuration (used for first cycle)
 * @param exe_dir    The exe's directory (used to re-find .env each cycle)
 */
static void run_main_loop(backup_config *config, const char *exe_dir) {
    for (;;) {
        /*
         * Step 1: Check internet connectivity.
         * If no internet, skip the cycle and sleep.
         */
        if (!check_connectivity(config->connectivity_timeout)) {
            log_event(LOG_WARNING, "main", NULL, "SKIPPED",
                      "No internet detected — cycle skipped");
            toast_info("No Internet",
                       "No internet detected — cycle skipped");
            sleep_seconds(config->cycle_interval);
            continue;
        }

        /*
         * Step 2: Fresh config read every cycle.
         * The spec says .env is re-read on every execution cycle so
         * that changes take effect without restarting the program.
         *
         * Set backup_dir to exe_dir so parse_env_file can find .env
         * next to the exe. After parsing, backup_dir will hold the
         * BACKUP_DIR value from .env (or remain exe_dir if not set).
         */
        backup_config cycle_config;
        memset(&cycle_config, 0, sizeof(backup_config));
        strncpy(cycle_config.backup_dir, exe_dir, MAX_URL_LEN - 1);
        cycle_config.backup_dir[MAX_URL_LEN - 1] = '\0';

        if (parse_env_file(&cycle_config) != 0) {
            log_error("main", NULL,
                      "Failed to parse .env — skipping this cycle");
            toast_error("Config Error",
                        "Failed to parse .env — skipping this cycle");
            sleep_seconds(config->cycle_interval);
            continue;
        }

        /*
         * Ensure BACKUP_DIR exists before starting the cycle.
         * It may have been deleted between cycles, or the user may
         * have changed BACKUP_DIR in .env to a new path.
         */
        if (ensure_dir_exists(cycle_config.backup_dir) != 0) {
            log_error("main", NULL,
                      "Cannot create or access BACKUP_DIR — skipping this cycle");
            toast_error("Directory Error",
                        "Cannot create BACKUP_DIR — check permissions");
            sleep_seconds(config->cycle_interval);
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
         * Decision 003: delete and start fresh (ephemeral log).
         */
        rotate_log(cycle_config.log_max_size);

        /*
         * Step 7: Sleep until the next cycle.
         */
        log_event(LOG_INFO, "main", NULL, "SLEEP",
                  "Sleeping until next cycle");
        sleep_seconds(cycle_config.cycle_interval);
    }
}


/* ================================================================
 * ENTRY POINT
 * ================================================================ */

int main(void) {
    /*
     * Step 1: Initialize notification subsystem (COM on Windows).
     * Must happen first so that startup errors can fire toasts.
     */
    if (notify_init() != 0) {
        fprintf(stderr, "Warning: Toast notifications unavailable\n");
    }

    /*
     * Step 2: Determine where this executable lives.
     * Everything else follows from this — .env is found next to the exe,
     * BACKUP_DIR is relative to the exe (or set explicitly in .env).
     */
    char exe_dir[MAX_URL_LEN];
    get_exe_dir(exe_dir, sizeof(exe_dir));

    if (exe_dir[0] == '\0') {
        fprintf(stderr, "Error: Cannot determine executable directory\n");
        notify_cleanup();
        return 1;
    }

    fprintf(stderr, "Info: Exe directory: %s\n", exe_dir);

    /*
     * Step 3: Find and validate .env — it sits next to the exe.
     */
    char env_path[MAX_URL_LEN];
    build_env_path(exe_dir, env_path);

    if (validate_env_exists(env_path) != 0) {
        toast_error("Config Error",
                    "Config file (.env) not found next to executable — exiting");
        notify_cleanup();
        return 1;
    }

    /*
     * Step 4: Parse .env.
     * Set backup_dir to exe_dir so parse_env_file can find .env.
     * After parsing, backup_dir will be set to BACKUP_DIR from .env
     * (or remain exe_dir if BACKUP_DIR was not specified).
     */
    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    strncpy(config.backup_dir, exe_dir, MAX_URL_LEN - 1);
    config.backup_dir[MAX_URL_LEN - 1] = '\0';

    if (parse_env_file(&config) != 0) {
        fprintf(stderr, "Error: Config validation failed — exiting\n");
        toast_error("Config Error",
                    "Config validation failed — exiting (requires manual intervention)");
        notify_cleanup();
        return 1;
    }

    fprintf(stderr, "Info: BACKUP_DIR: %s\n", config.backup_dir);

    /*
     * Step 5: Create BACKUP_DIR if it doesn't exist.
     * The program should never require manual directory creation —
     * it handles its own setup.
     */
    if (ensure_dir_exists(config.backup_dir) != 0) {
        fprintf(stderr, "Error: Cannot create BACKUP_DIR: %s\n", config.backup_dir);
        toast_error("Directory Error",
                    "Cannot create BACKUP_DIR — check permissions and path");
        notify_cleanup();
        return 1;
    }

    /*
     * Step 6: Initialize the log file in BACKUP_DIR.
     */
    char log_path[MAX_URL_LEN];
    build_log_path(config.backup_dir, log_path);

    if (log_init(log_path) != 0) {
        toast_error("Log Error",
                    "Cannot open log file — continuing without file logging");
        /* Continue anyway — stderr fallback is already active */
    }

    /*
     * Step 7: Initialize the network session.
     */
    if (network_init() != 0) {
        log_error("startup", NULL,
                  "Network initialization failed — cannot proceed");
        toast_error("Network Error",
                    "Failed to initialize HTTP session — exiting");
        log_close();
        notify_cleanup();
        return 1;
    }

    /*
     * Step 8: Fire "service started" toast.
     */
    log_event(LOG_INFO, "main", NULL, "STARTED",
              "GitHub Backup service started successfully");
    toast_info("Service Started",
               "GitHub Backup service is running in the background");

    /*
     * Step 9: Enter the main backup loop.
     * This function never returns under normal operation.
     */
    run_main_loop(&config, exe_dir);

    /*
     * Cleanup (only reached if the loop exits, which should
     * never happen under normal operation).
     */
    log_event(LOG_INFO, "main", NULL, "STOPPED",
              "GitHub Backup service shutting down");
    log_close();
    network_cleanup();
    notify_cleanup();

    return 0;
}
