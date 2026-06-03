# Data Flow — GitHub Backup Script

> **Note on configurable defaults:** The file paths shown in this document (e.g., `D:\BACKUP\`) are the default deployment location, configurable at runtime via the `BACKUP_DIR` variable in `.env`. The cycle timing shown as "1 hour" is the default interval, configurable via `CYCLE_INTERVAL_SECONDS` in `.env`. See `env.example` for the full list of configurable parameters.

## System Startup Flow

```mermaid
flowchart TD
    A["Computer Boots Up"] --> B["Windows Task Scheduler\nTriggers backup.exe\n(System Startup Trigger)"]
    B --> C["backup.exe Starts\n(Background Process, No Console)"]
    C --> D{"Startup Validation:\nDoes D:\\BACKUP\\.env exist?"}
    D -- No --> E["Toast: Config file not found\nLog: Fatal error\nEXIT (requires manual intervention)"]
    D -- Yes --> F{"Startup Validation:\nIs .env valid?\n(contains GITHUB_BASE_URL\nand REPOS with values)"}
    F -- No --> G["Toast: Corrupt .env file\nLog: Fatal error\nEXIT (requires manual intervention)"]
    F -- Yes --> H["Toast: Backup service started\nLog: Service initialized\nEnter Main Loop"]
    H --> I
```

## Main Loop — Hourly Cycle

```mermaid
flowchart TD
    I{"Internet Connectivity\nCheck\n(lightweight HTTP/DNS)"} -->|"No internet"| J["Toast: No internet detected\nLog: Cycle skipped"]
    J --> K["Sleep 1 hour"]
    K --> I

    I -->|"Internet available"| L["Toast: Starting backup cycle\nfor N repositories"]
    L --> M["Read .env file\n(fresh read every cycle)"]
    M --> N["Parse GITHUB_BASE_URL\nExtract: Token, Owner"]
    N --> O["Parse REPOS list\nTrim whitespace, skip comments"]
    O --> P["Initialize counters:\nsucceeded = 0, failed = 0"]
    P --> Q["For each repo\nin REPOS list"]
    Q --> R

    R --> S["GET /repos/{owner}/{repo}\nAuth: Bearer {token}"]
    S --> T{"HTTP Response?"}
    T -- "404 Not Found" --> U["Toast: {repo} not found\nLog: Warning\nfailed++"]
    T -- "Rate Limited\n(X-RateLimit-Remaining: 0)" --> V["Toast: Rate limited\nLog: Sleeping until reset\nParse X-RateLimit-Reset header"]
    V --> W["Sleep until reset window"]
    W --> S
    T -- "401/403 (Auth Error)" --> X["Toast: Invalid/expired token\nLog: Error\nfailed++\n(continue to next repo)"]
    T -- "200 OK" --> Y["Extract default_branch\nfrom JSON response"]
    Y --> AA
    T -- "Network Error /\nTimeout" --> AB["Toast: Network failure for {repo}\nLog: Error\nfailed++\n(continue to next repo)"]
    AB --> Q
    U --> Q
    X --> Q

    AA["GET /repos/{owner}/{repo}\n/zipball/{default_branch}\nAuth: Bearer {token}"]
    AA --> AC{"Download\nsuccessful?"}
    AC -- No --> AD["Toast: Download failed for {repo}\nLog: Error\ndelete .zip.tmp if exists\nfailed++\n(continue to next repo)"]
    AD --> Q
    AC -- "Disk Full" --> AE["Toast: Disk full\nLog: Error\ndelete .zip.tmp\nSTOP CYCLE\n(previous backup intact)"]
    AE --> AF
    AC -- Yes --> AG["Save to\nD:\\BACKUP\\{repo}.zip.tmp"]
    AG --> AH{"Verify .zip.tmp\n(non-zero size,\nreadable, accessible)"}
    AH -- "Verification Failed" --> AI["Toast: Corrupt download for {repo}\nLog: Error\ndelete .zip.tmp\nfailed++\n(previous backup intact)\n(continue to next repo)"]
    AI --> Q
    AH -- "Verification Passed" --> AJ{"Old backup\nD:\\BACKUP\\{repo}.zip\nexists?"}
    AJ -- Yes --> AK["Delete old .zip"]
    AK --> AL["Rename .zip.tmp\n→ .zip"]
    AJ -- No --> AL
    AL --> AM["Toast: {repo} backed up\nsuccessfully\nLog: Success\nsucceeded++"]
    AM --> Q
```

## Cycle Complete and Sleep

```mermaid
flowchart TD
    Q -- "All repos processed" --> AF["Toast: Backup cycle complete\n{succeeded} succeeded, {failed} failed"]
    AF --> AG["Log rotation check:\nIs backup.log too large?"]
    AG -- "Yes, too large" --> AH["Delete backup.log\nStart fresh"]
    AG -- No --> AI
    AH --> AI["Sleep 1 hour"]
    AI --> I
```

## Data Flow Summary

### Input Sources

| Source | Data | When Read |
|--------|------|-----------|
| `D:\BACKUP\.env` | `GITHUB_BASE_URL` (token + owner) | Once at startup for validation, then fresh every cycle |
| `D:\BACKUP\.env` | `REPOS` (comma-separated repo names) | Fresh every cycle |
| GitHub API | `default_branch` field | Once per repo per cycle |
| GitHub API | Zip archive bytes | Once per repo per cycle |
| GitHub API | `X-RateLimit-Remaining` header | Every API response |
| GitHub API | `X-RateLimit-Reset` header | When rate-limited |

### Output Destinations

| Destination | Data Written | When |
|-------------|-------------|------|
| `D:\BACKUP\{repo}.zip` | Repository zip archive | After successful download + verify + atomic rename |
| `D:\BACKUP\{repo}.zip.tmp` | Temporary download buffer | During download (deleted on failure or after rename) |
| `D:\BACKUP\backup.log` | Structured log entries (timestamp, action, repo, status, error) | Every event |
| Windows Toast | Notification (action, repo, status, timestamp, error details) | Every event |
| `D:\BACKUP\.env` | Never written (read-only) | N/A |

### Data Transformations

| Step | Input | Transformation | Output |
|------|-------|---------------|--------|
| Config parse | `.env` raw text | Extract token (before `@`), owner (after `github.com/`), repo list (split + trim) | Token string, owner string, repo name array |
| API call | Token + owner + repo name | Construct `Authorization: Bearer {token}` header, build endpoint URL | HTTP request |
| JSON parse | API response body | Extract `default_branch` field value | Branch name string |
| Zip download | Zip bytes from API response | Stream to temporary file on disk | `.zip.tmp` file |
| Verify | `.zip.tmp` file on disk | Check file size > 0, attempt read, check accessible | Pass/fail boolean |
| Atomic swap | `.zip.tmp` (verified) + old `.zip` (optional) | Delete old `.zip`, rename `.tmp` to `.zip` | Final `.zip` backup |

### Error Flow Branches

| Error Type | Detection Point | Script Behavior | User Feedback |
|------------|----------------|-----------------|---------------|
| `.env` missing | Startup validation | EXIT | Toast + Log |
| `.env` corrupt | Startup validation | EXIT | Toast + Log |
| No internet | Connectivity check (pre-cycle) | Skip cycle, sleep 1h | Toast + Log |
| Invalid/expired token | API response (401/403) | Continue to next repo | Toast + Log |
| Repo not found (404) | API response | Skip repo, continue | Toast + Log |
| Rate limited (429) | API response headers | Sleep until reset, retry | Toast + Log |
| Network failure | API call timeout/error | Continue to next repo | Toast + Log |
| Download failure | HTTP error / incomplete | Delete .tmp, continue | Toast + Log |
| Corrupt download | Post-download verification | Delete .tmp, continue | Toast + Log |
| Disk full | Write operation failure | Delete .tmp, stop cycle | Toast + Log |
