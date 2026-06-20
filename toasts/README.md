# toasts

Static PowerShell toast notification scripts for the GitHub Backup tool.

## Files

| File | Purpose |
|------|---------|
| `show-toast.ps1` | Generic toast script — takes `-Title` and `-Message` parameters, shows a toast, sleeps 2 seconds (fire-and-forget) |

## Architecture (R158 research)

These scripts are invoked by `backup.exe` via `CreateProcessW`:

```
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File show-toast.ps1 -Title "..." -Message "..."
```

### Why static .ps1 files (not -EncodedCommand)?

Per the R158 compiled research report (10 web-search agents + 1 compiler):
1. `-File` is more reliable than `-EncodedCommand` (no Base64/UTF-16LE encoding round-trip, no 32KB limit, real parse errors).
2. The old `-EncodedCommand` approach used WinRT event handlers (`[ToastActivatedEventHandler]::new({...})`) which silently throw on PowerShell 5.1 — the #1 root cause of toast failure.
3. Static .ps1 files are the dominant production pattern (imabdk/Toast-Notification-Script, PDQ Deploy, PSAppDeployToolkit).
4. Static files are independently testable: `.\show-toast.ps1 -Title Test -Message Hello`

### Why no WinRT event handlers?

PowerShell 5.1 cannot subscribe to WinRT events natively. The `[EventHandler]::new({...})` pattern silently throws, the `try/catch` swallows the error, and `$notifier.Show($toast)` is never reached. The fix: no event handlers, just `Show()` + `Start-Sleep -Seconds 2` (fire-and-forget).

### Why Start-Sleep -Seconds 2?

`ToastNotifier.Show()` is asynchronous. The Windows Notification Platform (separate service) renders the toast. The 2-second sleep keeps the PowerShell process alive long enough for the WNP to pick up the toast. This matches the working `test-toast.ps1` pattern.

## Testing

```powershell
.\show-toast.ps1 -Title "Test Toast" -Message "If you see this, toasts work."
```

## Deployment

The `update.ps1` script deploys this folder automatically (it's under `ghb/toasts/` in the repo). The C code finds it at runtime via `GetModuleFileNameA` (next to `backup.exe`).
