# User Guide — GitHub Backup Script

## Runtime Modes

The tool has three runtime modes, controlled by how you invoke `backup.exe` and whether an instance is already running.

### Foreground Mode

Run `backup.exe` directly from a terminal (Command Prompt or PowerShell).

```
backup.exe
```

**What happens:**
1. The tool initializes (reads `.env`, creates `BACKUP_DIR`, starts logging, connects to the network).
2. A "Service Started" toast notification fires.
3. The main backup loop begins: check internet, download each repo, sleep, repeat.
4. Every operation is printed to the terminal with ANSI color formatting (green = success, red = error, yellow = warning, white = info, cyan = repo names).
5. A toast notification fires for every event (cycle start, per-repo success/failure, cycle complete, no internet, rate limited).

**To stop:** Press `Ctrl+C`. The tool finishes the current download (if one is in progress), writes a shutdown log entry, and exits gracefully.

### Background Mode

Run `backup.exe` with the `--background` flag, or deploy via Task Scheduler (which launches with the same behavior).

```
backup.exe --background
```

**What happens:**
1. The tool initializes identically to foreground mode (all startup logging, config parsing, network init).
2. After initialization completes, the console is detached via `FreeConsole()`.
3. The tool runs with no visible window. No console output is produced.
4. Toast notifications are your **only** visible feedback. They fire for every event.
5. The log file (`{BACKUP_DIR}backup.log`) records everything silently in the background.

**To stop:**
- Run `backup.exe --shutdown` from any terminal.
- Or use Task Scheduler to end the task.
- Or use `taskkill /IM backup.exe /F` from a command prompt.

### Log Viewer Mode

Run `backup.exe` while an instance is already running (detected via a Windows mutex).

```
backup.exe
```

**What happens:**
1. The tool detects that another instance holds the mutex.
2. Instead of starting a backup, it enters a live log tailing mode.
3. New log entries written by the running backup instance are displayed on screen with ANSI colors.
4. Only **new** entries from the moment you open the viewer are shown (no historical log dump).

**To exit:** Press `Ctrl+C`. The backup instance continues running unaffected.

---

## Shutdown Methods

### Method 1: Command-Line Flag

```
backup.exe --shutdown
```

Opens the named shutdown event (`Global\GitHubBackupShutdown`). The running instance detects the signal and exits gracefully after completing its current operation (finishes any in-progress download, writes cycle summary, closes log file).

### Method 2: Ctrl+C (Foreground Only)

When running in foreground mode (no `--background` flag), pressing `Ctrl+C` sets a shutdown flag. The main loop checks the flag at the start of each cycle iteration and exits cleanly. If a download is in progress, it completes before shutdown occurs.

---

## Automatic Behavior

Once running, the tool operates autonomously. Here is exactly what happens on each cycle:

1. **Check internet connectivity** — A lightweight HEAD request to `github.com` with a configurable timeout (default: 5 seconds).
   - If no internet: toast fires "No internet detected — cycle skipped". Sleeps and retries next cycle.
2. **Re-read `.env`** — Configuration is parsed fresh every cycle. Changes to `.env` take effect without restart.
3. **Toast "Starting backup cycle"** — Fires with the repo count.
4. **For each repository** in the `REPOS` list:
   - Resolve the default branch via GitHub API.
   - Download the zip archive to a temporary file (`{repo}.zip.tmp`).
   - Verify the download (non-zero size, readable).
   - Atomic write: delete old backup, rename temp to final.
   - Toast on success (repo name + file size) or failure (repo name + error type).
5. **Toast "Cycle complete"** — Summary of succeeded and failed counts.
6. **Rotate log** — If `backup.log` exceeds the configured size, delete and start fresh.
7. **Sleep** — Wait for the configured interval (default: 1 hour), then repeat.

### Error Handling

