# CYD Ham Dashboard — working notes

ESP32 firmware for the ESP32-2432S028R "Cheap Yellow Display", built with
PlatformIO. Forked from HenrysCat/esp32-cyd-ham-dashboard and developed
independently here; GPLv3, and `upstream` remains a git remote for pulling
their changes.

## Working conventions

- Branch + PR always, never push to `main` — the `protect-main` ruleset requires
  a PR and one approving review, and Dave's admin bypass means a direct push
  would *succeed* rather than fail. Only Dave merges.
- Verification is `pio run`, which builds **both** display environments, plus the
  host tests in `tools/test_*.cpp`, which CI compiles and runs in one loop —
  adding a file there is enough to get it run. They cover the JSON object
  scanner, mode inference and watchlist matching against captured feeds in
  `tools/testdata`. Everything else has to be proven on hardware or by replaying
  real data through the logic, so say plainly which of the three a change has had.
- **Inference gets scored, not asserted.** `tools/test_dx_mode_plan.cpp` grades
  the band plan against spots whose comment already states the mode, so the
  claim in the README is a measurement that CI re-checks rather than a number
  someone once wrote down. Do the same for anything else that guesses.
- **Logic worth testing belongs in an Arduino-free header.** `src/dx_json_scanner.h`
  is the pattern: no `String`, no `Stream`, all state in the struct, so the host
  test compiles the same code the firmware runs rather than a copy of it. Reach
  for this when a change would otherwise be unverifiable without a board.

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
- **A release carries one image per display variant, and a board must never
  install the other one.** `OTA_ASSET_NAME` is set per environment in
  `platformio.ini`, compiled into the image, and matched against the release
  asset name exactly — never by suffix, never "the first `.bin`". `src/ota.h`
  `#error`s if the flag is missing so a new environment cannot inherit
  another's asset, and `tools/ota_assets.py` re-checks it in CI on every PR.
  `FIRMWARE_VERSION` is stamped by CI only; local builds report `dev`.
- **`include/app_config.h` is tracked and this repo is public.** It holds
  placeholders only. Real Wi-Fi credentials go in through the captive portal or
  the web settings page, never into that file.
- Display helpers erase before they draw and cache the last string per field, so
  a changed field must go through the same `draw*Field` helper or it leaves stale
  pixels. The panel is 320x240 and several status lines are already close to the
  full width.

## CI

- `build.yml` — builds both environments on every PR.
- `claude-code-review.yml` — **a self-contained copy of the reviewer,
  deliberately, not a caller stub.** A public repo cannot call a private repo's
  reusable workflow, so this one carries its own. Edit it here; the file header
  explains the rest. Do not rename the file — external monitoring finds this
  repo's review runs by that exact filename.
- `CLAUDE_CODE_OAUTH_TOKEN` is managed centrally across repos rather than set
  here. Never mint a fresh token to fix a missing one: that invalidates the
  working token everywhere else.
