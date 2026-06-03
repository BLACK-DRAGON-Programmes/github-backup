# GitHub Organization Backup Script — Project Specification

## Overview

A generic Windows-native script that automatically backs up specified GitHub repositories to local zip archives. The script runs as a scheduled task on Windows 10, executes on a configurable cycle interval (default: 1 hour, when internet is available), and retains only the latest zip per repository. The target account, token, and list of repositories to back up are all sourced from a local `.env` file, making the script portable across any GitHub account or organization.

---

## Requirements

### 1. Location and File Structure

- **Script location:** `D:\BACKUP\` (default — configurable via `BACKUP_DIR` in `.env`, see Section 2).
- **Configuration:** A `.env` file in the same directory (`D:\BACKUP\.env`) containing the base URL and repository list (see Section 2).
- **Output:** Each repository is downloaded as a zip archive and stored in `D:\BACKUP\<repo-name>.zip`.
- **Only the latest copy** of each repository is kept. If a previous zip exists for a given repo, it is overwritten (not accumulated).

> **Note:** The directory path `D:\BACKUP\` used throughout this specification is the default value. The actual deployment directory is set by the `BACKUP_DIR` variable in `.env` (default: `D:\BACKUP\`). All path references below resolve to whatever `BACKUP_DIR` is set to.

```
D:\BACKUP\                 (or whatever BACKUP_DIR is set to)
├── backup.exe              (the compiled script)
├── .env                    (see Section 2 for format)
├── repo-name-1.zip
├── repo-name-2.zip
└── ...
```

### 2. `.env` Configuration

The `.env` file provides all runtime configuration. No values are hardcoded in the script. The file is read fresh on every execution cycle, so changes take effect on the next run without recompilation.

**Format:**

```env
GITHUB_BASE_URL=https://<YOUR_PERSONAL_ACCESS_TOKEN>@github.com/<OWNER>/
REPOS=repo-one,repo-two,repo-three
```

**Fields:**

- **`GITHUB_BASE_URL`** — The base URL for the target GitHub account. Contains the personal access token embedded in the URL authority (before `@github.com`), followed by the owner/organization path. This single string controls three things: authentication, and the target account/organization.
  - Token can be swapped by editing the token portion of the URL.
  - The owner/org can be swapped by changing the path after `github.com/`.
  - This makes the script usable for **any** GitHub account or organization — not tied to one specific org.
  - The trailing slash is required.

- **`REPOS`** — A comma-separated list of repository names to back up. Only repositories listed in this array are downloaded. Repos not in the list are ignored, even if they exist in the target account.
  - The user manually maintains this list. Adding or removing a repo name takes effect on the next cycle.
  - Whitespace around commas should be trimmed by the parser.
  - Empty values or lines starting with `#` should be ignored (for comments).

**Token rotation:** Since the token is embedded in the URL, simply edit the `.env` file and replace the old token with a new one. No recompilation needed. The script must gracefully handle an invalid/expired token (log the error and retry on the next cycle — do not crash or exit).

