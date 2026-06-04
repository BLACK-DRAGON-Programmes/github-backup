/**
 * notify.c — Windows toast notification implementation.
 *
 * Uses a PowerShell bridge to display actual Windows toast notifications
 * from C code. The Windows Runtime toast API requires C++ WRL headers
 * which cannot be included in a C compilation unit. Instead, this module
 * writes a minimal .ps1 script to the temp directory and executes it
 * via CreateProcessW with CREATE_NO_WINDOW (fire-and-forget, non-blocking).
 *
 * Each toast function also logs the event through the logger module for
 * dual output. Toasts are the primary user-visible feedback mechanism in
 * background mode (Task Scheduler / FreeConsole).
 *
 * COM threading model: Single-Threaded Apartment (STA), required by
 * the Windows Runtime toast API.
 */

#include "notify.h"
#include "logger.h"
#include "console.h"

#ifdef _WIN32

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <ole2.h>

#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif


/** Track whether COM was successfully initialized. */
static int g_com_initialized = 0;


/**
 * Escape a string for safe inclusion in XML attribute values.
 * Converts &, <, >, " and ' to their XML entity equivalents.
 * Output buffer must be at least 6x the input length (worst case: all quotes).
 */
static void xml_escape(const char *in, char *out, int max_len) {
    int i = 0;
    while (*in && i < max_len - 6) {
        switch (*in) {
            case '&':  memcpy(out + i, "&amp;", 5);  i += 5; break;
            case '<':  memcpy(out + i, "&lt;", 4);   i += 4; break;
            case '>':  memcpy(out + i, "&gt;", 4);   i += 4; break;
            case '"':  memcpy(out + i, "&quot;", 6); i += 6; break;
            case '\'': memcpy(out + i, "&apos;", 6); i += 6; break;
            default:   out[i++] = *in; break;
        }
        in++;
    }
    out[i] = '\0';
}


/**
 * Show a Windows toast notification via PowerShell bridge.
 *
 * Writes a temporary .ps1 file that uses the WinRT toast API to display
 * a notification, then executes it via CreateProcessW with CREATE_NO_WINDOW.
 * The process runs asynchronously — we fire and forget without waiting.
 *
 * The temp .ps1 file is cleaned up after the process completes. Since we
 * don't wait, we rely on the temp directory cleanup mechanisms. In practice,
 * the file is only a few hundred bytes and Windows cleans %TEMP% periodically.
 *
 * @param title   Toast title text
 * @param message Toast body text
 */
