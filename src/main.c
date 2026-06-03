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
 * Startup sequence (per spec Section 3-4):
 *   1. Initialize COM (notify_init)
 *   2. Load config with defaults (apply_defaults runs first)
 *   3. Build .env path, verify it exists
 *   4. Parse .env, validate mandatory fields
 *   5. Initialize log file
 *   6. Initialize network session
 *   7. Fire "service started" toast
 *   8. Enter main loop
 *
 * Main loop (per spec Section 3-5):
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
 *
 * Platform-independent sleep function. Uses Windows Sleep() on
 * Windows and POSIX sleep() on other platforms.
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
 *
 * Validates that the .env file exists and contains the mandatory
 * fields before entering the main loop. Per Coding Standard #34
 * (Fail-Fast on Startup): if anything is wrong, the program exits
 * immediately with a specific error message.
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
        log_error("startup", NULL, "Config file not found");
        toast_error("Config Error", "Config file (.env) not found — exiting");
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
 * @param config  The initial configuration (used for first cycle)
 */
static void run_main_loop(backup_config *config) {
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
         */
        backup_config cycle_config;
        memset(&cycle_config, 0, sizeof(backup_config));
        strncpy(cycle_config.backup_dir, config->backup_dir,
                MAX_URL_LEN - 1);

        if (parse_env_file(&cycle_config) != 0) {
            /*
             * Config parse failed. This could mean the .env was
             * modified and is now corrupt. Log the error, wait
             * for the next cycle (don't exit — the previous
             * .env content may be restored).
             */
            log_error("main", NULL,
                      "Failed to parse .env — skipping this cycle");
            toast_error("Config Error",
                        "Failed to parse .env — skipping this cycle");
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
         * This iterates over all repos and attempts backup for each.
         */
        int succeeded = 0;
        int failed = 0;

        int cycle_result = run_backup_cycle(&cycle_config,
                                            &succeeded, &failed);

        /*
         * Step 5: Fire cycle-complete toast with summary.
         * If the cycle was aborted (disk full), include that in
         * the toast message.
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
        /*
         * COM init failed. The program can still run — toasts will
         * be silently skipped, but logging still works.
         */
        fprintf(stderr, "Warning: Toast notifications unavailable\n");
    }

    /*
     * Step 2: Load initial config with defaults.
     * This gives us the backup_dir to find the .env file.
     */
    backup_config config;
    memset(&config, 0, sizeof(backup_config));
    apply_defaults(&config);

    /*
     * Step 3: Initialize the log file early so that all subsequent
     * log events (including config parsing errors) are captured.
     * Uses the default backup_dir (apply_defaults ensures it's set).
     * After config is parsed, the log path may change — we re-init then.
     */
    char log_path[MAX_URL_LEN];
    build_log_path(config.backup_dir, log_path);
    log_init(log_path);

    /*
     * Step 4: Validate .env exists.
     * Fail-fast: if .env is missing, exit immediately.
     */
    char env_path[MAX_URL_LEN];
    build_env_path(config.backup_dir, env_path);

    if (validate_env_exists(env_path) != 0) {
        notify_cleanup();
        return 1;
    }

    /*
     * Step 5: Parse and validate .env.
     * If mandatory fields are missing, exit immediately.
     * Log events from parse_env_file are captured because log_init
     * was called in Step 3 (above).
     */
    if (parse_env_file(&config) != 0) {
        log_error("startup", NULL,
                  "Config validation failed — exiting");
        toast_error("Config Error",
                    "Config validation failed — exiting (requires manual intervention)");
        log_close();
        notify_cleanup();
        return 1;
    }

    /*
     * Step 6: Re-initialize the log file with the correct path
     * from the parsed config (BACKUP_DIR may differ from default).
     * Close the default log first, then open with the real path.
     */
    log_close();
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
     * Step 7: Fire "service started" toast.
     */
    log_event(LOG_INFO, "main", NULL, "STARTED",
              "GitHub Backup service started successfully");
    toast_info("Service Started",
               "GitHub Backup service is running in the background");

    /*
     * Step 8: Enter the main backup loop.
     * This function never returns under normal operation.
     */
    run_main_loop(&config);

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
