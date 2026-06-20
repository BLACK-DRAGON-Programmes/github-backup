/**
 * notify.c - Windows toast notification implementation.
 *
 * Uses a PowerShell bridge to display actual Windows toast notifications
 * from C code. The Windows Runtime toast API requires C++ WRL headers
 * which cannot be included in a C compilation unit. Instead, this module
 * writes a minimal .ps1 script to the temp directory and executes it
 * via CreateProcessW with CREATE_NO_WINDOW (fire-and-forget, non-blocking).
 *
 * Each toast function also logs the event through the logger module for
 * dual output. Toasts are the primary user-visible feedback mechanism in
 * daemon mode (headless background process).
 *
 * Toast click interaction (Spec Section 9):
 *   Clicking a toast notification launches backup.exe with no flags.
 *   Since the daemon is always running when toasts are fired, the mutex
 *   already exists and the new process enters viewer mode - the user
 *   sees a live log tail of backup activity.
 *
 * Implementation: The PowerShell script registers an Activated event
 * handler on the toast notification. When the user clicks the toast,
 * the handler invokes Start-Process to launch backup.exe.
 *
 * COM threading model: Single-Threaded Apartment (STA), required by
 * the Windows Runtime toast API.
 */

#include "notify.h"
#include "context.h"
#include "logger.h"  /* DBG macro — compile-time debug toggle, not a DI call */

#ifdef _WIN32

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>

#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

/** Custom AUMID for toast notifications (registered at runtime).
 * Per MS docs, desktop apps must register an AUMID under
 * HKCU\Software\Classes\AppUserModelId\ for toasts to appear.
 * Using PowerShell's built-in AUMID only works if the PS Start Menu
 * shortcut exists — unreliable. Custom AUMID is self-contained. */
#define TOAST_AUMID "GitHubBackup.Toast"
#define TOAST_DISPLAY_NAME "GitHub Backup"


/** Track whether COM was successfully initialized. */
static int g_com_initialized = 0;

/** Track whether the custom AUMID has been registered. */
static int g_aumid_registered = 0;


/**
 * Register a custom AppUserModelID (AUMID) in the Windows registry
 * so toast notifications appear. Per MS docs, desktop apps MUST
 * register an AUMID under HKCU\Software\Classes\AppUserModelId\.
 * Without this, CreateToastNotifier silently drops the toast.
 *
 * Needs only standard-user privileges (HKCU). No installer required.
 * Called once during notify_init().
 */
static void register_toast_aumid(void) {
    if (g_aumid_registered) return;

    HKEY hKey;
    char keyPath[256];
    snprintf(keyPath, sizeof(keyPath),
             "Software\\Classes\\AppUserModelId\\%s", TOAST_AUMID);

    LONG result = RegCreateKeyExA(HKEY_CURRENT_USER, keyPath, 0, NULL, 0,
                                  KEY_WRITE, NULL, &hKey, NULL);
    if (result != ERROR_SUCCESS) {
        DBG("notify: Failed to create AUMID registry key (error=%ld)", result);
        return;
    }

    /* DisplayName — shown on the toast and in notification settings */
    DWORD showInSettings = 1;
    RegSetValueExA(hKey, "DisplayName", 0, REG_SZ,
                   (const BYTE *)TOAST_DISPLAY_NAME,
                   (DWORD)strlen(TOAST_DISPLAY_NAME) + 1);
    RegSetValueExA(hKey, "ShowInSettings", 0, REG_DWORD,
                   (const BYTE *)&showInSettings, sizeof(showInSettings));

    RegCloseKey(hKey);
    g_aumid_registered = 1;
    DBG("notify: Custom AUMID registered: %s", TOAST_AUMID);
}


