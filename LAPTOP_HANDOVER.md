# Laptop Test Handover (Mafia Aim Assist LOS)

Date: 2026-10-05

## What changed

Implemented LOS (line-of-sight) support for Mafia 1.3 with safe fallback.

Files changed:
- src/MafiaAimLogic.c
- config/MafiaAimAssist.ini
- README.md
- .gitignore

### LOS behavior

- Target selection now checks LOS in:
  - nearest target acquisition
  - side-switch target acquisition
  - active lock maintenance
- If LOS binding fails, mod stays stable and falls back to old behavior (no wall blocking).

### Auto-scanner added

`src/MafiaAimLogic.c` now includes:
- auto-discovery of line-test thunk RVA in Game.exe text section
- auto-discovery of collision singleton from line-test callsites
- probe call validation before binding

New INI keys:
- `require_line_of_sight = 1`
- `line_test_auto_scan = 1`
- `line_test_horizontal_rva = 0` (manual override if auto-scan fails)

## Safety and privacy

- `_githubtoken.txt` is ignored in `.gitignore`.
- `mafia-reverse-engineering-export/` is ignored in `.gitignore`.

## Build status

Build succeeded after scanner integration.
Output package:
- dist/MafiaAimAssist-1.0.3.zip

## How to test on laptop

1. Pull this branch on laptop.
2. Build or use produced binaries from this branch.
3. Keep INI defaults for LOS scanner:
   - `require_line_of_sight = 1`
   - `line_test_auto_scan = 1`
   - `line_test_horizontal_rva = 0`
4. Run game and try lock-on through walls.
5. Check `%TEMP%/MafiaAimLogic.log`.

Expected log lines on success:
- `LOS auto-scan found line-test thunk RVA ...`
- `LOS auto-scan found collision singleton at ...`
- `LOS line test bound at RVA ...`

If LOS does not bind:
- Share last 100 lines of `%TEMP%/MafiaAimLogic.log`.
- Then set manual `line_test_horizontal_rva` as fallback and retest.

## Notes about repo state

- Reverse-engineering clone was used as local research input only.
- No secret/token included in commits.
