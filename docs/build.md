# Build Sequence — GitHub Backup Script

## Overview

This document defines the order in which source files, tests, decisions, and documentation will be written for the github-backup project. The sequence is driven entirely by **dependency analysis** — a file is written only after every file it depends on has been completed and tested. This prevents writing code that references unimplemented functions or undefined constants.

The project has 7 build stages, numbered 0 through 6. Within each stage, files are ordered by internal dependency. Stages are separated by hard dependency barriers — no stage N+1 file may begin until all stage N files are complete.

---

## Dependency Map

### File Dependency Graph

```
constants.h ← (nothing — root of all dependencies)
    │
    ├── logger.h / logger.c ← constants.h
    │       │
    ├── notify.h / notify.c ← constants.h
    │       │
    ├── config.h / config.c ← constants.h, logger, notify
    │       │
    ├── network.h / network.c ← constants.h, config types, logger, notify
    │       │
    ├── backup.h / backup.c ← constants.h, config, network, logger, notify
    │       │
    └── main.c ← constants.h, config, network, backup, logger, notify
```

### Module Interaction Summary

| Module | Provides | Consumed By |
|--------|----------|-------------|
| `constants.h` | All named constants, magic numbers, file paths | Every other module |
| `logger` | Logging functions (log_event, log_error, rotate_log) | Every other module + main |
| `notify` | Toast notification functions (toast_success, toast_error, toast_info) | Every other module + main |
| `config` | .env parsing (parse_env_file, extract_token, extract_owner, parse_repos) | network, backup, main |
| `network` | HTTP requests (http_get, check_connectivity, parse_json_field) | backup, main |
| `backup` | Backup orchestration (backup_repo, download_zip, atomic_write, verify_file) | main |
| `main` | Entry point, startup validation, main loop, cycle management | (consumed by OS — nothing consumes this) |

---

## Build Stages

### Stage 0 — Pre-Development Decisions and Templates

**Files to write: 4**

| # | File | Type | Dependencies |
|---|------|------|--------------|
| 0.1 | `dec/001-language-choice.md` | Decision | None (rationale documented post-hoc) |
| 0.2 | `dec/002-http-library-choice.md` | Decision | None |
| 0.3 | `dec/003-log-rotation-strategy.md` | Decision | None |
| 0.4 | `env.example` | Config template | None |

**Rationale:** These files document architectural decisions that were implicitly made when the spec was written (C as intermediate language, WinHTTP for HTTP, delete-and-restart for log rotation). They carry no code dependencies — they are pure documentation. Writing them first establishes the project's architectural record before any code exists. The `env.example` template defines the interface contract between the operator and the script, which the config parser must implement.

**Note:** Decisions 004 (toast notifications) and 005 (atomic backup behavior) already exist. No new decisions need to be written after Stage 0.

**Validation:** After Stage 0, all 5 decision files and the config template exist in the repository. No source code has been written yet.

---

### Stage 1 — Foundation Layer

**Files to write: 3**

| # | File | Type | Dependencies |
|---|------|------|--------------|
| 1.1 | `src/constants.h` | Header | None |
| 1.2 | `src/logger.h` | Header | constants.h |
| 1.3 | `src/logger.c` | Source | constants.h, logger.h |

**Rationale:** `constants.h` is the root of the dependency tree. Every other source file includes it for buffer sizes (`MAX_REPO_NAME_LEN`, `MAX_ENV_LINE_LEN`), HTTP status codes, API endpoints, header names, and env variable name lookups. Without it, no other file can compile.

**Constant split:** Values fall into two categories. Configurable values (deployment paths, timeouts, tuning parameters) are stored in `.env` and read at runtime by the config module — changes take effect without recompilation. Non-configurable values (protocol definitions, compile-time array bounds, interface contracts) are defined here as preprocessor constants. The 5 configurable values are: `BACKUP_DIR`, `CYCLE_INTERVAL_SECONDS`, `HTTP_TIMEOUT_MS`, `CONNECTIVITY_CHECK_TIMEOUT_MS`, `LOG_MAX_SIZE_BYTES`. These are documented in `env.example`. `MAX_REPOS` remains a compile-time constant because C requires stack-allocated array sizes to be known at compile time; the actual number of repos backed up is controlled by the `REPOS` variable in `.env`.

The logger module is the first utility module because **every subsequent module logs events**. The backup cycle logs every action (success, failure, cycle start/complete). The network module logs HTTP requests and responses. The config module logs parse errors. Building the logger first means every module written afterward can immediately use it rather than accumulating placeholder log calls that must be backfilled later.

