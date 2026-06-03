# NASM Translation Notes — GitHub Backup Script

## Overview

This document maps every C construct in the github-backup codebase to its NASM x86-64 equivalent for Windows. The eventual NASM version will be a direct, mechanical translation of the C version's logic, replacing standard library calls with Win32 API calls and C string functions with hand-written routines.

## Windows x86-64 Calling Convention

### Register Usage

| Register | Role | Preserved Across Calls |
|----------|------|----------------------|
| RCX | 1st argument | No |
| RDX | 2nd argument | No |
| R8 | 3rd argument | No |
| R9 | 4th argument | No |
| RSP+0x20, RSP+0x28, ... | 5th+ arguments | N/A (stack) |
| RAX | Return value | No |
| R10 | Scratch (used by syscall) | No |
| R11 | Scratch | No |
| RBX, RBP, RDI, RSI, R12-R15 | Callee-saved | Yes |

### Stack Alignment

The stack must be 16-byte aligned before a CALL instruction. Functions that push an odd number of 8-byte registers must subtract an additional 8 from RSP to maintain alignment.

### Shadow Space

The caller must reserve 32 bytes (4 × 8 bytes) of shadow space on the stack for the callee to use for register spilling. This space is allocated even if the function has fewer than 4 arguments.

## Module-by-Module Translation Guide

### constants.h → nasm/constants.inc

All `#define` constants become NASM `%define` or `equ` constants. Buffer sizes become assembly-time values used for stack allocation and memory operations.

| C Construct | NASM Equivalent |
|-------------|-----------------|
| `#define MAX_REPO_NAME_LEN 256` | `MAX_REPO_NAME_LEN equ 256` |
| `#define HTTP_OK 200` | `HTTP_OK equ 200` |
| `#define TEMP_FILE_SUFFIX ".zip.tmp"` | `temp_suffix db ".zip.tmp", 0` (null-terminated bytes) |
| `#define GITHUB_API_BASE "https://..."` | `github_api_base db "https://...", 0` |

String constants are declared as null-terminated byte arrays in `.data` section.

### logger.c → nasm/logger.asm

| C Function | NASM Approach |
|-----------|---------------|
| `log_init(path)` | Call `CreateFileA` to open the log file. Store handle in module-local memory. |
| `log_event(level, action, repo, status, detail)` | Build formatted string in stack buffer using hand-written string concatenation. Call `GetLocalTime` for timestamp, `wsprintfA` or manual formatting for ISO 8601, then `WriteFile`. |
| `log_error(action, repo, detail)` | Wrapper: call `log_event` with ERROR level. |
| `rotate_log(max_size)` | Call `GetFileSize`. Compare with threshold. If exceeded, call `DeleteFile` and reopen with `CreateFileA`. |
| `log_close()` | Call `CloseHandle` on the file handle. |

**Timestamp formatting:** Write a subroutine that converts SYSTEMTIME fields to ASCII digits in "YYYY-MM-DD HH:MM:SS" format. No libc strftime — hand-written numeric-to-ASCII conversion.

### notify.c → nasm/notify.asm

| C Function | NASM Approach |
|-----------|---------------|
| `notify_init()` | Call `CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)` via COM vtable dispatch. Store init status flag. |
| `toast_info/success/error()` | Build XML string (stack buffer), activate `IToastNotificationManager` via `RoGetActivationFactory`, create `IToastNotification`, call `Show`. |
| `notify_cleanup()` | Call `CoUninitialize()`. |

**COM Activation Path:**
1. `RoGetActivationFactory(CLSID, IID_IActivationFactory)` — get factory interface
2. Factory->CreateInstance(IID_IToastNotificationManager2) — get manager
3. Manager->CreateToastNotification(xml) — create toast from XML template
4. Toast->Show() — display

COM vtable dispatch uses indirect calls through interface pointers: `mov rax, [rcx]` (load vtable), `call [rax + METHOD_OFFSET]`.

### config.c → nasm/config.asm

