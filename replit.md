# Time Tracker

A full-screen terminal time tracker written in C using the ncurses library.

## Overview

Tracks cumulative time spent on up to 9 named projects. Displays each project's accumulated time, percentage of total, and a colour bar graph. Updates once per second. Data is persisted in a CSV file and reloaded on startup.

## Files

| File | Description |
|------|-------------|
| `timetracker.c` | Main source file (all logic in one file) |
| `Makefile` | Build rules: `make` compiles, `make clean` removes binary |
| `timetracker.csv` | Auto-created data file (saved on exit / SIGTERM / SIGHUP) |

## Build

```bash
make
```

Requires GCC and ncurses. Compatible with Fedora 42 (GCC 14) and CentOS 7 (GCC 4.8).

```bash
# Manual compile if make is unavailable
gcc -std=c99 -Wall -O2 -o timetracker timetracker.c -lncurses
```

## Run

```bash
./timetracker
```

## Keyboard Controls

| Key | Action |
|-----|--------|
| `1`–`9` | Select / switch to project |
| `Space` | Pause / resume |
| `+` | Add a new project (prompts for name) |
| `-` | Remove the current project |
| `R` | Reset all timers to zero |
| `Q` | Quit and save |

Mouse clicks on project rows or bottom-bar buttons perform the equivalent action.

## Data Persistence

- Saved to `timetracker.csv` (quoted CSV with header `name,seconds`).
- Written on quit (`Q`), `SIGTERM`, and `SIGHUP`.
- Time is **not** accumulated while the program is not running.

## Dependencies

- `ncurses` (system package, installed via Nix)
- `gcc` (GCC, available in Replit Nix environment)