**What `constants.h` defines (non-configurable — compile-time only):**
- File extension constants (`TEMP_FILE_SUFFIX`, `FINAL_FILE_SUFFIX`)
- Network constants (`CONNECTIVITY_CHECK_URL`, `GITHUB_API_BASE`, `AUTH_HEADER_PREFIX`, `RATELIMIT_REMAINING_HEADER`, `RATELIMIT_RESET_HEADER`)
- API path constants (`API_REPOS_PATH`, `API_ZIPBALL_PATH`)
- Buffer sizes (`MAX_REPO_NAME_LEN`, `MAX_ENV_LINE_LEN`, `MAX_TOKEN_LEN`, `MAX_URL_LEN`, `MAX_LOG_ENTRY_LEN`, `MAX_REPOS`)
- HTTP status codes (`HTTP_OK`, `HTTP_NOT_FOUND`, `HTTP_UNAUTHORIZED`, `HTTP_FORBIDDEN`, `HTTP_RATE_LIMITED`)
- Environment variable name constants (`ENV_VAR_GITHUB_BASE_URL`, `ENV_VAR_REPOS`, `ENV_VAR_BACKUP_DIR`, `ENV_VAR_CYCLE_INTERVAL`, `ENV_VAR_HTTP_TIMEOUT`, `ENV_VAR_CONNECTIVITY_TIMEOUT`, `ENV_VAR_LOG_MAX_SIZE`)
- JSON field name constant (`JSON_FIELD_DEFAULT_BRANCH`)