/**
 * Show a Windows toast notification via a static PowerShell script.
 *
 * Invokes toasts/show-toast.ps1 (located next to backup.exe) via
 * powershell.exe -File. The script takes -Title and -Message parameters,
 * shows the toast, and sleeps 2 seconds (fire-and-forget).
 *
 * R158 research findings (10 agents + compiled report):
 *   1. ROOT CAUSE #1: The old -EncodedCommand script used WinRT event
 *      handlers ([ToastActivatedEventHandler]::new({...})) which silently
 *      throw on PS 5.1. The try/catch swallowed the error and Show() was
 *      never reached. The static script has NO event handlers.
 *   2. ROOT CAUSE #2: CREATE_NEW_CONSOLE was wrong (R88 was empirically
 *      falsified). Using CREATE_NO_WINDOW per R158-10.
 *   3. ROOT CAUSE #3: The while/Start-Sleep loop didn't pump the STA COM
 *      queue. Replaced with a simple Start-Sleep -Seconds 2 (matches the
 *      working test-toast.ps1 pattern).
 *   4. -File is more reliable than -EncodedCommand (no encoding round-trip,
 *      no 32KB limit, real parse errors, not EDR-flagged). R158-02, R158-06.
 *
 * @param title   Toast title text
 * @param message Toast body text
 */
static void show_toast_powershell(const char *title, const char *message) {
    if (!g_com_initialized) return;
    if (!g_aumid_registered) register_toast_aumid();

    /*
     * Find the toast script — it lives in the toasts/ subdirectory
     * next to backup.exe. GetModuleFileNameA gives us the exe path;
     * we strip the exe filename and append "toasts\show-toast.ps1".
     */
    char exe_path[MAX_PATH_BUF];
    DWORD exe_len = GetModuleFileNameA(NULL, exe_path, MAX_PATH_BUF);
    if (exe_len == 0 || exe_len >= MAX_PATH_BUF) {
        DBG("notify: Cannot get exe path for toast script");
        return;
    }

    /* Strip the exe filename to get the directory */
    char *last_sep = NULL;
    for (DWORD i = 0; i < exe_len; i++) {
        if (exe_path[i] == '\\' || exe_path[i] == '/') {
            last_sep = &exe_path[i];
        }
    }
    if (last_sep == NULL) {
        DBG("notify: Cannot find directory separator in exe path");
        return;
    }
    *last_sep = '\0';

    char script_path[MAX_PATH_BUF];
    /* exe_path comes from GetModuleFileNameA which always returns < MAX_PATH.
     * GCC cannot prove this at compile time, so suppress -Wformat-truncation.
     * Runtime length check is defense-in-depth (Rule 23/45). */
    size_t exe_len_str = strlen(exe_path);
    if (exe_len_str + 24 >= sizeof(script_path)) {
        DBG("notify: Exe path too long for toast script path");
        return;
    }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(script_path, sizeof(script_path),
             "%s\\toasts\\show-toast.ps1", exe_path);
#pragma GCC diagnostic pop

    /* Verify the script exists */
    DWORD attr = GetFileAttributesA(script_path);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        DBG("notify: Toast script not found at %s", script_path);
        return;
    }

    /*
     * Escape title and message for PowerShell double-quoted parameters.
     * Double quotes inside the values must be escaped as \" (backslash-quote).
     * Backslashes themselves are fine in PS double-quoted strings.
     */
    char safe_title[512];
    char safe_message[1024];
    int ti = 0, mi = 0;
    for (const char *p = title; *p && ti < (int)sizeof(safe_title) - 4; p++) {
        if (*p == '"') { safe_title[ti++] = '\\'; safe_title[ti++] = '"'; }
        else { safe_title[ti++] = *p; }
    }
    safe_title[ti] = '\0';

    for (const char *p = message; *p && mi < (int)sizeof(safe_message) - 4; p++) {
        if (*p == '"') { safe_message[mi++] = '\\'; safe_message[mi++] = '"'; }
        else { safe_message[mi++] = *p; }
    }
    safe_message[mi] = '\0';

    /*
     * Build the command line:
     *   powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass
     *     -WindowStyle Hidden -File "<script>" -Title "<title>" -Message "<msg>"
     *
     * -NoProfile: skip user profile (fast startup)
     * -NonInteractive: no prompts
     * -ExecutionPolicy Bypass: allow unsigned scripts
     * -WindowStyle Hidden: no console window flash
     * -File: run the script file (more reliable than -EncodedCommand)
     */
    char cmd[MAX_PATH_BUF * 2 + 1600];
    snprintf(cmd, sizeof(cmd),
             "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass "
             "-WindowStyle Hidden -File \"%s\" -Title \"%s\" -Message \"%s\"",
             script_path, safe_title, safe_message);

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    wchar_t wcmd[8192];
    MultiByteToWideChar(CP_ACP, 0, cmd, -1, wcmd,
                        (int)(sizeof(wcmd) / sizeof(wchar_t)));

    /*
     * CREATE_NO_WINDOW (R158-10): the universal flag for toast delivery.
     * R88 claimed CREATE_NO_WINDOW caused silent failure, but that was
     * empirically falsified — the current code used CREATE_NEW_CONSOLE
     * and toasts still failed. The real root cause was the WinRT event
     * handlers in the script (now removed). R158-10 confirms CREATE_NO_WINDOW
     * is used by every working example.
     */
    BOOL created = CreateProcessW(
        NULL, wcmd, NULL, NULL, FALSE,
        CREATE_NO_WINDOW,
        NULL, NULL, &si, &pi
    );

    if (created) {
        DBG("notify: Toast process spawned (pid=%lu, script=%s)",
            (unsigned long)pi.dwProcessId, script_path);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        DBG("notify: CreateProcessW FAILED for toast (error=%lu)", GetLastError());
    }
}


