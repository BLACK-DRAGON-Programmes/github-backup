# GitHub Backup (ghb)

A lightweight, zero-dependency Windows utility that automatically backs up specified GitHub repositories as local zip archives. Written in C with WinHTTP for native Windows execution, designed as a stepping stone to a future NASM assembly translation.

## What It Does

- Runs as a background Windows scheduled task (or standalone daemon)
- Downloads `.zip` archives of repositories you specify
- Uses atomic writes (download to temp, verify, rename) so you never lose a good backup
- Fires Windows toast notifications for every event (startup, cycle start, per-repo success with file size, errors, rate limits, shutdown)
- Respects GitHub API rate limits with automatic sleep-until-reset retry
- Reads configuration fresh every cycle — no restart needed for changes
- Immediate shutdown on `q` key (aborts mid-download, cleans up, exits within ~10ms)

## Status

**v0.02.0** — C implementation complete. All 146 unit tests passing. Verified in production: 6/6 repositories backed up successfully (248 MB + 3.8 MB + 2.5 MB + 4.1 MB + 307 MB + 260 KB). Toast notifications working. Immediate shutdown working. Compiles with MinGW-w64 on Windows. CI pipeline on GitHub Actions runs all tests on every push.

NASM assembly translation planned as the next phase.

## Quick Start

### Prerequisites

- **Windows 10** (or later)
- **MinGW-w64** (GCC 13+) with Windows SDK headers
- A **GitHub personal access token** with `repo` scope

### Build

```bash
gcc -Wall -Wextra -O2 -static -o backup.exe \
    src/main.c src/backup.c src/config.c src/network.c \
    src/logger.c src/notify.c src/console.c \
    -I src/ \
    -D_WIN32_WINNT=0x0600 -DUNICODE -D_UNICODE \
    -lwinhttp -lkernel32 -lshell32 -lole32 -lruntimeobject -lshlwapi -ladvapi32
```

See `docs/compile.md` for the full compilation guide, static build options, and troubleshooting.

### Configure

1. Copy `src/env.example` to the same directory as `backup.exe` and rename to `.env`
2. Fill in your values:

```env
GITHUB_BASE_URL=https://<YOUR_TOKEN>@github.com/<OWNER>/
REPOS=repo-one,repo-two,repo-three
```

3. (Optional) Adjust tuning parameters — see `env.example` for all options with defaults

### Run

Double-click `backup.exe` — it spawns the daemon (headless) and opens the viewer (ANSI color log tail). Press `q` in the viewer to shut down the daemon immediately.

**CLI flags:**
- `backup.exe` — default: spawn daemon + open viewer
- `backup.exe --daemon` — run as headless daemon only
- `backup.exe --shutdown` — signal a running daemon to exit
- `backup.exe --register` — register as a Windows Task Scheduler task (runs at system startup)
- `backup.exe --unregister` — remove the Task Scheduler entry
- `backup.exe --status` — check if the Task Scheduler task is registered

### Run as a Scheduled Task

Either use `backup.exe --register` (automatic), or manually:

1. Open **Task Scheduler** on Windows
2. Create a task that runs `backup.exe --daemon` at system startup
3. Set it to run whether the user is logged in or not, with highest privileges
4. The daemon runs continuously, backing up on a configurable cycle (default: every hour)

## Configuration Reference

All runtime configuration lives in the `.env` file (never committed to version control).