**What `.env` defines (configurable — runtime, no recompilation needed):**
- `GITHUB_BASE_URL` — authentication token and target owner
- `REPOS` — comma-separated repository list
- `BACKUP_DIR` — deployment directory path (default: `D:\BACKUP\`)
- `CYCLE_INTERVAL_SECONDS` — backup cycle interval (default: 3600)
- `HTTP_TIMEOUT_MS` — API request timeout (default: 30000)
- `CONNECTIVITY_CHECK_TIMEOUT_MS` — connectivity check timeout (default: 5000)
- `LOG_MAX_SIZE_BYTES` — log rotation threshold (default: 1048576)

**What `logger` implements:**
- `log_init()` — Open log file, verify writeable
- `log_event(level, action, repo, status, detail)` — Write structured log entry with timestamp
- `log_error(action, repo, error_detail)` — Shorthand for error-level log
- `rotate_log()` — Check file size, delete and start fresh if over limit
- `log_close()` — Flush and close log file handle

**Validation:** After Stage 1, a minimal test program can be compiled that includes `constants.h`, initializes the logger, writes a log entry, and closes. This confirms the build toolchain (MinGW-w64) is configured, paths are correct, and the foundation layer compiles cleanly.

---

### Stage 2 — Notification Layer

**Files to write: 2**

| # | File | Type | Dependencies |
|---|------|------|--------------|
| 2.1 | `src/notify.h` | Header | constants.h |
| 2.2 | `src/notify.c` | Source | constants.h, notify.h, logger |

**Rationale:** The notification module is the second utility module, built immediately after the logger. Like the logger, it is consumed by virtually every other module — config fires toasts for corrupt `.env`, network fires toasts for rate limits and connectivity failures, backup fires toasts for per-repo success/failure, and main fires toasts for cycle start/complete. Building it second ensures all subsequent modules can use both logging and notifications from the start.

The notify module depends on the logger because every toast event should also be logged (dual output). This is not a spec requirement, but it follows Coding Standard #40 (Comprehensive Logging) — "everything must be logged."

**What `notify` implements:**
- `notify_init()` — Initialize COM for toast notifications (Windows requirement)
- `toast_info(title, message)` — Informational toast (cycle start, connectivity OK)
- `toast_success(repo, message)` — Success toast (repo backed up)
- `toast_error(title, message)` — Error toast (download failed, rate limited, etc.)
- `notify_cleanup()` — Release COM resources

**Why notify before config:** Config parsing can fail (corrupt `.env`), and that failure must fire a toast before the script exits. If notify were written after config, the config module would have a dangling dependency on an unimplemented notification function. Writing notify first eliminates this ordering constraint.

**Validation:** After Stage 2, a test program can initialize COM, fire a toast, and log the event. This confirms Win32 toast infrastructure is accessible from the build environment.

---

### Stage 3 — Configuration Layer

**Files to write: 3**

| # | File | Type | Dependencies |
|---|------|------|--------------|
| 3.1 | `src/config.h` | Header | constants.h |
| 3.2 | `src/config.c` | Source | constants.h, config.h, logger, notify |
| 3.3 | `tests/test_config.c` | Test | constants.h, config.h |

**Rationale:** The config module is the first data-processing module. It reads `.env`, extracts the token and owner from `GITHUB_BASE_URL`, and parses the `REPOS` list. It depends on constants (paths, buffer sizes), logger (log parse errors), and notify (toast on corrupt `.env`). All three of its dependencies are complete after Stage 2.

The config module is a prerequisite for both the network module (needs token, owner, and repo list to construct API requests) and the backup module (needs the repo list to iterate). Writing it before those modules removes their dependency on unimplemented parsing functions.

**What `config` implements:**
- `parse_env_file(backup_config *config)` — Read `.env`, populate struct with all 7 variables, handle missing/corrupt file. The 7 env variables are: GITHUB_BASE_URL, REPOS, BACKUP_DIR, CYCLE_INTERVAL_SECONDS, HTTP_TIMEOUT_MS, CONNECTIVITY_CHECK_TIMEOUT_MS, LOG_MAX_SIZE_BYTES. For each missing configurable value, the config module applies a sensible default (documented in env.example) rather than failing — these are tuning parameters, not mandatory authentication fields.
- `build_env_path(const char *backup_dir, char *path_out)` — Construct the .env file path by appending ".env" to BACKUP_DIR at runtime
- `build_log_path(const char *backup_dir, char *path_out)` — Construct the log file path by appending "backup.log" to BACKUP_DIR at runtime
- `extract_token(const char *base_url, char *token_out)` — Parse token from URL authority
- `extract_owner(const char *base_url, char *owner_out)` — Parse owner from URL path
- `parse_repos(const char *repos_raw, char repos[][MAX_REPO_NAME_LEN], int *count)` — Split, trim, filter comments/blanks
- `validate_config(const backup_config *config)` — Verify all required fields are populated (GITHUB_BASE_URL and REPOS are mandatory; the 5 configurable values have defaults)

**Test scope for `test_config.c`:**
- Valid `.env` with all fields → struct populated correctly
- Token extraction from various URL formats
- Owner extraction with and without trailing slash
- Repo list parsing: single repo, multiple repos, whitespace trimming, comment skipping
- Missing `.env` file → error returned
- Corrupt `.env` (missing required fields) → error returned, toast fired

**Validation:** After Stage 3, the config module can be unit-tested in isolation. A mock `.env` file is created, the parser reads it, and assertions verify the extracted values match expectations.

---

### Stage 4 — Network Layer

**Files to write: 3**

| # | File | Type | Dependencies |
|---|------|------|--------------|
| 4.1 | `src/network.h` | Header | constants.h |
| 4.2 | `src/network.c` | Source | constants.h, network.h, logger, notify |
| 4.3 | `tests/test_network.c` | Test | constants.h, network.h |

**Rationale:** The network module handles all HTTP communication with the GitHub API. It depends on constants (API base URL, timeout values, HTTP status codes), logger (log request/response details), and notify (fire toast on rate limit). It does not directly depend on the config module — it receives token, owner, and repo name as function parameters. This design keeps the network module a pure I/O layer that knows nothing about `.env` files.

The network module is a prerequisite for the backup module, which calls network functions to resolve the default branch and download zip archives.

**What `network` implements:**
- `network_init()` — Initialize WinHTTP session and connection
- `check_connectivity()` — Lightweight HTTP/DNS check (pre-cycle)
- `http_get(const char *url, const char *token, char *response_body, int *response_code, rate_limit_info *rate_info)` — Generic HTTP GET with bearer auth
- `parse_json_string(const char *json, const char *key, char *value_out)` — Extract string field from JSON response (no external JSON library)
- `parse_json_int(const char *json, const char *key, int *value_out)` — Extract integer field from JSON response
- `get_default_branch(const char *owner, const char *repo, const char *token, char *branch_out)` — Resolve default branch for a repo
- `download_repo_zip(const char *owner, const char *repo, const char *branch, const char *token, const char *output_path)` — Download zip archive to file
- `network_cleanup()` — Close WinHTTP session

**Test scope for `test_network.c`:**
- Connectivity check returns correct boolean
- HTTP status code parsing from response
- JSON field extraction (string and integer)
- Rate limit header parsing (remaining count, reset timestamp)
- Timeout handling

**Validation:** After Stage 4, the network module can be tested against mock HTTP responses or the live GitHub API. The test verifies that status codes, JSON fields, and rate limit headers are parsed correctly.

---

### Stage 5 — Backup Logic Layer

**Files to write: 3**

| # | File | Type | Dependencies |
|---|------|------|--------------|
| 5.1 | `src/backup.h` | Header | constants.h |
| 5.2 | `src/backup.c` | Source | constants.h, backup.h, config, network, logger, notify |
| 5.3 | `tests/test_backup.c` | Test | constants.h, backup.h, config.h, network.h |

**Rationale:** The backup module is the highest-level domain logic module. It orchestrates the per-repo backup cycle: resolve default branch, download zip, verify file on disk, perform atomic write (delete old, rename new). It depends on every module written so far — config for the repo list, network for API calls, logger for logging every step, and notify for firing toasts on every outcome.

This module depends on the most code. Writing it last among the domain modules ensures all its dependencies are implemented, tested, and stable. The only file that depends on backup is `main.c`, which comes next.

**What `backup` implements:**
- `backup_single_repo(const char *owner, const char *repo, const char *token)` — Full per-repo backup flow (branch resolve → download → verify → atomic write). Returns success/failure status.
- `verify_downloaded_file(const char *file_path)` — Check file exists, non-zero size, readable
- `atomic_write(const char *temp_path, const char *final_path)` — Delete old backup (if exists), rename temp to final
- `cleanup_temp_file(const char *temp_path)` — Delete temporary file (used on failure)
- `run_backup_cycle(backup_config *config, int *succeeded, int *failed)` — Iterate over all repos, call backup_single_repo for each, track counters

**Atomic write guarantee (Decision 005):**
1. Download to `{repo}.zip.tmp`
2. Verify `.zip.tmp` (non-zero size, readable)
3. Delete old `{repo}.zip` (if exists)
4. Rename `.zip.tmp` to `{repo}.zip`

If step 2 fails, delete `.zip.tmp` and skip. Old backup untouched. If disk full during any write operation, delete `.zip.tmp`, stop cycle, old backup intact.

**Test scope for `test_backup.c`:**
- File verification with valid and invalid files
- Atomic write with no existing backup
- Atomic write with existing backup (old deleted, new renamed)
- Atomic write failure recovery (temp file cleaned up, old backup intact)
- Cycle execution with mix of successes and failures
- Rate limit interruption mid-cycle

**Validation:** After Stage 5, the complete backup pipeline can be tested end-to-end (using mock network responses). The backup module ties together config parsing, network I/O, atomic file operations, logging, and notifications.

---

### Stage 6 — Entry Point and Integration

**Files to write: 1**

| # | File | Type | Dependencies |
|---|------|------|--------------|
| 6.1 | `src/main.c` | Source | constants.h, config, network, backup, logger, notify |

**Rationale:** `main.c` is the last source file because it depends on every other module. It provides the program entry point, performs startup validation (`.env` exists and is valid), enters the main loop (connectivity check → backup cycle → sleep → repeat), and handles graceful shutdown.

`main.c` does not introduce new functionality — it wires together the modules built in Stages 1-5. It calls config to load settings, logger to initialize, notify to initialize COM, then enters the infinite loop where it calls network for connectivity, backup for the cycle, and logs/toasts the results.

**What `main` implements:**
- `main()` — Entry point
  - Call `notify_init()` and `log_init()`
  - Startup validation: check `.env` exists, parse and validate config. If invalid: toast + log + exit
  - Fire "service started" toast
  - Enter infinite loop:
    1. `check_connectivity()` — if no internet: toast + log + sleep + loop
    2. Fire "cycle start" toast with repo count
    3. `parse_env_file()` (fresh read every cycle)
    4. `run_backup_cycle()` — returns succeeded/failed counts
    5. Fire "cycle complete" toast with summary
    6. `rotate_log()` if needed
    7. Sleep for cycle interval
  - On loop exit (should never happen): `log_close()` + `notify_cleanup()`

**Validation:** After Stage 6, the entire project compiles into a single `backup.exe`. This is the first point at which the program can be run as a complete unit.

---

### Stage 7 — Documentation

**Files to write: 2**

| # | File | Type | Dependencies |
|---|------|------|--------------|
| 7.1 | `docs/compile.md` | Documentation | All source files |
| 7.2 | `docs/nasm-notes.md` | Documentation | All source files |

**Rationale:** Build instructions are written after all source files exist because they must reference actual file names, include commands, and describe the exact compilation sequence. Writing them earlier would require guessing at the final file list and compilation flags.

NASM translation notes are written last because they document how each C construct in the completed codebase maps to NASM x86-64. These notes are a reference for the eventual NASM rewrite — they cannot be written until the C code they are translating exists.

**What `compile.md` contains:**
- Prerequisites (MinGW-w64, Windows SDK)
- File list and purpose
- Compilation command (`gcc` flags, link libraries: `winhttp`, `kernel32`, `shlwapi`)
- Build sequence (which files to compile in order)
- Deployment instructions (copy `backup.exe` + `env.example` to `D:\BACKUP\` — default path, configurable via `BACKUP_DIR` in `.env`)
- Task Scheduler setup instructions
- Testing instructions

**What `nasm-translation-notes.md` contains:**
- C-to-NASM translation guide for each module
- Win32 API call conventions (x86-64 Windows calling convention)
- String handling replacements (C string functions → hand-written NASM)
- Memory management patterns (stack allocation vs. heap)
- WinHTTP API call mapping
- JSON parsing approach in assembly
- Toast notification COM interface calls in assembly

---

## Complete Build Order (Flat List)

This is the definitive sequence in which files will be written. Each entry is written only after all its dependencies are complete.

| Step | Stage | File | Depends On |
|------|-------|------|------------|
| 1 | 0 | `dec/001-language-choice.md` | (none) |
| 2 | 0 | `dec/002-http-library-choice.md` | (none) |
| 3 | 0 | `dec/003-log-rotation-strategy.md` | (none) |
| 4 | 0 | `env.example` | (none) |
| 5 | 1 | `src/constants.h` | (none) |
| 6 | 1 | `src/logger.h` | constants.h |
| 7 | 1 | `src/logger.c` | constants.h, logger.h |
| 8 | 2 | `src/notify.h` | constants.h |
| 9 | 2 | `src/notify.c` | constants.h, notify.h, logger |
| 10 | 3 | `src/config.h` | constants.h |
| 11 | 3 | `src/config.c` | constants.h, config.h, logger, notify |
| 12 | 3 | `tests/test_config.c` | constants.h, config.h |
| 13 | 4 | `src/network.h` | constants.h |
| 14 | 4 | `src/network.c` | constants.h, network.h, logger, notify |
| 15 | 4 | `tests/test_network.c` | constants.h, network.h |
| 16 | 5 | `src/backup.h` | constants.h |
| 17 | 5 | `src/backup.c` | constants.h, backup.h, config, network, logger, notify |
| 18 | 5 | `tests/test_backup.c` | constants.h, backup.h, config.h, network.h |
| 19 | 6 | `src/main.c` | all modules |
| 20 | 7 | `docs/compile.md` | all source files |
| 21 | 7 | `docs/nasm-translation-notes.md` | all source files |

**Total: 21 files** (including 2 already existing decisions: 004, 005; and 1 already existing doc: flow.md)

---

## Already Complete

The following files exist and do not need to be written again:

| File | Created In |
|------|-----------|
| `spec.md` | R16 |
| `dec/004.md` | R21 |
| `dec/005.md` | R21 |
| `docs/flow.md` | R22 |

---

## Compilation Checkpoints

After certain stages, a compilation test is performed to verify that the code written so far compiles cleanly. These are not formal build targets — they are verification steps that catch integration errors early rather than allowing them to accumulate until the final build.

| After Stage | Files Compilable | Verification |
|-------------|------------------|--------------|
| Stage 1 | `constants.h`, `logger.h`, `logger.c` | Compile logger standalone (test program that calls `log_init`, `log_event`, `log_close`) |
| Stage 2 | + `notify.h`, `notify.c` | Compile notify standalone (test program that fires a toast) |
| Stage 3 | + `config.h`, `config.c` | Compile config module (test program that parses a mock `.env`) |
| Stage 4 | + `network.h`, `network.c` | Compile network module (test program that makes an HTTP request) |
| Stage 5 | + `backup.h`, `backup.c` | Compile backup module (test program that runs a backup cycle with mock network) |
| Stage 6 | + `main.c` | **Full build** — compile all source files into `backup.exe` |

---

## Test Execution Order

Tests run immediately after the module they test is complete. Tests for independent modules can be run in parallel. The test execution order:

| Step | Test File | Tests Module | Run After |
|------|-----------|--------------|-----------|
| 1 | `tests/test_config.c` | config | Step 12 |
| 2 | `tests/test_network.c` | network | Step 15 |
| 3 | `tests/test_backup.c` | backup | Step 18 |

Logger and notify modules do not have dedicated test files in the initial plan. If testing infrastructure permits (mocking Win32 toast API, verifying log file output), test files for these modules may be added later. The priority is testing the data-processing modules (config, network, backup) which contain the most complex logic.
