# GitHub Organization Backup Script — Project Specification

## Overview

A generic Windows-native tool that automatically backs up specified GitHub repositories to local zip archives. The tool runs as a scheduled task (or manually) on Windows 10, executes on a configurable cycle interval (default: 1 hour, when internet is available), and retains only the latest zip per repository. The target account, token, and list of repositories to back up are all sourced from a local `.env` file located next to the executable, making the tool portable across any GitHub account or organization.

The tool supports three runtime modes controlled by whether an instance is already running:
- **Backup mode** (default) — single backup instance, runs cycles, writes to log.
- **Log viewer mode** — when an instance is already running, new invocations enter a live log tailing mode with colored console output.
- **Shutdown mode** — a command-line flag (`--shutdown`) that signals the running instance to exit gracefully.

---

## Requirements

### 1. Location and File Structure

- **Executable location:** Any directory. The tool determines its own location via `GetModuleFileNameA` at startup. All paths are resolved relative to where the executable lives.
- **Configuration:** A `.env` file in the same directory as the executable. The tool locates `.env` by finding its own directory and appending `.env` — no hardcoded paths.
- **Output (zip archives):** Stored in `BACKUP_DIR` (default: `D:\BACKUP\` — configurable via `.env`). Each repository is downloaded as a zip archive to `{BACKUP_DIR}<repo-name>.zip`.
- **Output (log file):** `{BACKUP_DIR}backup.log` — structured log entries with timestamps, levels, actions, and details.
- **Only the latest copy** of each repository is kept. If a previous zip exists for a given repo, it is overwritten (via atomic write, see Section 5).

> **Note:** The directory path `D:\BACKUP\` used throughout this specification is the default value for `BACKUP_DIR`. The actual deployment directory is set by the `BACKUP_DIR` variable in `.env`. All path references below resolve to whatever `BACKUP_DIR` is set to.

```
<exe-directory>/                  (wherever backup.exe lives)
├── backup.exe                    (the compiled executable)
├── .env                          (configuration — must be next to exe)
├── update.ps1                    (source update tool — never overwritten by updates)
├── src/                          (source files — development only)
├── docs/                         (documentation — development only)
├── tests/                        (unit tests — development only)
└── build/                        (build artifacts — development only)

<BACKUP_DIR>/                     (default: D:\BACKUP\)
├── backup.log                    (auto-created, auto-rotated at LOG_MAX_SIZE_BYTES)
├── repo-name-1.zip               (latest backup of repo-name-1)
├── repo-name-2.zip               (latest backup of repo-name-2)
└── ...
```

**Auto-create directories:** `BACKUP_DIR` is created automatically at startup if it does not exist (via `SHCreateDirectoryExA`). The operator never needs to manually create directories.

### 2. `.env` Configuration

The `.env` file provides all runtime configuration. No values are hardcoded in the tool. The file must reside in the same directory as the executable and is located by the tool at startup (not via a hardcoded path). The file is read fresh on every execution cycle, so changes take effect on the next run without recompilation.

**Format:**

```env
GITHUB_BASE_URL=https://<YOUR_PERSONAL_ACCESS_TOKEN>@github.com/<OWNER>/
REPOS=repo-one,repo-two,repo-three
```

**Alternatively, the token can be provided as a separate field:**

```env
GITHUB_TOKEN=ghp_xxxxxxxxxxxxxxxxxxxx
GITHUB_OWNER=agent-workspace-1157
REPOS=repo-one,repo-two,repo-three
```

**Fields:**

- **`GITHUB_BASE_URL`** — The base URL for the target GitHub account. Contains the personal access token embedded in the URL authority (before `@github.com`), followed by the owner/organization path. This single string controls authentication and the target account/organization.
  - Token can be swapped by editing the token portion of the URL.
  - The owner/org can be swapped by changing the path after `github.com/`.
  - This makes the tool usable for **any** GitHub account or organization — not tied to one specific org.
  - The trailing slash is required.

- **`GITHUB_TOKEN`** — Alternative to embedding the token in `GITHUB_BASE_URL`. When this field is present, it takes precedence for authentication. Used when the operator prefers separating the token from the URL. Must be a `ghp_` personal access token.

- **`GITHUB_OWNER`** — Used in conjunction with `GITHUB_TOKEN` when `GITHUB_BASE_URL` is not set. Specifies the target GitHub account or organization.

- **`REPOS`** — A comma-separated list of repository names to back up. Only repositories listed in this array are downloaded. Repos not in the list are ignored, even if they exist in the target account.
  - The user manually maintains this list. Adding or removing a repo name takes effect on the next cycle.
  - Whitespace around commas should be trimmed by the parser.
  - Empty values or lines starting with `#` should be ignored (for comments).

**Token rotation:** Since the token is either embedded in the URL or stored as a separate field, simply edit the `.env` file and replace the old token with a new one. No recompilation needed. The tool must gracefully handle an invalid/expired token (log the error and retry on the next cycle — do not crash or exit).

**Additional configurable parameters:** The `.env` file also accepts optional tuning parameters. These have sensible defaults that match the values stated throughout this specification. They only need to be set if the defaults are not suitable for a particular deployment. All changes take effect on the next cycle without recompilation. See `env.example` for the full list with documentation and usage guidance:

| Variable | Default | Spec Reference |
|----------|---------|----------------|
| `BACKUP_DIR` | `D:\BACKUP\` | Section 1 (location), Section 5 (file paths), Section 8 (log path) |
| `CYCLE_INTERVAL_SECONDS` | 3600 (1 hour) | Section 3 (cycle interval) |
| `HTTP_TIMEOUT_MS` | 30000 (30 seconds) | Section 7 (network failure handling) |
| `CONNECTIVITY_CHECK_TIMEOUT_MS` | 5000 (5 seconds) | Section 4 (connectivity check) |
| `SHUTDOWN_CHECK_INTERVAL_MS` | 1000 (1 second) | Section 11 (shutdown polling interval) |
| `LOG_MAX_SIZE_BYTES` | 1048576 (1 MiB) | Section 8 (log rotation) |

**Every numeric value that controls runtime behavior** (timing intervals, timeouts, thresholds, directory paths) is configurable from the `.env` file. The only values that are not configurable are those that are intrinsic to the C language or the GitHub API protocol — HTTP status codes, API endpoint URLs, and stack-allocated buffer sizes. These compile-time constants are documented in `constants.h`. If a value affects how the tool behaves at runtime, it can be changed in `.env` without recompilation.

**Constant architecture:** Values that are operator-configurable (deployment paths, timing, timeouts, log thresholds) live in `.env` and are read at runtime — no recompilation required. Values that are not operator-configurable (HTTP protocol status codes, GitHub API endpoints, buffer sizes required by the C compiler for stack allocation) are compile-time constants defined in `constants.h`. This split is documented in both `env.example` and `constants.h`.

### 3. Auto-Start Behavior

- The tool starts automatically when the computer boots.
- **Method:** Windows Task Scheduler. A scheduled task triggers the executable at system startup (runs even if nobody is logged in).
- The tool runs continuously in the background, executing a backup cycle every **1 hour** (default — configurable via `CYCLE_INTERVAL_SECONDS` in `.env`, see Section 2).
- After completing a cycle, the tool sleeps for the configured interval, then checks internet connectivity and runs the next cycle.
- **Background operation:** When launched via Task Scheduler (or with `--background` flag), the tool detaches from its console after initialization using `FreeConsole()`. It continues running with no visible window. Toast notifications remain active as the primary user-visible feedback mechanism.
- **Foreground operation:** When launched directly from a terminal without `--background`, the tool runs with an attached console. Log entries are printed to the console with ANSI color formatting in addition to being written to the log file.

### 4. Internet Connectivity Check

- Before each backup cycle, the tool checks whether the internet is available.
- If no internet: fire a toast ("No internet detected — cycle skipped"), log, skip the cycle, sleep, and retry after the configured cycle interval (default: 1 hour).
- If internet is available: proceed with the backup cycle.
- The check should be simple and lightweight — a DNS resolution and TCP connection to `github.com`, not a full API call.
- **Network initialization:** Direct connection mode (`WINHTTP_ACCESS_TYPE_NO_PROXY`). No proxy auto-detection (WPAD) — the tool connects directly to `api.github.com` and `codeload.github.com`, so proxy detection is unnecessary and would cause startup delays.

### 5. Backup Logic

1. Read `GITHUB_BASE_URL` (or `GITHUB_TOKEN` + `GITHUB_OWNER`) and `REPOS` from `.env`.
2. Parse the base URL to extract:
   - **Token:** The portion before `@github.com` (used for API authentication via `Authorization: Bearer <token>` header), or the standalone `GITHUB_TOKEN` field.
   - **Owner:** The path segment after `github.com/` (e.g., `my-organization`), or the `GITHUB_OWNER` field.
3. For each repository in the `REPOS` array:
   a. Resolve the repository's default branch.
      - Endpoint: `GET https://api.github.com/repos/<owner>/<repo-name>`
      - Extract the `default_branch` field from the JSON response.
   b. Download the repository as a zip archive.
      - Endpoint: `GET https://api.github.com/repos/<owner>/<repo-name>/zipball/<default-branch>`
      - Authenticate with `Authorization: Bearer <token>`.
   c. Download the zip to a temporary file: `{BACKUP_DIR}<repo-name>.zip.tmp`.
   d. Verify the temporary file is fully written, valid (non-zero size, readable), and accessible on disk.
   e. If verification passes: delete the previous backup file `{BACKUP_DIR}<repo-name>.zip` (if it exists), then rename the temporary file to `{BACKUP_DIR}<repo-name>.zip`.
   f. If verification fails (corrupt or incomplete download): delete the temporary file. The previous backup (if any) remains untouched. Log the error, fire a toast, and continue with the next repo.
   g. If the repository does not exist or returns 404, log a warning, fire a toast, and skip (the repo may have been renamed or deleted). Continue with the next repo.

**Atomic write guarantee:** The old backup is never deleted before the new backup is confirmed on disk. At no point during a cycle does a repository have zero valid backups. Only one backup per repo exists at any time — no timestamped copies, no accumulation.
4. Log each action (repo name, success/failure, timestamp) to the log file and console (if attached).

**Note:** The tool does NOT list all repos in the org and back up everything. It only downloads repos explicitly listed in the `REPOS` array. This gives full control over which repos are backed up and avoids wasting bandwidth on repos not needed.

### 6. Language and Build

- **Intermediate language:** C (compiled with MinGW-w64 on Windows).
  - Produces a single static `.exe` — no runtime dependencies, no installation, no interpreter.
  - C's Win32 API calls (WinHTTP for HTTP, Kernel32 for file I/O) map almost 1:1 to NASM Win32 calls, making the eventual NASM rewrite a mechanical translation.
  - Alternative languages (PowerShell, Go, Rust, etc.) were rejected because they operate at a higher abstraction level and would not serve as useful stepping stones to NASM.
- **Final language:** NASM assembly (x86-64, Windows calling convention).
  - The NASM version will be a direct translation of the C version's logic, replacing standard library calls with Win32 API calls and libc functions with hand-written routines.
- The tool is **not** Python. This is a hard constraint.

### 7. Error Handling

- **Invalid/expired token:** Log the error, fire a toast, do not crash. Retry on the next cycle.
- **Network failure during download:** Log the failed repo, fire a toast, continue with the next repo. Do not abort the entire cycle for one failure.
- **API rate limiting:** Respect `X-RateLimit-Remaining` headers. If rate-limited, log, fire a toast, sleep until the reset window and retry.
- **Disk full:** Log the error, fire a toast, stop the current cycle. Delete any temporary file. Do not attempt partial writes. The previous backup for the current repo remains intact.
- **Corrupt `.env` file:** Log the error, fire a toast, exit gracefully (this requires manual intervention).

### 8. Logging

#### 8a. File Logging (always active)

- A structured log file at `{BACKUP_DIR}backup.log` (default: `D:\BACKUP\backup.log`).
- Each entry: timestamp, action, repo name (if applicable), status (success/failure), error details (if any).
- Log file should be appended to, not overwritten. If it grows too large, rotate it (delete the file and start fresh — the log is ephemeral, not an archive).

#### 8b. Console Logging (when terminal is attached)

- When the tool is running with an attached console (foreground mode), log entries are printed to the console **in addition to** being written to the log file.
- Console output uses ANSI escape codes for color-coded, column-aligned formatting (Windows 10+ supports VT100 via `SetConsoleMode` with `ENABLE_VIRTUAL_TERMINAL_PROCESSING`).
- Color scheme:
  - `INFO` → dim white / gray
  - `OK` / `SUCCESS` → bright green
  - `WARN` / `WARNING` → yellow
  - `ERROR` → bright red
  - Timestamps → dim gray
  - Repo names → cyan
  - Separator characters → dim gray
- Format (example):
```
[2026-06-04 05:00:12]  INFO   │ main       │ STARTED     │ GitHub Backup service started successfully
[2026-06-04 05:00:13]  OK     │ network    │ CONNECTED   │ Internet connectivity confirmed
[2026-06-04 05:00:14]  INFO   │ main       │ CYCLE_START │ Starting backup cycle for 6 repositories
[2026-06-04 05:00:18]  OK     │ backup     │ my-repo     │ BACKED_UP   │ Downloaded 142KB in 3.2s
```
- In background mode (detached console), console output is suppressed. Only toasts and the log file remain active.

### 9. Notifications (Windows Toasts)

- The tool fires a Windows toast notification for **every runtime event** — not just errors. This includes:
  - **Per-repo backup success:** Repo name, timestamp, file size (if available).
  - **Per-repo backup failure:** Repo name, timestamp, error type and details.
  - **Internet connectivity:** "No internet detected — cycle skipped" toast when connectivity check fails.
  - **Rate limit hit:** "Rate limited by GitHub API — sleeping until reset" toast with reset time.
  - **Corrupt `.env`:** Toast on detection before graceful exit.
  - **Cycle start:** "Starting backup cycle for N repositories" toast.
  - **Cycle complete:** "Backup cycle complete: X succeeded, Y failed" summary toast.
- **Toast content:** All available information — action, repo name (if applicable), status (success/failure), timestamp, error details (if applicable).
- **Toast duration:** Standard Windows auto-dismiss behavior. No persistent/to-click-dismiss toasts.
- **Implementation:** Windows native toast notification API. The tool runs as a background scheduled task with no console window — toasts are the primary user-visible feedback mechanism in background mode.

### 10. Single-Instance and Log Viewer

#### 10a. Instance Detection

- At startup, the tool attempts to create a named Windows mutex (e.g., `Global\GitHubBackupMutex`).
- If the mutex is created successfully → no other instance is running → enter **backup mode** (normal operation).
- If the mutex already exists (ERROR_ALREADY_EXISTS) → another instance is running → enter **log viewer mode**.

#### 10b. Backup Mode (normal operation)

- The tool runs the full startup sequence (config parsing, log init, network init) and enters the main backup loop.
- If launched with a console attached, pretty console output is active.
- If launched with `--background` flag or via Task Scheduler, the console is detached after startup via `FreeConsole()`.
- The mutex is held for the entire lifetime of the process.

#### 10c. Log Viewer Mode

- When a second instance detects the mutex, it enters log viewer mode instead of starting a backup.
- The log viewer opens `{BACKUP_DIR}backup.log`, seeks to the current end of the file, and tails new entries as they are written by the running backup instance.
- Each tailed entry is printed to the console with the same ANSI color formatting as backup mode (Section 8b).
- The log viewer does NOT show historical log entries — only new entries that appear after the viewer starts.
- `Ctrl+C` in the log viewer exits the viewer. The backup instance continues running unaffected.

### 11. Shutdown Mechanism

The running backup instance can be shut down gracefully via two methods:

#### 11a. Command-Line Flag

```
backup.exe --shutdown
```

- When invoked with `--shutdown`, the tool does not start a backup or log viewer.
- Instead, it signals the running instance to exit gracefully.
- Implementation: the running instance creates a named event (e.g., `Global\GitHubBackupShutdown`). The `--shutdown` invocation opens this event and sets it. The backup loop checks the event on each iteration and exits cleanly if signaled.
- Graceful shutdown means: finish the current repo download (if in progress), write cycle summary, close log file, release mutex, and exit.

#### 11b. Ctrl+C (when in backup mode with console)

- If the tool is running in foreground mode (console attached, no `--background`), `Ctrl+C` triggers graceful shutdown.
- SIGINT handler sets a shutdown flag. The main loop checks the flag at the start of each cycle (not mid-download).
- If a download is in progress, it completes before shutdown occurs.

### 12. Source Update Tool (`update.ps1`)

A PowerShell script (`update.ps1`) is included in the project for pulling the latest source code from the private PROGRAMMING repository. This script is never overwritten by its own updates.

**Purpose:** Allows the operator to update source files on the deployment machine without downloading full tarballs or manually copying files.

**Behavior:**
- Reads the GitHub token from the local `.env` file (supports both `GITHUB_TOKEN=` and `GITHUB_BASE_URL=` formats).
- Downloads the PROGRAMMING repository archive from `agent-workspace-1157/PROGRAMMING` via the GitHub API zipball endpoint (authenticated with Bearer token header).
- Extracts the `ghb/` subdirectory from the archive.
- Copies all source files to the local directory, **protecting** `.env`, `.git`, `*.exe`, `*.zip`, `*.log`, and `update.ps1` from being overwritten.
- Reports the number of files updated and skipped.

**Usage:**
```powershell
cd D:\BACKUP\ghb
.\update.ps1
```

### 13. Non-Requirements (Explicitly Out of Scope)

- No GUI. This is a background tool with console output (when attached) and toast notifications.
- No incremental backups. Full zip each cycle, overwriting the previous.
- No encryption of the zip files.
- No upload/sync to any other location. Local backup only.
- No delta/differential downloads.
- No automatic discovery of repos. The `REPOS` array in `.env` is the single source of truth for which repos to back up.
- No proxy support. Direct connection to GitHub API only (no WPAD auto-detection).

---

## Notes

- This project was conceived as a safety net against intermittent GitHub availability issues, providing local redundancy for critical repositories.
- The tool is **not** tied to any specific GitHub account or organization. The `GITHUB_BASE_URL` (or `GITHUB_TOKEN` + `GITHUB_OWNER`) in `.env` controls the target, making the tool universally reusable for any GitHub account.
- The `.env` file must be in the same directory as the executable. The tool locates it via `GetModuleFileNameA`, not a hardcoded path.
- `BACKUP_DIR` is auto-created at startup. No manual directory setup required.
- The tool can be deployed as a Task Scheduler task on any Windows 10 machine.
