# tests

Test files for the github-backup project.

## Status

No tests written yet. Tests are created after their corresponding modules in the build sequence.

## Planned Files

Per `docs/build-sequence.md`:

| File | Tests |
|------|-------|
| `test_config.c` | `.env` parsing, validation, defaults |
| `test_network.c` | GitHub API calls, error handling, rate limits |
| `test_backup.c` | Backup flow, atomic writes, retry logic |
| `test_logger.c` | Log output, file rotation |