| C Function | NASM Approach |
|-----------|---------------|
| `trim(str)` | Hand-written loop: advance pointer past spaces, then scan backwards replacing trailing spaces with null bytes. |
| `parse_env_file(config)` | Call `CreateFileA` to open .env, loop `ReadFile` for each line, scan for `=`, match key against constants using `strcmp` equivalent, store value in struct. |
| `extract_token(base_url, out)` | Find `://`, then find `@` after it. Copy bytes between them. |
| `extract_owner(base_url, out)` | Find `@github.com/`, copy bytes after the `/` until next `/` or end. |
| `parse_repos(raw, repos, count)` | Tokenize by comma (hand-written `strtok_r` replacement): scan for delimiter, trim each token, copy to output array. |
| `apply_defaults(config)` | Check each optional field for zero/empty. If zero, write default value from constants. |

**String comparison:** Write a `strcmp` replacement that compares two null-terminated byte strings byte-by-byte and returns zero for match.

### network.c → nasm/network.asm

| C Function | NASM Approach |
|-----------|---------------|
| `network_init()` | Call `WinHttpOpen` to create session. Store handle in module-local memory. |
| `check_connectivity(timeout)` | Call `WinHttpConnect` to github.com. If handle returned, internet is up. Close handle. |
| `http_get(url, token, body, size, status, rate_info, timeout)` | `WinHttpConnect` → `WinHttpOpenRequest` → `WinHttpSetOption` (timeouts) → `WinHttpSendRequest` → `WinHttpReceiveResponse` → `WinHttpQueryHeaders` (status, rate limits) → loop `WinHttpReadData` into buffer. |
| `parse_json_string(json, key, out, len)` | Hand-written scanner: `strstr` equivalent to find `"key"`, skip colon/whitespace, copy chars between quotes. |
| `parse_json_int(json, key, out)` | Same scanner, then hand-written `atoi` (scan digits, multiply-accumulate). |
| `get_default_branch(...)` | Build URL string, call `http_get`, call `parse_json_string` for `"default_branch"`. |
| `download_repo_zip(...)` | Build URL, `WinHttpOpenRequest`, `WinHttpSendRequest`, loop `WinHttpReadData` → `WriteFile` for each chunk. |
| `network_cleanup()` | `WinHttpCloseHandle` on session. |

**URL path parsing:** In C, we scan for the third slash to extract the path from a full URL. In NASM, the same logic applies: `repne scasb` or a manual byte-by-byte scan.

**Wide string conversion:** WinHTTP uses UTF-16 (wchar_t). Every path string passed to WinHTTP must be converted from ASCII to UTF-16LE. Write a `MultiByteToWideChar` wrapper or hand-convert each string (zero-extend each byte to word, append trailing zero word).

### backup.c → nasm/backup.asm

| C Function | NASM Approach |
|-----------|---------------|
| `verify_downloaded_file(path)` | Call `CreateFileA` (check for INVALID_HANDLE_VALUE), then `GetFileSize`. If size > 0, return success. |
| `cleanup_temp_file(path)` | Call `DeleteFileA`. Log result. |
| `atomic_write(temp, final)` | Call `DeleteFileA` on final (ignore "not found"), then call `MoveFileA` (Windows rename) on temp→final. |
| `backup_single_repo(...)` | Orchestration: call `get_default_branch`, construct paths (string concatenation), call `download_repo_zip`, call `verify_downloaded_file`, call `atomic_write`. Handle each error case. |
| `run_backup_cycle(config, succeeded, failed)` | Loop over `config.repo_count`, call `backup_single_repo` for each, update counters. |

**String concatenation for paths:** `backup_dir` + `repo_name` + `.zip.tmp`. In NASM: `strcpy` equivalent to copy backup_dir, then `strcat` equivalents for repo and suffix. Write hand-written memory copy routines.

### main.c → nasm/main.asm

| C Section | NASM Approach |
|-----------|---------------|
| `main()` | Entry point label (`global _main` or `global WinMain` for GUI subsystem). Call init functions in sequence, then jump to main loop. |
| Startup validation | Call `apply_defaults`, build env path, `CreateFileA` to check existence, `parse_env_file`, `log_init`, `network_init`. |
| Main loop | Infinite loop label: call `check_connectivity`, branch on result, call `parse_env_file`, call `run_backup_cycle`, build toast message, call `toast_info`, call `rotate_log`, call `Sleep` (kernel32). |
| `sleep_seconds(n)` | `mov ecx, n` → `shl ecx, 10` (ms conversion) → `stdcall Sleep` (or `mov rcx, n*1000; call Sleep`). |