int notify_init(ghb_context *ctx) {
    DBG("notify: Initializing COM (STA)...");
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        DBG("notify: COM init FAILED (hr=0x%lx)", (unsigned long)hr);
        ctx->logger->log_error(ctx, "notify", NULL, "COM initialization failed");
        return -1;
    }
    g_com_initialized = 1;
    DBG("notify: COM initialized successfully");

    /* Register custom AUMID for toast notifications (R154).
     * Without this, toasts are silently dropped by Windows. */
    register_toast_aumid();

    return 0;
}


void toast_info(ghb_context *ctx, const char *title, const char *message) {
    DBG("notify: [INFO]  '%s' - '%s'", title, message);
    ctx->logger->log_event(ctx, LOG_INFO, "toast", NULL, "INFO", message);
    #ifdef _WIN32
    show_toast_powershell(title, message);
    #endif
}


void toast_success(ghb_context *ctx, const char *repo, const char *message) {
    DBG("notify: [OK]     '%s' - '%s'", repo, message);
    ctx->logger->log_event(ctx, LOG_SUCCESS, "toast", repo, "OK", message);
    #ifdef _WIN32
    char title[512];
    snprintf(title, sizeof(title), "Backup OK: %s", repo);
    show_toast_powershell(title, message);
    #endif
}


void toast_error(ghb_context *ctx, const char *title, const char *message) {
    DBG("notify: [ERROR] '%s' - '%s'", title, message);
    ctx->logger->log_event(ctx, LOG_ERROR, "toast", NULL, "FAILED", message);
    #ifdef _WIN32
    show_toast_powershell(title, message);
    #endif
}


void notify_cleanup(ghb_context *ctx) {
    (void)ctx;  /* Cleanup uses only the global COM state flag */
    if (g_com_initialized) {
        DBG("notify: Cleanup - CoUninitialize");
        CoUninitialize();
        g_com_initialized = 0;
    }
}

#else

#include <stddef.h>

/* ─── Non-Windows stubs (for compilation testing on Linux) ──── */

int notify_init(ghb_context *ctx) {
    ctx->logger->log_event(ctx, LOG_INFO, "notify", NULL, "INFO",
              "Toast notifications disabled (non-Windows platform)");
    return 0;
}

void toast_info(ghb_context *ctx, const char *title, const char *message) {
    (void)title;
    ctx->logger->log_event(ctx, LOG_INFO, "toast", NULL, "INFO", message);
}

void toast_success(ghb_context *ctx, const char *repo, const char *message) {
    ctx->logger->log_event(ctx, LOG_SUCCESS, "toast", repo, "OK", message);
}

void toast_error(ghb_context *ctx, const char *title, const char *message) {
    (void)title;
    ctx->logger->log_event(ctx, LOG_ERROR, "toast", NULL, "FAILED", message);
}

void notify_cleanup(ghb_context *ctx) {
    (void)ctx;  /* No-op on non-Windows */
}

#endif
