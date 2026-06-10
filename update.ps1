<#
.SYNOPSIS
    Updates local github-backup source files from the PROGRAMMING private repo.
.DESCRIPTION
    Downloads the latest ghb/ source tree from agent-workspace-1157/PROGRAMMING
    and copies it into the current directory (where backup.exe lives).
    The .env file, .git folder, compiled binaries, and zip archives are never touched.
.EXAMPLE
    cd D:\BACKUP\ghb
    .\update.ps1
#>

param(
    [string]$TokenFile = ".env"
)

# ── Read the token from .env ──────────────────────────────────
# Supports two formats:
#   GITHUB_TOKEN=ghp_xxxxxxxxxxxx        (preferred)
#   GITHUB_BASE_URL=https://ghp_xxx@github.com/owner/   (legacy)

if (-not (Test-Path $TokenFile)) {
    Write-Host "ERROR: $TokenFile not found in current directory." -ForegroundColor Red
    Write-Host "This script must be run from the same directory as .env" -ForegroundColor Red
    exit 1
}

$Token = $null

# Method 1: Read GITHUB_TOKEN= directly
$tokenLine = (Get-Content $TokenFile | Where-Object { $_ -match "^GITHUB_TOKEN=" })
if ($tokenLine) {
    $Token = ($tokenLine -split "=", 2)[1].Trim()
}

# Method 2: Extract from GITHUB_BASE_URL=https://<TOKEN>@github.com/...
if (-not $Token) {
    $urlLine = (Get-Content $TokenFile | Where-Object { $_ -match "^GITHUB_BASE_URL=" })
    if ($urlLine) {
        $urlValue = ($urlLine -split "=", 2)[1].Trim()
        if ($urlValue -match "^https://([^@]+)@") {
            $Token = $Matches[1]
        }
    }
}

if (-not $Token) {
    Write-Host "ERROR: No token found in $TokenFile" -ForegroundColor Red
    Write-Host "Expected either:" -ForegroundColor Red
    Write-Host "  GITHUB_TOKEN=ghp_xxxxxxxxxxxx" -ForegroundColor Red
    Write-Host "  GITHUB_BASE_URL=https://ghp_xxx@github.com/owner/" -ForegroundColor Red
    exit 1
}

Write-Host "Token extracted from .env ($($Token.Substring(0,8))...)" -ForegroundColor Gray

# ── Configuration ─────────────────────────────────────────────
$Repo       = "agent-workspace-1157/PROGRAMMING"
$Branch     = "main"
$Subdir     = "ghb"           # Subdirectory inside PROGRAMMING that holds our source
$TempDir    = Join-Path $env:TEMP "ghb-update-$(Get-Random)"
$Archive    = Join-Path $env:TEMP "ghb-update-$(Get-Random).zip"
$DestDir    = $PSScriptRoot
if (-not $DestDir) { $DestDir = Get-Location }

# Files to NEVER overwrite
$Protected = @(".env", ".git", "*.exe", "*.zip", "*.log", "update.ps1")

# ── Download archive ──────────────────────────────────────────
# CRITICAL: PowerShell's Invoke-WebRequest strips credentials from URLs.
# The token MUST go in the Authorization header, not in the URL.
# Using the GitHub API zipball endpoint which accepts Bearer tokens.

$ArchiveUrl = "https://api.github.com/repos/${Repo}/zipball/${Branch}"
$Headers = @{
    "Authorization" = "Bearer $Token"
    "Accept"       = "application/vnd.github.v3+json"
    "User-Agent"   = "ghb-update-script"
}

Write-Host ""
Write-Host "Downloading ${Repo}@${Branch}..." -ForegroundColor Cyan