| Variable | Required | Default | Purpose |
|----------|----------|---------|---------|
| `GITHUB_BASE_URL` | Yes | — | GitHub token + target org URL (trailing slash required) |
| `GITHUB_TOKEN` | No | — | Standalone token (alternative to URL), must be `ghp_` prefix |
| `GITHUB_OWNER` | No | — | Target org (used with GITHUB_TOKEN) |
| `REPOS` | Yes | — | Comma-separated list of repos to back up |
| `BACKUP_DIR` | No | `D:\BACKUP\` | Where to store zips and logs |
| `CYCLE_INTERVAL_SECONDS` | No | `3600` | Seconds between backup cycles (minimum: 60) |
| `HTTP_TIMEOUT_MS` | No | `30000` | HTTP request timeout |
| `CONNECTIVITY_CHECK_TIMEOUT_MS` | No | `5000` | Pre-cycle internet check timeout |
| `SHUTDOWN_CHECK_INTERVAL_MS` | No | `1000` | Shutdown polling interval during sleep |
| `LOG_MAX_SIZE_BYTES` | No | `1048576` | Log rotation threshold (1 MiB) |

## Architecture

- **Dependency Injection** — all cross-module calls go through function pointer tables (`logger_ops`, `notify_ops`, `network_ops`) in `ghb_context`. Swappable backends (e.g., GitLab) without changing consumer code.
- **Interface Decoupling** — consumers include minimal interface headers (`*_iface.h`) instead of full module headers.
- **Two-process model** — daemon (headless, `CREATE_NO_WINDOW`) + viewer (ANSI console). Closing the viewer doesn't stop the daemon. Press `q` to shut down.
- **Atomic writes** — the old backup is never deleted before the new one is verified on disk. Uses `MoveFileExA` with `MOVEFILE_REPLACE_EXISTING` on Windows for true filesystem-level atomicity.
- **Immediate shutdown** — `q` key aborts the current download immediately (within ~10ms), cleans up the temp file, and exits. Old backups remain intact.
- **Toast notifications** — static PowerShell scripts in `toasts/` invoked via `powershell.exe -File`. Custom AUMID registered at runtime. Fire-and-forget (no WinRT event handlers — they silently fail on PS 5.1).
- **No accumulation** — one zip per repo, always the latest
- **Loud errors** — every failure gets a toast notification and a log entry; nothing fails silently
- **Fresh config every cycle** — edit `.env`, and the next cycle picks up the changes automatically
- **Zero runtime dependencies** — single static `.exe`, no installer, no framework

## Project Structure

```
├── README.md              # This file
├── LICENSE                # MIT License
├── spec.md                # Full project specification
├── update.ps1             # Source update tool — pulls latest from repo
├── test-toast.ps1         # Standalone toast notification test
├── .gitignore
├── .github/workflows/ci.yml   # CI pipeline — compile & test on push
├── src/                   # Source code (C / WinHTTP)
│   ├── main.c             # Entry point — startup, main loop, CLI flags
│   ├── config.h / .c      # .env parsing, URL extraction, validation
│   ├── network.h / .c     # WinHTTP calls, JSON parsing, rate limits
│   ├── backup.h / .c      # Download, verify, atomic write
│   ├── logger.h / .c      # Append-only log with size-based rotation
│   ├── notify.h / .c      # Windows toast notifications (PowerShell bridge)
│   ├── console.h / .c     # ANSI color viewer, log tailing
│   ├── constants.h        # Compile-time constants
│   ├── context.h          # Dependency injection container
│   ├── logger_iface.h     # Logger DI interface
│   ├── notify_iface.h     # Notify DI interface
│   ├── network_iface.h    # Network DI interface
│   └── env.example        # Configuration template
├── toasts/                # Static PowerShell toast scripts
│   └── show-toast.ps1     # Generic toast script (takes -Title, -Message)
├── tests/                 # Unit tests (146 tests total)
│   ├── test_config.c      # Config parsing, validation (49 tests)
│   ├── test_network.c     # JSON parser, rate limits (30 tests)
│   ├── test_backup.c      # File verification, atomic write (14 tests)
│   ├── test_logger.c      # Log init, events, rotation (17 tests)
│   ├── test_notify.c      # Toast lifecycle (9 tests)
│   ├── test_console.c     # Console output, ANSI colors (15 tests)
│   └── test_main.c        # Main.c static functions (12 tests)
├── docs/                  # Documentation
│   ├── compile.md         # Compilation and deployment guide
│   ├── flow.md            # Data flow diagram
│   ├── nasm-notes.md      # C-to-NASM translation reference
│   └── user-guide.md      # User-facing guide
├── dec/                   # Architecture decision records
│   ├── 001.md             # Language choice (C, MinGW-w64)
│   ├── 002.md             # HTTP library (WinHTTP)
│   ├── 003.md             # Log rotation (delete-and-restart)
│   ├── 004.md             # Notification system (Windows toasts)
│   ├── 005.md             # Atomic backup writes
│   ├── 006.md             # Single-instance, viewer, shutdown, console
│   ├── 007.md             # Two-process architecture (daemon + viewer)
│   └── 008.md             # Immediate shutdown on 'q' (spec override)
└── build/                 # Compiled binary output (empty by default)
```

## Testing

Unit tests are located in `tests/` and can be compiled independently. CI runs all tests on every push via GitHub Actions.

```bash
# Build and run all tests (Linux):
make test

# Or compile individually:
gcc -Wall -Wextra -o test_config tests/test_config.c src/config.c src/logger.c src/notify.c src/console.c -I src && ./test_config
gcc -Wall -Wextra -o test_network tests/test_network.c src/network.c src/logger.c src/notify.c src/console.c -I src && ./test_network
gcc -Wall -Wextra -o test_backup tests/test_backup.c src/backup.c src/config.c src/network.c src/logger.c src/notify.c src/console.c -I src && ./test_backup
gcc -Wall -Wextra -o test_logger tests/test_logger.c src/logger.c src/notify.c src/console.c -I src && ./test_logger
gcc -Wall -Wextra -o test_notify tests/test_notify.c src/notify.c src/logger.c src/console.c -I src && ./test_notify
gcc -Wall -Wextra -o test_console tests/test_console.c src/console.c src/logger.c src/notify.c src/config.c src/network.c src/backup.c -I src && ./test_console
# test_main uses #include "main.c" with GHB_TEST_BUILD guard:
gcc -Wall -Wextra -o test_main tests/test_main.c src/logger.c src/notify.c src/config.c src/network.c src/backup.c src/console.c -I src && ./test_main
```

**146 tests, all passing.**

## License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.

You are free to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of this software. The only requirement is that the copyright notice and license text be included in all copies or substantial portions of the software.
