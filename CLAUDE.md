# CYD Ham Dashboard — working notes

ESP32 firmware for the ESP32-2432S028R "Cheap Yellow Display", built with
PlatformIO. Forked from HenrysCat/esp32-cyd-ham-dashboard and developed
independently here; GPLv3, and `upstream` remains a git remote for pulling
their changes.

## Working conventions

- Branch + PR always, never push to `main` — the `protect-main` ruleset requires
  a PR and one approving review, and Dave's admin bypass means a direct push
  would *succeed* rather than fail. Only Dave merges.
- Verification is `pio run`, which builds **both** display environments. There is
  no test suite: anything beyond a compile has to be proven on hardware or by
  replaying real data through the logic, so say plainly which of the two a change
  has had.

## Build

```sh
pio run                      # both environments
pio run -e esp32-2432s028r   # ILI9341 only
pio run -t upload            # flash over USB
```

The two environments differ **only** by which TFT_eSPI `User_Setup` header is
`-include`d: `esp32-2432s028r` (ILI9341) and `esp32-2432s028r-st7789`. A change
can compile against one and fail against the other, which is why CI builds both.

## Constraints worth knowing before you write code

- **One loop drives everything** — display redraws, the web settings server, and
  a persistent Telnet DX cluster connection. Network reads must be bounded per
  iteration (see `kMaxTelnetBytesPerLoop`, and the object-at-a-time scanners in
  `dx_spots.cpp` and `dx_backfill.cpp`) rather than draining a stream to
  completion, or the panel visibly stalls.
- **Heap is ~300 KB and fragmentation is the real enemy**, not peak usage. Prefer
  a small reusable `DynamicJsonDocument` parsed per object over one large
  document sized for a whole response.
- **Preferences keys are limited to 15 characters.** A new setting must be added
  to `AppSettings`, then loaded in `settingsBegin`, written in `saveSettings`,
  and bounds-checked in `normalizeSettings` — miss one and it silently fails to
  persist or survives as garbage.
- **`include/app_config.h` is tracked and this repo is public.** It holds
  placeholders only. Real Wi-Fi credentials go in through the captive portal or
  the web settings page, never into that file.
- Display helpers erase before they draw and cache the last string per field, so
  a changed field must go through the same `draw*Field` helper or it leaves stale
  pixels. The panel is 320x240 and several status lines are already close to the
  full width.

## CI

- `build.yml` — builds both environments on every PR.
- `claude-code-review.yml` — **a local copy of the reviewer, deliberately, not a
  caller stub.** Every private repo here calls the shared workflow in
  `davidjconnolly/shared-workflows`; a public repo cannot call a private repo's
  reusable workflow, so this one keeps its own copy, as claudemon does. Edit it
  here. The file header carries the evidence.
- The workflow filename `claude-code-review.yml` is load-bearing: the fleet's
  `reviewer-health.py` collector fetches runs by that exact filename, and a repo
  named differently cannot be watched.
- `CLAUDE_CODE_OAUTH_TOKEN` is fanned out from hermes-config's
  `sync-claude-token.yml`, which is the single source of truth. Never re-mint a
  token to fix a missing one — that invalidates the working token everywhere.