try {
    Invoke-WebRequest -Uri $ArchiveUrl -Headers $Headers -OutFile $Archive -UseBasicParsing
} catch {
    Write-Host "ERROR: Download failed: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.Exception.Response) {
        $statusCode = $_.Exception.Response.StatusCode
        Write-Host "HTTP Status: $statusCode" -ForegroundColor Red
        if ($statusCode -eq 404) {
            Write-Host "404 means: token lacks access to this repo, or repo/branch does not exist." -ForegroundColor Yellow
        }
        if ($statusCode -eq 401) {
            Write-Host "401 means: token is invalid or expired." -ForegroundColor Yellow
        }
        if ($statusCode -eq 403) {
            Write-Host "403 means: token valid but lacks permission, or rate limited." -ForegroundColor Yellow
        }
    }
    exit 1
}

if (-not (Test-Path $Archive)) {
    Write-Host "ERROR: Archive file not created" -ForegroundColor Red
    exit 1
}

Write-Host "Download complete ($( [math]::Round((Get-Item $Archive).Length / 1KB, 1) ) KB)" -ForegroundColor Green

# ── Extract archive ───────────────────────────────────────────
Write-Host "Extracting..." -ForegroundColor Cyan

try {
    if (Test-Path $TempDir) { Remove-Item $TempDir -Recurse -Force }
    Expand-Archive -Path $Archive -DestinationPath $TempDir -Force
} catch {
    Write-Host "ERROR: Extraction failed: $($_.Exception.Message)" -ForegroundColor Red
    Remove-Item $Archive -Force -ErrorAction SilentlyContinue
    exit 1
}

# Find the extracted root folder (GitHub names it agent-workspace-1157-PROGRAMMING-<hash>)
$ExtractedRoot = Get-ChildItem $TempDir -Directory | Select-Object -First 1
$GhbSource = Join-Path $ExtractedRoot.FullName $Subdir

if (-not (Test-Path $GhbSource)) {
    Write-Host "ERROR: '$Subdir' subdirectory not found in archive" -ForegroundColor Red
    Remove-Item $Archive -Force -ErrorAction SilentlyContinue
    Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

# ── Copy files (respecting protected list) ─────────────────────
Write-Host "Updating local files..." -ForegroundColor Cyan

$UpdatedCount = 0
$SkippedCount = 0

Get-ChildItem -Path $GhbSource -Recurse -File | ForEach-Object {
    $RelativePath = $_.FullName.Substring($GhbSource.Length + 1)
    $DestPath = Join-Path $DestDir $RelativePath

    # Check against protected patterns
    $Skip = $false
    foreach ($Pattern in $Protected) {
        if ($RelativePath -like $Pattern) {
            $Skip = $true
            break
        }
        # Also protect root-level files matching pattern
        if ($_ -like $Pattern) {
            $Skip = $true
            break
        }
    }

    if ($Skip) {
        $SkippedCount++
        return
    }

    # Ensure destination directory exists
    $DestParent = Split-Path $DestPath -Parent
    if (-not (Test-Path $DestParent)) {
        New-Item -ItemType Directory -Path $DestParent -Force | Out-Null
    }

    Copy-Item -Path $_.FullName -Destination $DestPath -Force
    $UpdatedCount++
}

# ── Cleanup ───────────────────────────────────────────────────
Remove-Item $Archive -Force -ErrorAction SilentlyContinue
Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue

# ── Report ───────────────────────────────────────────────────
Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host "  Updated: $UpdatedCount files" -ForegroundColor White
Write-Host "  Skipped (protected): $SkippedCount files (.env, .git, *.exe, *.zip, *.log, update.ps1)" -ForegroundColor Gray
Write-Host ""
Write-Host "Ready to compile:" -ForegroundColor Yellow
Write-Host "  gcc -Wall -Wextra -O2 -static -o backup.exe src/main.c src/backup.c src/config.c src/network.c src/logger.c src/notify.c src/console.c -I src/ -D_WIN32_WINNT=0x0600 -DUNICODE -D_UNICODE -lwinhttp -lkernel32 -lshell32 -lole32 -lruntimeobject -lshlwapi" -ForegroundColor DarkGray
