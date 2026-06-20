# dec

Architectural and design decision records (ADRs) for the GitHub backup project.

## Naming

Files are numbered with a three-digit prefix (sequential). The README below indexes what each number covers.

```
NNN.md
```

## Index

| File | Decision |
|------|----------|
| `001.md` | Language choice — C with MinGW-w64 |
| `002.md` | HTTP library — WinHTTP |
| `003.md` | Log rotation — delete and restart |
| `004.md` | Adding Windows toast notifications for all runtime events |
| `005.md` | Using temp-file-then-rename pattern for backup file integrity |
| `006.md` | Single-instance, log viewer, shutdown, ANSI console, background mode |
| `007.md` | Two-process architecture (daemon + viewer) |
| `008.md` | Immediate shutdown on 'q' key — spec override (abort download, don't finish it) |