## Memory Management Patterns

### Stack Allocation

C uses stack-allocated arrays for most buffers. In NASM, the same approach applies:

```nasm
; C equivalent: char path[MAX_URL_LEN];
sub rsp, MAX_URL_LEN
; ... use [rsp] as path buffer ...
add rsp, MAX_URL_LEN
```

### Heap Allocation

The program rarely uses heap allocation. The only malloc call is in `http_get` for the wide-character path string. In NASM, this can be replaced with stack allocation since the path length is bounded by MAX_URL_LEN:

```nasm
; Instead of malloc for wide path:
sub rsp, MAX_URL_LEN * 2  ; wchar_t is 2 bytes
; ... convert and use ...
add rsp, MAX_URL_LEN * 2
```

### Struct Layout

The `backup_config` struct layout must match between C and NASM. Use the C compiler's struct layout rules (no packing pragmas, default alignment). Each field's offset must be calculated manually or verified with the C compiler's `-dM` output.

## C Standard Library Replacements

| C Function | NASM Replacement | Notes |
|-----------|-----------------|-------|
| `strlen(s)` | Manual byte scan until null | `xor ecx, ecx; dec ecx; repne scasb; not ecx; dec ecx` |
| `strcpy/dst, src/` | Manual byte copy until null | `lodsb; stosb; test al, al; jnz loop` |
| `strncpy(dst, src, n)` | Byte copy with counter | Same as strcpy with loop limit |
| `strcat(dst, src)` | `strlen(dst)` + `strcpy` | Find end, then copy |
| `strcmp(a, b)` | Byte-by-byte comparison | `repe cmpsb` or manual loop |
| `strstr(haystack, needle)` | Two-level scan | Outer loop scans haystack, inner loop compares needle |
| `strtok_r(s, delim, save)` | Stateful token scanner | Track position, scan for delimiter |
| `atoi(s)` | Manual digit accumulation | `sub al, '0'; imul eax, 10; add ...` |
| `atol(s)` | Same as atoi, wider result | |
| `snprintf(buf, n, fmt, ...)` | Hand-written formatters | One per format pattern needed |
| `fopen/fclose/fread/fwrite` | `CreateFileA/CloseHandle/ReadFile/WriteFile` | Direct Win32 API |
| `remove(path)` | `DeleteFileA` | |
| `rename(old, new)` | `MoveFileA` | |
| `sleep(n)` | `Sleep(n * 1000)` | Win32 uses milliseconds |
| `malloc/free` | Stack allocation | No heap needed for this program |
| `memset(p, 0, n)` | `xor eax, eax; rep stosb` | |
| `memcpy(dst, src, n)` | `rep movsb` | Direction flag must be cleared |

## Build Sequence (NASM)

The NASM translation should follow the same dependency-ordered build sequence as the C version:

1. `constants.inc` (no dependencies)
2. `logger.asm` (constants.inc)
3. `notify.asm` (constants.inc, logger)
4. `config.asm` (constants.inc, logger, notify)
5. `network.asm` (constants.inc, logger, notify)
6. `backup.asm` (all modules)
7. `main.asm` (all modules)

### NASM Build Command

```bash
nasm -f win64 src/constants.inc -o build/constants.o
nasm -f win64 src/logger.asm -o build/logger.o
nasm -f win64 src/notify.asm -o build/notify.o
nasm -f win64 src/config.asm -o build/config.o
nasm -f win64 src/network.asm -o build/network.o
nasm -f win64 src/backup.asm -o build/backup.o
nasm -f win64 src/main.asm -o build/main.o
gcc -o build/backup.exe build/main.o build/backup.o build/config.o build/network.o build/logger.o build/notify.o -lwinhttp -lkernel32 -lshell32 -lole32 -lruntimeobject -lshlwapi
```

Note: Using GCC as the linker for NASM object files is the simplest approach. Alternatively, GoLink or MSVC link can be used.
