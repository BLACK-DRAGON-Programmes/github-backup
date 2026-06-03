# src

Source code for the GitHub backup project. C with WinHTTP for HTTP operations on Windows. All 21 build steps complete.

## Files

| File | What |
|------|------|
| `constants.h` | Compile-time constants (buffer sizes, HTTP codes, API paths, env var names) |
| `logger.h` | Logging interface (log levels, init/event/error/rotate/close) |
| `logger.c` | Logging implementation (ISO 8601 timestamps, structured format, file rotation) |
| `notify.h` | Toast notification interface (init, toast_info/success/error, cleanup) |
| `notify.c` | Toast implementation (COM init, XML templates, dual logging, non-Windows stubs) |
| `config.h` | Configuration interface (backup_config struct, parse/extract/validate/defaults) |
| `config.c` | Configuration implementation (.env parser, URL parsing, repo list splitting) |
| `network.h` | Network interface (HTTP GET, JSON parser, connectivity check, rate limit info) |
| `network.c` | Network implementation (WinHTTP session, API calls, zip streaming, JSON extraction) |
| `backup.h` | Backup interface (backup_result enum, single repo flow, atomic write, cycle runner) |
| `backup.c` | Backup implementation (file verification, atomic write, per-repo flow, cycle loop) |
| `main.c` | Entry point (startup validation, main loop with connectivity/sleep cycle, graceful shutdown) |
| `env.example` | Runtime config template — copy to `.env` and fill in |

## Naming

Headers: `.h`. Source: `.c`. Config templates: `.example`.

## Tests

Three test files in `tests/`:
- `test_config.c` — 11 tests (token/owner extraction, repo parsing, path construction)
- `test_network.c` — 15 tests (JSON string/int parsing, buffer handling, null safety)
- `test_backup.c` — 11 tests (file verification, atomic write, temp cleanup)

Total: 37 unit tests, all passing on Linux (non-Windows code paths).

## Module Dependency Order

```
constants.h
    ├── logger.h / logger.c
    ├── notify.h / notify.c
    ├── config.h / config.c
    ├── network.h / network.c
    ├── backup.h / backup.c
    └── main.c
```
