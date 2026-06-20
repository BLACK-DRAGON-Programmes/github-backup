# tests

Unit test files for the GitHub backup project.

## Files

| File | Tests | Count |
|------|-------|-------|
| `test_config.c` | Token extraction, owner parsing, repo list splitting, path construction, apply_defaults, validate_config, ensure_dir_exists, parse_env_file integration, input validation, token/owner precedence, edge cases, negative interval, whitespace trimming, default fallbacks, GITHUB_BASE_URL trailing slash | 49 |
| `test_network.c` | JSON string/int parsing, JSON escape sequences, null safety, buffer handling, rate_limit_info struct, network constants | 30 |
| `test_backup.c` | File verification, atomic write, temp cleanup, result codes (all distinct), content verification | 14 |
| `test_logger.c` | Log init, event writing (all levels), error shorthand, rotation, close safety, multiple entries | 17 |
| `test_notify.c` | Init/cleanup lifecycle, toast info/success/error logging, NULL safety, multiple toasts | 9 |
| `test_console.c` | Init/is_active/cleanup lifecycle, all 4 log levels, NULL argument safety (repo/detail/action/status), output gating when console inactive | 15 |
| `test_main.c` | validate_env_exists (portable), Linux stubs (signal_shutdown, register/unregister/is_registered task scheduler, check_single_instance, spawn_daemon, sleep_with_shutdown_check), check_shutdown_requested both branches. Uses `#include "main.c"` with GHB_TEST_BUILD guard | 12 |

## Status

146 unit tests, all passing on Linux (non-Windows code paths).

## Running

```bash
# Preferred: build all + run via Makefile
make test

# Or compile individually:
gcc -o test_config tests/test_config.c src/config.c src/logger.c src/console.c src/notify.c -I src/
gcc -o test_network tests/test_network.c src/network.c src/logger.c src/console.c src/notify.c -I src/
gcc -o test_backup tests/test_backup.c src/backup.c src/config.c src/network.c src/logger.c src/console.c src/notify.c -I src/
gcc -o test_logger tests/test_logger.c src/logger.c src/console.c src/notify.c -I src/
gcc -o test_notify tests/test_notify.c src/notify.c src/logger.c src/console.c -I src/
gcc -o test_console tests/test_console.c src/console.c src/logger.c src/notify.c src/config.c src/network.c src/backup.c -I src/
# test_main #includes main.c (GHB_TEST_BUILD guard excludes real main()) - do NOT list src/main.c:
gcc -o test_main tests/test_main.c src/logger.c src/notify.c src/config.c src/network.c src/backup.c src/console.c -I src/
```

See `src/README.md` for module dependency details.
