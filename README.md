# GitHub Backup (ghb)

A lightweight, zero-dependency Windows utility that automatically backs up specified GitHub repositories as local zip archives. Written in C with WinHTTP for native Windows execution, designed as a stepping stone to a future NASM assembly translation.

## What It Does

- Runs as a background Windows scheduled task
- Downloads `.zip` archives of repositories you specify
- Uses atomic writes (download to temp, verify, rename) so you never lose a good backup
- Fires Windows toast notifications for every event (success, failure, connectivity, rate limits)
- Respects GitHub API rate limits with automatic retry
- Reads configuration fresh every cycle — no restart needed for changes

## Status

C implementation complete. All 117 unit tests passing. Compiles with MinGW-w64 on Windows. CI pipeline on GitHub Actions runs all tests on every push. NASM translation planned as the next phase.

## Project Structure

```
ghb/
├── README.md              # This file
├── spec.md                # Full project specification
├── update.ps1             # Source update tool — pulls from private repo
├── .gitignore             # Git ignore rules
├── .github/
│   └── workflows/
│       └── ci.yml         # CI pipeline — compile & test on push
├── src/                   # Source code (C / WinHTTP)
│   ├── main.c             # Entry point — startup, main loop, composition root
│   ├── config.h / .c      # .env parsing, URL extraction, validation
│   ├── network.h / .c     # WinHTTP calls, JSON parsing, rate limits
│   ├── backup.h / .c      # Download, verify, atomic write
│   ├── logger.h / .c      # Append-only log with size-based rotation
│   ├── notify.h / .c      # Windows toast notifications
│   ├── console.h / .c     # ANSI color viewer, log tailing
│   ├── constants.h        # Compile-time constants (buffers, HTTP codes, API paths)
│   ├── context.h          # Dependency injection container (ghb_context)
│   ├── logger_iface.h     # Logger DI interface (logger_ops)
│   ├── notify_iface.h     # Notify DI interface (notify_ops)
│   ├── network_iface.h    # Network DI interface (network_ops)
│   └── env.example        # Configuration template — copy to .env and fill in
├── tests/                 # Unit tests (117 tests total)
│   ├── test_config.c      # Config parsing, validation, edge cases (47 tests)
│   ├── test_network.c     # JSON parser, null-safety, rate limits (30 tests)
│   ├── test_backup.c      # File verification, atomic write, cycle logic (14 tests)
│   ├── test_logger.c      # Log init, events, rotation (17 tests)
│   └── test_notify.c      # Toast lifecycle, logging (9 tests)
├── docs/                  # Documentation
│   ├── build.md           # Build sequence (21 steps)
│   ├── compile.md         # Compilation and deployment guide
│   ├── flow.md            # Data flow diagram
│   ├── nasm-notes.md      # C-to-NASM translation reference
│   └── user-guide.md      # User-facing guide: modes, controls, configuration
├── dec/                   # Architecture decision records
│   ├── 001.md             # Language choice (C, MinGW-w64)
│   ├── 002.md             # HTTP library (WinHTTP)
│   ├── 003.md             # Log rotation (delete-and-restart)
│   ├── 004.md             # Notification system (Windows toasts)
│   ├── 005.md             # Atomic backup writes
│   ├── 006.md             # Single-instance, log viewer, shutdown, ANSI console, background mode
│   └── 007.md             # Two-process architecture (daemon + viewer)
└── build/                 # Compiled binary output (empty by default)
```

## Quick Start

### Prerequisites

- **Windows 10** (or later)
- **MinGW-w64** with Windows SDK headers
- A **GitHub personal access token** with `repo` scope

### Build

```bash
gcc -o backup.exe src/main.c src/config.c src/logger.c src/notify.c src/network.c src/backup.c src/console.c \
    -lwinhttp -lkernel32 -lshell32 -lole32 -lruntimeobject -lshlwapi \
    -O2 -Wall -DUNICODE -D_UNICODE
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

### Run as a Scheduled Task

1. Open **Task Scheduler** on Windows
2. Create a **Basic Task** that runs `backup.exe --daemon` at system startup
3. Set it to run whether the user is logged in or not
4. The script runs continuously, backing up on a configurable cycle (default: every hour)

## Configuration Reference

All runtime configuration lives in the `.env` file (never committed to version control). Two groups of settings:

| Variable | Required | Default | Purpose |
|----------|----------|---------|---------|
| `GITHUB_BASE_URL` | Yes | — | GitHub token + target org URL |
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
- **Atomic writes** — the old backup is never deleted before the new one is verified on disk. Uses `MoveFileExA` with `MOVEFILE_REPLACE_EXISTING` on Windows for true filesystem-level atomicity.
- **No accumulation** — one zip per repo, always the latest
- **Loud errors** — every failure gets a toast notification and a log entry; nothing fails silently
- **Fresh config every cycle** — edit `.env`, and the next cycle picks up the changes automatically
- **Graceful shutdown** — daemon checks for shutdown between repos, finishes current download, writes summary, exits cleanly
- **Zero runtime dependencies** — single static `.exe`, no installer, no framework

## Testing

Unit tests are located in `tests/` and can be compiled independently. CI runs all tests on every push via GitHub Actions.

```bash
# Compile and run all tests (Linux/macOS):
gcc -Wall -Wextra -o test_config tests/test_config.c src/config.c src/logger.c src/notify.c src/network.c src/backup.c src/console.c -I src && ./test_config
gcc -Wall -Wextra -o test_network tests/test_network.c src/config.c src/logger.c src/notify.c src/network.c src/backup.c src/console.c -I src && ./test_network
gcc -Wall -Wextra -o test_backup tests/test_backup.c src/config.c src/logger.c src/notify.c src/network.c src/backup.c src/console.c -I src && ./test_backup
gcc -Wall -Wextra -o test_logger tests/test_logger.c src/logger.c src/notify.c src/console.c -I src && ./test_logger
gcc -Wall -Wextra -o test_notify tests/test_notify.c src/logger.c src/notify.c src/console.c -I src && ./test_notify
```

**117 tests, all passing:**
- `test_config`: 47 tests — token extraction, owner parsing, repo list splitting, validation, edge cases
- `test_network`: 30 tests — JSON string/int parsing, escape sequences, null safety, rate limits
- `test_backup`: 14 tests — file verification, temp cleanup, atomic write, result codes
- `test_logger`: 17 tests — log init, events, rotation, multiple writes
- `test_notify`: 9 tests — init/cleanup lifecycle, toast logging

## License

This project is provided as-is for personal and educational use.
