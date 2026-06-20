# test-toast.ps1 — Standalone toast notification test
#
# Purpose: Isolate the Windows toast mechanism from the full backup flow.
# Run this directly in PowerShell. If a toast appears, the PowerShell/WinRT
# approach works and the issue is in how the C code invokes it. If NO toast
# appears, the issue is environmental (Focus Assist, AUMID, Windows version).
#
# Usage (from D:\BACKUP\ghb):
#   .\test-toast.ps1
#
# Or from anywhere:
#   powershell.exe -ExecutionPolicy Bypass -File "D:\BACKUP\ghb\test-toast.ps1"

Write-Host "=== Toast Notification Test ===" -ForegroundColor Cyan
Write-Host ""

# Test 1: Check PowerShell version (WinRT toasts need 5.1+)
Write-Host "PowerShell version: $($PSVersionTable.PSVersion)" -ForegroundColor Gray
if ($PSVersionTable.PSVersion.Major -lt 5) {
    Write-Host "FAIL: PowerShell 5.1+ required for WinRT toasts" -ForegroundColor Red
    exit 1
}
Write-Host "PASS: PowerShell version OK" -ForegroundColor Green
Write-Host ""

# Test 2: Load WinRT types
Write-Host "Loading WinRT types..." -ForegroundColor Gray
try {
    [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
    [Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom, ContentType = WindowsRuntime] | Out-Null
    Write-Host "PASS: WinRT types loaded" -ForegroundColor Green
} catch {
    Write-Host "FAIL: Cannot load WinRT types: $_" -ForegroundColor Red
    Write-Host "This is why toasts don't work. WinRT may not be available on this system." -ForegroundColor Yellow
    exit 1
}
Write-Host ""

# Test 3: Register custom AUMID (same as the C code does)
$AUMID = "GitHubBackup.Toast"
Write-Host "Registering AUMID: $AUMID" -ForegroundColor Gray
$regPath = "HKCU:\Software\Classes\AppUserModelId\$AUMID"
if (-not (Test-Path $regPath)) {
    New-Item -Path $regPath -Force | Out-Null
}
New-ItemProperty -Path $regPath -Name "DisplayName" -Value "GitHub Backup" -PropertyType String -Force | Out-Null
New-ItemProperty -Path $regPath -Name "ShowInSettings" -Value 1 -PropertyType DWord -Force | Out-Null
Write-Host "PASS: AUMID registered" -ForegroundColor Green
Write-Host ""

# Test 4: Build the toast XML (same format as the C code generates)
$xml = New-Object Windows.Data.Xml.Dom.XmlDocument
$toastXml = '<toast duration="short"><visual><binding template="ToastGeneric"><text>GitHub Backup Test</text><text>If you see this, toasts work.</text></binding></visual><audio silent="true"/></toast>'
$xml.LoadXml($toastXml)

# Test 5: Create and show the toast
Write-Host "Creating toast notification with AUMID: $AUMID" -ForegroundColor Gray
try {
    $toast = [Windows.UI.Notifications.ToastNotification]::new($xml)
    $notifier = [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($AUMID)
    $notifier.Show($toast)
    Write-Host "PASS: Toast shown (check your notification center)" -ForegroundColor Green
    Write-Host ""
    Write-Host "Waiting 10 seconds for toast to display..." -ForegroundColor Yellow
    Start-Sleep -Seconds 10
    Write-Host ""
    Write-Host "Did you see a toast notification?" -ForegroundColor Cyan
    Write-Host "  YES -> The toast mechanism works. The issue is in how backup.exe calls it." -ForegroundColor Green
    Write-Host "  NO  -> Environmental issue: check Focus Assist, notification settings, or AUMID." -ForegroundColor Yellow
} catch {
    Write-Host "FAIL: Cannot show toast: $_" -ForegroundColor Red
    Write-Host ""
    Write-Host "This error would be captured in %TEMP%\ghb-toast-error.log when backup.exe runs." -ForegroundColor Yellow
}
Write-Host ""
Write-Host "=== Test complete ===" -ForegroundColor Cyan