**Additional configurable parameters:** The `.env` file also accepts five optional tuning parameters. These have sensible defaults that match the values stated throughout this specification (e.g., `D:\BACKUP\` for the directory, 3600 for the cycle interval). They only need to be set if the defaults are not suitable for a particular deployment. All changes take effect on the next cycle without recompilation. See `env.example` for the full list with documentation and usage guidance:

| Variable | Default | Spec Reference |
|----------|---------|----------------|
| `BACKUP_DIR` | `D:\BACKUP\` | Section 1 (location), Section 5 (file paths), Section 8 (log path) |
| `CYCLE_INTERVAL_SECONDS` | 3600 (1 hour) | Section 3 (cycle interval) |
| `HTTP_TIMEOUT_MS` | 30000 (30 seconds) | Section 7 (network failure handling) |
| `CONNECTIVITY_CHECK_TIMEOUT_MS` | 5000 (5 seconds) | Section 4 (connectivity check) |
| `LOG_MAX_SIZE_BYTES` | 1048576 (1 MiB) | Section 8 (log rotation) |

**Constant architecture:** Values that are operator-configurable (deployment paths, timing, timeouts, log thresholds) live in `.env` and are read at runtime — no recompilation required. Values that are not operator-configurable (HTTP protocol status codes, GitHub API endpoints, buffer sizes required by the C compiler for stack allocation) are compile-time constants defined in `constants.h`. This split is documented in both `env.example` and `constants.h`.

### 3. Auto-Start Behavior

- The script starts automatically when the computer boots.
- **Method:** Windows Task Scheduler. A scheduled task triggers the executable at system startup (runs even if nobody is logged in).
- The script runs continuously in the background, executing a backup cycle every **1 hour** (default — configurable via `CYCLE_INTERVAL_SECONDS` in `.env`, see Section 2).
- After completing a cycle, the script sleeps for the configured interval, then checks internet connectivity and runs the next cycle.

### 4. Internet Connectivity Check

- Before each backup cycle, the script checks whether the internet is available.
- If no internet: fire a toast ("No internet detected — cycle skipped"), log, skip the cycle, sleep, and retry after the configured cycle interval (default: 1 hour).
- If internet is available: proceed with the backup cycle.
- The check should be simple and lightweight — a basic HTTP request or DNS resolution test, not a full API call.

### 5. Backup Logic

1. Read `GITHUB_BASE_URL` and `REPOS` from `.env`.
2. Parse the base URL to extract:
   - **Token:** The portion before `@github.com` (used for API authentication via `Authorization: Bearer <token>` header).
   - **Owner:** The path segment after `github.com/` (e.g., `my-organization`).
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
4. Log each action (repo name, success/failure, timestamp) to a log file.

**Note:** The script does NOT list all repos in the org and back up everything. It only downloads repos explicitly listed in the `REPOS` array. This gives full control over which repos are backed up and avoids wasting bandwidth on repos not needed.

### 6. Language and Build

- **Intermediate language:** C (compiled with MinGW-w64 on Windows).
  - Produces a single static `.exe` — no runtime dependencies, no installation, no interpreter.
  - C's Win32 API calls (WinHTTP for HTTP, Kernel32 for file I/O) map almost 1:1 to NASM Win32 calls, making the eventual NASM rewrite a mechanical translation.
  - Alternative languages (PowerShell, Go, Rust, etc.) were rejected because they operate at a higher abstraction level and would not serve as useful stepping stones to NASM.
- **Final language:** NASM assembly (x86-64, Windows calling convention).
  - The NASM version will be a direct translation of the C version's logic, replacing standard library calls with Win32 API calls and libc functions with hand-written routines.
- The script is **not** Python. This is a hard constraint.

### 7. Error Handling

- **Invalid/expired token:** Log the error, fire a toast, do not crash. Retry on the next cycle.
- **Network failure during download:** Log the failed repo, fire a toast, continue with the next repo. Do not abort the entire cycle for one failure.
- **API rate limiting:** Respect `X-RateLimit-Remaining` headers. If rate-limited, log, fire a toast, sleep until the reset window and retry.
- **Disk full:** Log the error, fire a toast, stop the current cycle. Delete any temporary file. Do not attempt partial writes. The previous backup for the current repo remains intact.
- **Corrupt `.env` file:** Log the error, fire a toast, exit gracefully (this requires manual intervention).

### 8. Logging

- A simple log file at `{BACKUP_DIR}backup.log` (default: `D:\BACKUP\backup.log`).
- Each entry: timestamp, action, repo name (if applicable), status (success/failure), error details (if any).
- Log file should be appended to, not overwritten. If it grows too large, rotate it (delete the file and start fresh — the log is ephemeral, not an archive).

### 9. Notifications (Windows Toasts)

- The script fires a Windows toast notification for **every runtime event** — not just errors. This includes:
  - **Per-repo backup success:** Repo name, timestamp, file size (if available).
  - **Per-repo backup failure:** Repo name, timestamp, error type and details.
  - **Internet connectivity:** "No internet detected — cycle skipped" toast when connectivity check fails.
  - **Rate limit hit:** "Rate limited by GitHub API — sleeping until reset" toast with reset time.
  - **Corrupt `.env`:** Toast on detection before graceful exit.
  - **Cycle start:** "Starting backup cycle for N repositories" toast.
  - **Cycle complete:** "Backup cycle complete: X succeeded, Y failed" summary toast.
- **Toast content:** All available information — action, repo name (if applicable), status (success/failure), timestamp, error details (if applicable).
- **Toast duration:** Standard Windows auto-dismiss behavior. No persistent/to-click-dismiss toasts.
- **Implementation:** Windows native toast notification API. The script runs as a background scheduled task with no console window — toasts are the only user-visible feedback mechanism.

### 10. Non-Requirements (Explicitly Out of Scope)

- No GUI. This is a background service/tool — console output only (via logging).
- No incremental backups. Full zip each cycle, overwriting the previous.
- No encryption of the zip files.
- No upload/sync to any other location. Local backup only.
- No delta/differential downloads.
- No automatic discovery of repos. The `REPOS` array in `.env` is the single source of truth for which repos to back up.

---

## Notes

- This project was conceived as a safety net against intermittent GitHub availability issues, providing local redundancy for critical repositories.
- The script is **not** tied to any specific GitHub account or organization. The `GITHUB_BASE_URL` in `.env` controls the target, making the script universally reusable for any GitHub account.
- The target account/organization is configured in `.env` and can be changed at any time without recompilation.
- The script can be deployed as a Task Scheduler task on any Windows 10 machine. The default deployment directory is `D:\BACKUP\` (configurable via `BACKUP_DIR` in `.env`).
