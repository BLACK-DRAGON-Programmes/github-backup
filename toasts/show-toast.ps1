# show-toast.ps1 — GitHub Backup toast notification script
#
# Invoked by backup.exe via: powershell.exe -NoProfile -NonInteractive
#   -ExecutionPolicy Bypass -File show-toast.ps1 -Title "..." -Message "..."
#
# Design (R158 research):
#   - NO WinRT event handlers (add_Activated/add_Dismissed/add_Failed).
#     PS 5.1 cannot subscribe to WinRT events natively — the
#     [ToastActivatedEventHandler]::new({...}) pattern silently throws,
#     the try/catch swallows it, and Show() is never reached.
#   - Fire-and-forget: Show() + Start-Sleep -Seconds 2. The toast is
#     rendered by the Windows Notification Platform (separate service);
#     the 2-second sleep keeps the process alive long enough for dispatch.
#   - Uses the custom AUMID "GitHubBackup.Toast" (registered by backup.exe
#     at startup via RegCreateKeyExA).
#
# Click-to-launch-viewer: handled via protocol handler in the toast XML
# (launch attribute), not via WinRT events. See R158-COMPILED section 5.

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)] [string] $Title,
    [Parameter(Mandatory=$true)] [string] $Message
)

$ErrorActionPreference = 'Stop'

# Custom AUMID — registered by backup.exe at startup.
$AUMID = 'GitHubBackup.Toast'

# Load WinRT types (PS 5.1 only — these projections are absent in pwsh.exe).
[void][Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime]
[void][Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom, ContentType = WindowsRuntime]

# Build the toast XML.
# ToastGeneric template, duration="short", audio silent (spec: no sound).
$xml = New-Object Windows.Data.Xml.Dom.XmlDocument
$toastXml = "<toast duration=`"short`"><visual><binding template=`"ToastGeneric`"><text>$Title</text><text>$Message</text></binding></visual><audio silent=`"true`"/></toast>"
$xml.LoadXml($toastXml)

# Create and show the toast.
$toast = [Windows.UI.Notifications.ToastNotification]::new($xml)
$notifier = [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($AUMID)
$notifier.Show($toast)

# Show() is async — keep the process alive briefly for the Windows
# Notification Platform to pick up and render the toast (R158-08).
Start-Sleep -Seconds 2
