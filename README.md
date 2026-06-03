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

C implementation complete. All 37 unit tests passing. Compiles with MinGW-w64 on Windows. NASM translation planned as the next phase.

## Project Structure

```
ghb/
├── README.md              # This file
├── spec.md                # Full project specification
├── .gitignore             # Git ignore rules
├── src/                   # Source code (C / WinHTTP)
│   ├── main.c            # Entry point — startup, main loop
│   ├── config.h / .c     # .env parsing, URL extraction, validation
│   ├── network.h / .c     # WinHTTP calls, JSON parsing, rate limits
│   ├── backup.h / .c      # Download, verify, atomic write
│   ├── logger.h / .c      # Append-only log with size-based rotation
│   ├── notify.h / .c      # Windows toast notifications
│   ├── constants.h        # Compile-time constants (buffers, HTTP codes, API paths)
│   └── env.example        # Configuration template — copy to .env and fill in
├── tests/                 # Unit tests (37 tests total)
│   ├── test_config.c      # Config parsing and extraction tests
│   ├── test_network.c     # JSON parser and null-safety tests
│   └── test_backup.c      # File verification, atomic write tests
├── docs/                  # Documentation
│   ├── build.md           # Build sequence (21 steps)
│   ├── build-instructions.md  # Compilation and deployment guide
│   ├── flow.md            # Data flow diagram
│   └── nasm-notes.md      # C-to-NASM translation reference
├── dec/                   # Architecture decision records
│   ├── 001.md             # Language choice (C, MinGW-w64)
│   ├── 002.md             # HTTP library (WinHTTP)
│   ├── 003.md             # Log rotation (delete-and-restart)
│   ├── 004.md             # Notification system (Windows toasts)
│   └── 005.md             # Atomic backup writes
└── build/                 # Compiled binary output (empty by default)
```

## Quick Start

### Prerequisites

- **Windows 10** (or later)
- **MinGW-w64** with Windows SDK headers
- A **GitHub personal access token** with `repo` scope

### Build

```bash
gcc -o backup.exe src/main.c src/config.c src/logger.c src/notify.c src/network.c src/backup.c \
    -lwinhttp -lkernel32 -lshell32 -lole32 -lruntimeobject -lshlwapi \
    -O2 -Wall -DUNICODE -D_UNICODE
```

See `docs/build-instructions.md` for the full compilation guide, static build options, and troubleshooting.

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
2. Create a **Basic Task** that runs `backup.exe` at system startup
3. Set it to run whether the user is logged in or not
4. The script runs continuously, backing up on a configurable cycle (default: every hour)

## Configuration Reference

All runtime configuration lives in the `.env` file (never committed to version control). Two groups of settings:

| Variable | Required | Default | Purpose |
|----------|----------|---------|---------|
| `GITHUB_BASE_URL` | Yes | — | GitHub token + target org URL |
| `REPOS` | Yes | — | Comma-separated list of repos to back up |
| `BACKUP_DIR` | No | `D:\BACKUP\` | Where to store zips and logs |
| `CYCLE_INTERVAL_SECONDS` | No | `3600` | Seconds between backup cycles |
| `HTTP_TIMEOUT_MS` | No | `30000` | HTTP request timeout |
| `CONNECTIVITY_CHECK_TIMEOUT_MS` | No | `5000` | Pre-cycle internet check timeout |
| `LOG_MAX_SIZE_BYTES` | No | `1048576` | Log rotation threshold (1 MiB) |

## Design Highlights

- **Atomic writes** — the old backup is never deleted before the new one is verified on disk
- **No accumulation** — one zip per repo, always the latest
- **Loud errors** — every failure gets a toast notification and a log entry; nothing fails silently
- **Fresh config every cycle** — edit `.env`, and the next cycle picks up the changes automatically
- **Zero runtime dependencies** — single static `.exe`, no installer, no framework

## Testing

Unit tests are located in `tests/` and can be compiled independently:

```bash
# Config tests
gcc -o test_config tests/test_config.c src/config.c src/logger.c src/notify.c src/network.c src/backup.c -I src -DTEST_CONFIG

# Network tests
gcc -o test_network tests/test_network.c src/config.c src/logger.c src/notify.c src/network.c src/backup.c -I src -DTEST_NETWORK

# Backup tests
gcc -o test_backup tests/test_backup.c src/config.c src/logger.c src/notify.c src/network.c src/backup.c -I src -DTEST_BACKUP
```

**37 tests, all passing:**
- `test_config`: 11 tests — token extraction, owner parsing, repo list splitting
- `test_network`: 15 tests — JSON string/int parsing, null safety, buffer handling
- `test_backup`: 11 tests — file verification, temp cleanup, atomic write

## License

This project is provided as-is for personal and educational use.