static void show_toast_powershell(const char *title, const char *message) {
    if (!g_com_initialized) return;

    /*
     * Escape title and message for XML embedding.
     * The PowerShell script builds an XML template for the toast.
     */
    char safe_title[512];
    char safe_message[1024];
    xml_escape(title, safe_title, (int)sizeof(safe_title));
    xml_escape(message, safe_message, (int)sizeof(safe_message));

    /*
     * Build a temporary .ps1 file path in %TEMP%.
     * Uses GetCurrentProcessId() for uniqueness so multiple instances
     * don't collide.
     */
    char temp_dir[MAX_PATH_BUF];
    char ps1_path[MAX_PATH_BUF];

    if (GetTempPathA((DWORD)sizeof(temp_dir), temp_dir) == 0) {
        return;
    }

    snprintf(ps1_path, sizeof(ps1_path),
             "%sghb-toast-%lu.ps1",
             temp_dir, (unsigned long)GetCurrentProcessId());

    /*
     * Write the PowerShell script.
     * Uses WinRT toast API via [Windows.UI.Notifications] types.
     *
     * Critical requirements for desktop app toasts (researched):
     *   1. AppUserModelID MUST be the pre-registered PowerShell AUMID.
     *      A made-up string like 'GitHub Backup' causes SILENT failure —
     *      no error, no notification. The PowerShell AUMID is guaranteed to
     *      exist on all Windows 10/11 installations.
     *   2. Template MUST be ToastGeneric. ToastText02 is deprecated and
     *      may not render on newer Windows builds.
     *   3. Audio silent="true" prevents the default notification sound.
     *   4. Duration="short" gives auto-dismiss behavior matching spec.
     *
     * Implementation note: The XML is built using PowerShell single-quoted
     * strings. Single-quoted strings in PowerShell treat all characters as
     * literal — no escaping needed for double-quotes inside them. This avoids
     * complex C escape sequences (\\\" \\\"  multi-line concatenation) that
     * caused compilation errors in previous versions. The xml_escape function
     * converts ' to &apos; so title/message never contain raw single quotes
     * that would break the PowerShell string delimiters.
     */
    FILE *fp = fopen(ps1_path, "w");
    if (!fp) return;

    fprintf(fp,
        "# GitHub Backup toast notification\n"
        "try {\n"
        "    [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null\n"
        "    [Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom, ContentType = WindowsRuntime] | Out-Null\n"
        "    $APP_ID = '{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\WindowsPowerShell\\v1.0\\powershell.exe'\n"
        "    $xml = New-Object Windows.Data.Xml.Dom.XmlDocument\n"
        "    $toastXml = '<toast duration=\"short\">"
        "<visual><binding template=\"ToastGeneric\">"
        "<text>%s</text>"
        "<text>%s</text>"
        "</binding></visual>"
        "<audio silent=\"true\"/></toast>'\n"
        "    $xml.LoadXml($toastXml)\n"
        "    $toast = [Windows.UI.Notifications.ToastNotification]::new($xml)\n"
        "    $notifier = [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($APP_ID)\n"
        "    $notifier.Show($toast)\n"
        "} catch { }\n",
        safe_title, safe_message);

    fclose(fp);

    /*
     * Execute the script via CreateProcessW.
     * - powershell.exe (5.1): WinRT types are pre-loaded. Do NOT use pwsh.exe.
     * - NoProfile: Skip user profile loading (fast startup).
     * - NonInteractive: Prevent interactive prompts that could hang.
     * - ExecutionPolicy Bypass: Allow unsigned temp scripts.
     * - WindowStyle Hidden: No console window flash.
     * - CREATE_NEW_CONSOLE with SW_HIDE: Required for toast delivery.
     *   CREATE_NO_WINDOW causes silent toast failure on some systems.
     */
    char cmd[MAX_PATH_BUF * 2];
    snprintf(cmd, sizeof(cmd),
             "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass "
             "-WindowStyle Hidden -File \"%s\"",
             ps1_path);

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    wchar_t wcmd[4096];
    MultiByteToWideChar(CP_ACP, 0, cmd, -1, wcmd, (int)(sizeof(wcmd) / sizeof(wchar_t)));

    BOOL created = CreateProcessW(
        NULL,           /* Application name (NULL = use cmd line) */
        wcmd,           /* Command line */
        NULL,           /* Process security attributes */
        NULL,           /* Thread security attributes */
        FALSE,          /* Inherit handles */
        CREATE_NEW_CONSOLE, /* Required: CREATE_NO_WINDOW causes silent toast failure */
        NULL,           /* Use parent's environment */
        NULL,           /* Use parent's working directory */
        &si,            /* Startup info */
        &pi             /* Process info (output) */
    );

    if (created) {
        /*
         * Wait for the PowerShell process to finish before deleting
         * the temp script. Without waiting, there's a race condition where
         * the C program deletes the .ps1 file before PowerShell reads it.
         * 15-second timeout prevents hangs if PowerShell freezes.
         */
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    /*
     * Delete the temp script AFTER the PowerShell process has finished.
     * This eliminates the race condition that existed when we deleted
     * immediately after CreateProcessW.
     */
    remove(ps1_path);
}


int notify_init(void) {
    fprintf(stderr, "[DBG] notify: Initializing COM (STA)...\n");
    fflush(stderr);
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[DBG] notify: COM init FAILED (hr=0x%lx)\n", (unsigned long)hr);
        fflush(stderr);
        log_error("notify", NULL, "COM initialization failed");
        return -1;
    }
    g_com_initialized = 1;
    fprintf(stderr, "[DBG] notify: COM initialized successfully\n");
    fflush(stderr);
    return 0;
}


void toast_info(const char *title, const char *message) {
    fprintf(stderr, "[DBG] notify: [INFO]  '%s' — '%s'\n", title, message);
    fflush(stderr);
    log_event(LOG_INFO, "toast", NULL, "INFO", message);
    #ifdef _WIN32
    show_toast_powershell(title, message);
    #endif
}


void toast_success(const char *repo, const char *message) {
    fprintf(stderr, "[DBG] notify: [OK]     '%s' — '%s'\n", repo, message);
    fflush(stderr);
    log_event(LOG_SUCCESS, "toast", repo, "OK", message);
    #ifdef _WIN32
    char title[512];
    snprintf(title, sizeof(title), "Backup OK: %s", repo);
    show_toast_powershell(title, message);
    #endif
}


void toast_error(const char *title, const char *message) {
    fprintf(stderr, "[DBG] notify: [ERROR] '%s' — '%s'\n", title, message);
    fflush(stderr);
    log_event(LOG_ERROR, "toast", NULL, "FAILED", message);
    #ifdef _WIN32
    show_toast_powershell(title, message);
    #endif
}


void notify_cleanup(void) {
    fprintf(stderr, "[DBG] notify: Cleanup — CoUninitialize\n");
    fflush(stderr);
    if (g_com_initialized) {
        CoUninitialize();
        g_com_initialized = 0;
    }
}

#else

#include <stddef.h>

/* ─── Non-Windows stubs (for compilation testing on Linux) ──── */

int notify_init(void) {
    log_event(LOG_INFO, "notify", NULL, "INFO",
              "Toast notifications disabled (non-Windows platform)");
    return 0;
}

void toast_info(const char *title, const char *message) {
    (void)title;
    log_event(LOG_INFO, "toast", NULL, "INFO", message);
}

void toast_success(const char *repo, const char *message) {
    log_event(LOG_SUCCESS, "toast", repo, "OK", message);
}

void toast_error(const char *title, const char *message) {
    (void)title;
    log_event(LOG_ERROR, "toast", NULL, "FAILED", message);
}

void notify_cleanup(void) {
    /* No-op on non-Windows */
}

#endif
