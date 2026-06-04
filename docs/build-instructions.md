# Build Instructions — GitHub Backup Script

## Prerequisites

### Compiler and Tools

- **MinGW-w64** — The C compiler for Windows. Produces 64-bit executables with no runtime dependencies. Download the latest release from [mingw-w64.org](https://www.mingw-w64.org/) or install via MSYS2.
- **Windows SDK** — Provides the WinHTTP library (`winhttp.lib`) and Windows Runtime headers needed for toast notifications and HTTP operations. Included with Visual Studio or installable standalone.

### Verified Toolchain

| Tool | Minimum Version | Purpose |
|------|---------------|---------|
| GCC (MinGW-w64) | 8.1+ | C compilation (`gcc.exe`) |
| WinHTTP | Windows 10 built-in | HTTP library (`-lwinhttp`) |
| Kernel32 | Windows 10 built-in | File I/O, process management |
| Shell32 | Windows 10 built-in | Toast notification COM interfaces |
| Ole32 | Windows 10 built-in | COM initialization |
| RuntimeObject | Windows 10 built-in | Windows Runtime activation |
| ShlObj | Windows Vista built-in | `SHCreateDirectoryExA` for recursive directory creation |
| ShLwApi | Windows 10 built-in | Shell utility functions (path manipulation) |

## Source Files

All source files are in `src/`:

| File | Lines | Purpose |
|------|-------|---------|
| `constants.h` | 439 | Compile-time constants (buffer sizes, HTTP codes, API paths, env var names, IPC names, path buffer) |
| `console.h` | 65 | Console output interface (ANSI colors, log viewer, instance detection) |
| `console.c` | 228 | Console implementation (VT100 init, color-coded output, log tail loop) |
| `logger.h` | 103 | Logging interface (log levels, init/event/error/rotate/close, console toggle) |
| `logger.c` | 216 | Logging implementation (file append, ISO timestamps, size-based rotation, stderr dev output) |
| `notify.h` | 63 | Toast notification interface (init, toast_info/success/error, cleanup) |
| `notify.c` | ~250 | Toast implementation (COM init, PowerShell bridge toast display, dual logging output) |
| `config.h` | 161 | Configuration interface (backup_config struct, parse/extract/validate functions) |
| `config.c` | ~500 | Configuration implementation (.env parser, URL token/owner extraction, repo list parser, verbose logging) |
| `network.h` | 217 | Network interface (HTTP GET, JSON parser, connectivity check, rate limit info) |
| `network.c` | ~1077 | Network implementation (WinHTTP session, API calls, zip streaming, JSON field extraction, rate limit retry) |
| `backup.h` | 113 | Backup interface (backup_result enum, single repo flow, atomic write, cycle orchestrator) |
| `backup.c` | ~405 | Backup implementation (file verification, atomic write, per-repo flow, cycle loop, verbose logging) |
| `main.c` | ~610 | Entry point (arg parsing, COM init, mutex, log viewer, network init, main loop, shutdown) |

**Total: 14 source files (7 .c, 7 .h), ~4,000 lines of C.**

## Compilation

### Compile Command (PowerShell)

```powershell
gcc -Wall -Wextra -O2 -static -o backup.exe `
    src/main.c src/backup.c src/config.c src/network.c src/logger.c src/notify.c src/console.c `
    -I src/ `
    -D_WIN32_WINNT=0x0600 -DUNICODE -D_UNICODE `
    -lwinhttp -lkernel32 -lshell32 -lole32 -lruntimeobject -lshlwapi
```

**One-liner (paste-safe for any terminal):**

```
gcc -Wall -Wextra -O2 -static -o backup.exe src/main.c src/backup.c src/config.c src/network.c src/logger.c src/notify.c src/console.c -I src/ -D_WIN32_WINNT=0x0600 -DUNICODE -D_UNICODE -lwinhttp -lkernel32 -lshell32 -lole32 -lruntimeobject -lshlwapi
```

> **Note:** `-D_WIN32_WINNT=0x0600` is required for `SHCreateDirectoryExA` to be
> declared in `<shlobj.h>`. Without it, MinGW-w64 defaults to an older
> Windows version that hides the function prototype. The source code also
> defines this macro as a fallback, but passing it via compiler flags
> ensures consistency across all translation units.

### Compiler Flags

| Flag | Purpose |
|------|---------|
| `-Wall -Wextra` | Enable all common warnings. Catch potential bugs at compile time. |
| `-O2` | Optimization level 2. Produces a fast binary without extreme compile times. |
| `-static` | Produce a fully static executable with zero runtime dependencies. |
| `-o backup.exe` | Output executable name. |
| `-I src/` | Tell the compiler where to find header files. |
| `-D_WIN32_WINNT=0x0600` | Target Windows Vista+. Required for `SHCreateDirectoryExA` declaration. |
| `-DUNICODE -D_UNICODE` | Enable Unicode character set for Windows APIs. |
| `-lwinhttp` | Link the WinHTTP library for HTTP operations. |
| `-lkernel32` | Link Kernel32 for file I/O (CreateFileA, GetFileSize) and Sleep. |
| `-lshell32` | Link Shell32 for toast notification COM interfaces and `SHCreateDirectoryExA`. |
| `-lole32` | Link Ole32 for COM initialization (CoInitializeEx). |
| `-lruntimeobject` | Link RuntimeObject for Windows Runtime activation. |
| `-lshlwapi` | Link ShLwApi for shell utility functions (PathCanonicalize, etc.). |

> **Note:** `SHCreateDirectoryExA` (used for directory creation) is provided by
> `-lshell32` on MinGW-w64. A separate `-lshlobj` flag is not needed.

### Static Build (Default)

The compile command above already includes `-static` for a fully static
executable. Remove `-static` if you prefer dynamic linking (smaller binary,
but requires MinGW runtime DLLs at runtime).

```powershell
# Dynamic build (smaller binary, requires MinGW DLLs)
gcc -Wall -Wextra -O2 -o backup.exe `
    src/main.c src/backup.c src/config.c src/network.c src/logger.c src/notify.c src/console.c `
    -I src/ `
    -D_WIN32_WINNT=0x0600 -DUNICODE -D_UNICODE `
    -lwinhttp -lkernel32 -lshell32 -lole32 -lruntimeobject -lshlwapi
```

## Updating Source Files

### Using update.ps1 (recommended)

`update.ps1` is included in the project root. It downloads the latest source from the private PROGRAMMING repo and overwrites your local files. Your `.env`, compiled binaries, zip archives, and logs are never touched.

```powershell
cd D:\BACKUP\ghb
.\update.ps1
```

The script reads the token from your `.env` file automatically (supports both `GITHUB_TOKEN=` and `GITHUB_BASE_URL=` formats). No manual token configuration needed.

### Manual update (without the script)

If you prefer to update manually, download the PROGRAMMING repo archive and extract the `ghb/` subdirectory:

```powershell
# Read token from .env, download via GitHub API, extract ghb/ contents
$token = (Get-Content .env | Where-Object { $_ -match "^GITHUB_TOKEN=" }) -split "=",2 | Select-Object -Last 1
Invoke-WebRequest -Uri "https://api.github.com/repos/agent-workspace-1157/PROGRAMMING/zipball/main" `
    -Headers @{ "Authorization" = "Bearer $token" } `
    -OutFile "$env:TEMP\ghb-update.zip" -UseBasicParsing
Expand-Archive "$env:TEMP\ghb-update.zip" "$env:TEMP\ghb-update" -Force
$extracted = (Get-ChildItem "$env:TEMP\ghb-update" -Directory).FullName
Copy-Item "$extracted\ghb\*" . -Recurse -Force -Exclude .env,*.exe,*.zip,*.log,update.ps1
Remove-Item "$env:TEMP\ghb-update.zip","$env:TEMP\ghb-update" -Recurse -Force
```

> **Important:** The token goes in the `Authorization` header, not in the URL.
> PowerShell's `Invoke-WebRequest` silently strips credentials from URLs.

## Testing

### Unit Tests

Three test files exist in `tests/`. Each tests a specific module:

```powershell
# Config module tests
gcc -Wall -Wextra -o test_config.exe tests/test_config.c src/config.c src/logger.c src/notify.c src/console.c -I src/ -D_WIN32_WINNT=0x0600 -lshell32 -lole32 -lruntimeobject

# Network module tests
gcc -Wall -Wextra -o test_network.exe tests/test_network.c src/network.c src/logger.c src/notify.c src/console.c -I src/ -D_WIN32_WINNT=0x0600 -lwinhttp -lshell32 -lole32 -lruntimeobject

# Backup module tests
gcc -Wall -Wextra -o test_backup.exe tests/test_backup.c src/backup.c src/config.c src/network.c src/logger.c src/notify.c src/console.c -I src/ -D_WIN32_WINNT=0x0600 -lwinhttp -lshell32 -lole32 -lruntimeobject
```

### Running Tests

```bash
test_config.exe    # Expected: 11 passed, 0 failed
test_network.exe   # Expected: 15 passed, 0 failed
test_backup.exe    # Expected: 11 passed, 0 failed
```

### Integration Testing

On the target Windows machine with a valid `.env` file:

1. Place `backup.exe` and `.env` in `D:\BACKUP\` (or whatever `BACKUP_DIR` is set to).
2. Run `backup.exe` from a command prompt to verify it starts, logs, and fires toasts.
3. Check `D:\BACKUP\backup.log` for structured log entries.
4. Verify that zip archives are created for each repo listed in `.env`.
5. Let the program run for at least two cycles to verify the sleep/wake behavior.

## Deployment

### File Installation

Copy `backup.exe` and `.env` to the same directory. The executable
automatically finds `.env` in its own directory — no hardcoded paths.

| File | Source | Purpose |
|------|--------|---------|
| `backup.exe` | Build output | The compiled executable |
| `.env` | From `src/env.example` | Configuration file (rename, fill in values) |

> **Both files must be in the same directory.** The executable determines
> its own location via `GetModuleFileNameA()` and looks for `.env` next
> to itself. If `.env` is not found next to the exe, the program exits with
> an error.

### .env Configuration

Create `.env` from the template in `src/env.example`. At minimum, these two fields are required:

```env
GITHUB_BASE_URL=https://YOUR_TOKEN@github.com/YOUR_OWNER/
REPOS=repo-one,repo-two,repo-three
```

Optional tuning parameters (defaults shown):

```env
BACKUP_DIR=D:\BACKUP\
CYCLE_INTERVAL_SECONDS=3600
HTTP_TIMEOUT_MS=30000
CONNECTIVITY_CHECK_TIMEOUT_MS=5000
LOG_MAX_SIZE_BYTES=1048576
```

### Task Scheduler Setup

1. Open Task Scheduler (`taskschd.msc`).
2. Click "Create Task" (not "Basic Task").
3. **General tab:**
   - Name: "GitHub Backup Service"
   - Security options: "Run whether user is logged on or not"
   - Check "Run with highest privileges"
4. **Triggers tab:**
   - New trigger: "At startup"
5. **Actions tab:**
   - New action: "Start a program"
   - Program: `D:\BACKUP\ghb\backup.exe`
   - Start in: `D:\BACKUP\ghb\`
6. **Conditions tab:**
   - Uncheck "Start the task only if the computer is on AC power"
   - Check "Start even if on batteries" (for laptops)
7. **Settings tab:**
   - Check "Allow task to be run on demand"
   - Check "Run task as soon as possible after a scheduled start is missed"
   - If the task fails, restart every 1 minute, up to 3 times

## File Structure After Deployment

```
D:\BACKUP\ghb\                     (or any directory you choose)
├── backup.exe                   (the compiled script)
├── .env                         (configuration — must be next to exe)
├── backup.log                   (created in BACKUP_DIR — auto-rotated at 1 MiB)

D:\BACKUP\agent-workspace-1157\  (BACKUP_DIR — created automatically)
├── repo-name-1.zip              (latest backup of repo-name-1)
├── repo-name-2.zip              (latest backup of repo-name-2)
└── ...
```

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Program exits immediately | `.env` missing or corrupt | Check `.env` exists in the same directory as `backup.exe` and contains `GITHUB_BASE_URL` and `REPOS` |
| No zip files created | No internet / token invalid | Check `backup.log` for errors. Verify token in `.env` is valid. |
| Toast "Invalid/expired token" | Token revoked or expired | Edit `.env` with a new token. Takes effect on next cycle. |
| Toast "Rate Limited" | Too many API calls | Normal behavior. Script sleeps until reset window automatically. |
| Toast "Disk Full" | Not enough space for zip | Free disk space. Previous backups remain intact. |
| `backup.log` missing | Log init failed (permissions) | Check write permissions on `BACKUP_DIR`. |