| Situation | Behavior |
|----------|----------|
| Invalid/expired token | Toast + log error. Skip cycle. Retry next interval. |
| Network failure (one repo) | Toast + log error. Continue with next repo. |
| Repository 404 (not found) | Toast + log warning. Skip that repo. Continue with others. |
| Rate limited (HTTP 429) | Toast + log warning. Sleep until reset window. Retry. |
| Disk full | Toast + log error. Stop cycle. All existing backups remain intact. |
| Corrupt `.env` | Toast + log error. Exit gracefully (manual intervention required). |
| No internet | Toast + log warning. Skip cycle. Sleep and retry. |

---

## Configuration

All runtime behavior is controlled by the `.env` file next to `backup.exe`. Edit it anytime — changes take effect on the next cycle without recompilation.

### Mandatory Fields

| Variable | Description | Example |
|----------|------------|---------|
| `GITHUB_BASE_URL` | Token + owner URL (Option A) | `https://ghp_ABC123@github.com/my-org/` |
| `REPOS` | Comma-separated repo list | `repo-one,repo-two,repo-three` |

**Alternative authentication (Option B):**

| Variable | Description | Example |
|----------|------------|---------|
| `GITHUB_TOKEN` | Standalone personal access token | `ghp_xxxxxxxxxxxxxxxxxxxx` |
| `GITHUB_OWNER` | Target GitHub account/org | `my-organization` |

When `GITHUB_TOKEN` is present, it takes precedence over any token embedded in `GITHUB_BASE_URL`.

### Configurable Parameters (Optional — All Have Defaults)

| Variable | Default | Description |
|----------|---------|-------------|
| `BACKUP_DIR` | `D:\BACKUP\` | Root directory for zip archives and log file |
| `CYCLE_INTERVAL_SECONDS` | `3600` | Seconds between backup cycles (1 hour) |
| `HTTP_TIMEOUT_MS` | `30000` | Timeout for GitHub API requests (30 seconds) |
| `CONNECTIVITY_CHECK_TIMEOUT_MS` | `5000` | Timeout for internet connectivity check (5 seconds) |
| `SHUTDOWN_CHECK_INTERVAL_MS` | `1000` | How often to poll for shutdown signal during sleep (1 second) |
| `LOG_MAX_SIZE_BYTES` | `1048576` | Log file size before rotation (1 MiB) |

All values are read from `.env` at runtime. No recompilation needed to change any of them.

---

## Deployment via Task Scheduler

1. Open Task Scheduler (`taskschd.msc`).
2. Click **Create Task** (not "Basic Task").
3. **General tab:**
   - Name: `GitHub Backup Service`
   - Security options: "Run whether user is logged on or not"
   - Check "Run with highest privileges"
4. **Triggers tab:** New trigger — "At startup"
5. **Actions tab:** New action — "Start a program"
   - Program: `D:\BACKUP\ghb\backup.exe`
   - Start in: `D:\BACKUP\ghb\`
   - Add arguments: `--background` (for silent background operation)
6. **Conditions tab:**
   - Uncheck "Start the task only if the computer is on AC power"
   - Check "Start even if on batteries"
7. **Settings tab:**
   - Check "Allow task to be run on demand"
   - Check "Run task as soon as possible after a scheduled start is missed"
   - If the task fails, restart every 1 minute, up to 3 times

---

## Updating the Software

### Using update.ps1 (Recommended)

```powershell
cd D:\BACKUP\ghb
.\update.ps1
```

The script reads the token from your `.env` file, downloads the latest source from the private PROGRAMMING repo, and copies files to the local directory. Your `.env`, compiled binaries, zip archives, logs, and `update.ps1` itself are never overwritten.

### Recompiling After Update

```powershell
gcc -Wall -Wextra -O2 -static -o backup.exe src/main.c src/backup.c src/config.c src/network.c src/logger.c src/notify.c src/console.c -I src/ -D_WIN32_WINNT=0x0600 -DUNICODE -D_UNICODE -lwinhttp -lkernel32 -lshell32 -lole32 -lruntimeobject -lshlwapi
```

Copy the new `backup.exe` next to your `.env` file.
