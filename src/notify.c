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
 * a notification with an Activated event handler, then executes it via
 * CreateProcessW with CREATE_NO_WINDOW. The process runs asynchronously -
 * we fire and forget without waiting.
 *
 * The temp .ps1 file is cleaned up after the process completes.
 *
 * Toast click behavior (Spec Section 9):
 *   The PowerShell script registers an Activated event handler via
 *   ToastNotification.Add_Activated(). When the user clicks the toast,
 *   the handler invokes Start-Process to launch backup.exe (no flags).
 *   The new process detects the daemon's mutex and enters viewer mode.
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

    /* snprintf truncation is safe - temp paths from GetTempPathA
     * are always shorter than MAX_PATH_BUF. */
    snprintf(ps1_path, sizeof(ps1_path),
             "%sghb-toast-%lu.ps1",
             temp_dir, (unsigned long)GetCurrentProcessId());

    /*
     * Get the path to the current executable.
     * The toast click handler will launch this executable to enter viewer mode.
     */
    char exe_path[MAX_PATH_BUF];
    DWORD exe_len = GetModuleFileNameA(NULL, exe_path, MAX_PATH_BUF);
    if (exe_len == 0 || exe_len >= MAX_PATH_BUF) {
        return;
    }

    /*
     * Escape the exe path for PowerShell single-quoted strings.
     * Single-quoted strings treat everything as literal EXCEPT single
     * quotes themselves - those must be doubled ('').
     */
    char ps_exe_path[MAX_PATH_BUF];
    int j = 0;
    for (DWORD k = 0; k < exe_len && j < (int)sizeof(ps_exe_path) - 2; k++) {
        if (exe_path[k] == '\'') {
            ps_exe_path[j++] = '\'';
            ps_exe_path[j++] = '\'';
        } else {
            ps_exe_path[j++] = exe_path[k];
        }
    }
    ps_exe_path[j] = '\0';

    /*
     * Write the PowerShell script.
     * Uses WinRT toast API via [Windows.UI.Notifications] types.
     *
     * Critical requirements for desktop app toasts:
     *   1. AppUserModelID MUST be the pre-registered PowerShell AUMID.
     *   2. Template MUST be ToastGeneric.
     *   3. Audio silent="true" prevents default notification sound.
     *   4. Duration="short" gives auto-dismiss behavior matching spec.
     *
     * Toast click interaction (Spec Section 9):
     *   The script registers an Activated event handler via
     *   $toast.Add_Activated(). When the user clicks the toast,
     *   the handler calls Start-Process to launch backup.exe.
     *   The new process enters viewer mode (mutex already exists).
     *
     * The event is registered BEFORE Show() and the script waits
     * for toast dismissal/activation before exiting. This ensures
     * the handler has time to fire if the user clicks quickly.
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
        "    $EXE_PATH = '%s'\n"
        "    $clicked = $false\n"
        "    $handler = [Windows.UI.Notifications.ToastActivatedEventHandler]::new({\n"
        "        param($sender, $e)\n"
        "        $script:clicked = $true\n"
        "        try { Start-Process -FilePath $EXE_PATH -WindowStyle Normal } catch { }\n"
        "    })\n"
        "    $toast.add_Activated($handler)\n"
        "    $dismissed = [Windows.UI.Notifications.ToastNotificationEventHandler]::new({\n"
        "        param($sender, $e)\n"
        "        $script:clicked = $true\n"
        "    })\n"
        "    $toast.add_Dismissed($dismissed)\n"
        "    $failed = [Windows.UI.Notifications.ToastFailedEventHandler]::new({\n"
        "        param($sender, $e)\n"
        "        $script:clicked = $true\n"
        "    })\n"
        "    $toast.add_Failed($failed)\n"
        "    $notifier.Show($toast)\n"
        "    $timeout = [System.DateTime]::Now.AddSeconds(10)\n"
        "    while (-not $clicked -and [System.DateTime]::Now -lt $timeout) {\n"
        "        Start-Sleep -Milliseconds 100\n"
        "    }\n"
        "} catch { }\n",
        safe_title, safe_message, ps_exe_path);

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
     */
    remove(ps1_path);
}


int notify_init(void) {
    fprintf(stderr, "[DBG] notify: Initializing COM (STA)...\n");
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[DBG] notify: COM init FAILED (hr=0x%lx)\n", (unsigned long)hr);
        log_error("notify", NULL, "COM initialization failed");
        return -1;
    }
    g_com_initialized = 1;
    fprintf(stderr, "[DBG] notify: COM initialized successfully\n");
    return 0;
}


void toast_info(const char *title, const char *message) {
    fprintf(stderr, "[DBG] notify: [INFO]  '%s' - '%s'\n", title, message);
    log_event(LOG_INFO, "toast", NULL, "INFO", message);
    #ifdef _WIN32
    show_toast_powershell(title, message);
    #endif
}


void toast_success(const char *repo, const char *message) {
    fprintf(stderr, "[DBG] notify: [OK]     '%s' - '%s'\n", repo, message);
    log_event(LOG_SUCCESS, "toast", repo, "OK", message);
    #ifdef _WIN32
    char title[512];
    snprintf(title, sizeof(title), "Backup OK: %s", repo);
    show_toast_powershell(title, message);
    #endif
}


void toast_error(const char *title, const char *message) {
    fprintf(stderr, "[DBG] notify: [ERROR] '%s' - '%s'\n", title, message);
    log_event(LOG_ERROR, "toast", NULL, "FAILED", message);
    #ifdef _WIN32
    show_toast_powershell(title, message);
    #endif
}


void notify_cleanup(void) {
    if (g_com_initialized) {
        fprintf(stderr, "[DBG] notify: Cleanup - CoUninitialize\n");
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
