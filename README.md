# CYD Ham Dashboard

A HamClock-inspired ham radio dashboard for the ESP32-2432S028R Cheap Yellow Display.

This is a fork of [HenrysCat/esp32-cyd-ham-dashboard](https://github.com/HenrysCat/esp32-cyd-ham-dashboard), developed independently. See [Credits](#credits-and-license).

**[Flash the original project in your browser with its Web Flasher](https://henryscat.github.io/)** — note that this serves the upstream firmware, not the builds from this repository.

It provides a touch-controlled 320x240 landscape dashboard with UTC/local time, HamQSL propagation data, a greyline map, DX spots, a DXpedition watchlist, Wi-Fi setup, and a local web settings page.

https://github.com/user-attachments/assets/84a32ac5-e0f6-4b6f-89ae-bf321e0d997d

## Features

- ESP32-2432S028R / CYD ILI9341 display support
- XPT2046 touch navigation
- Six dashboard pages:
  - Clock
  - HF Propagation from HamQSL
  - VHF Conditions from HamQSL
  - Greyline map with QTH marker, sun marker, terminator, sunrise/sunset, and day/night status
  - DX spots from JSON and/or a persistent Telnet DX Cluster connection
  - DX Watch: monitors a list of callsigns and alerts when one is spotted
- Captive portal Wi-Fi setup, which automatically switches off a few seconds after the device confirms it has joined your Wi-Fi network (it can be switched back on from the web settings page if you need it again)
- Local web settings page on the device IP
- Hold the BOOT button on the back of the board for 5 seconds to factory reset all settings
- Optional mDNS address: `http://cyd-ham.local/`
- NTP time sync
- Configurable callsign, locator, timezone, data URLs, refresh intervals, watched callsigns, and brightness
- Settings stored in ESP32 non-volatile preferences
- No LVGL, SD card, or external filesystem required

## Hardware

Tested target:

- ESP32-2432S028R “Cheap Yellow Display”
- ESP32-WROOM based board
- ILI9341 320x240 TFT
- XPT2046 resistive touch controller

The included TFT_eSPI setup uses this common CYD wiring:

| Signal | GPIO |
| --- | ---: |
| TFT MISO | 12 |
| TFT MOSI | 13 |
| TFT SCLK | 14 |
| TFT CS | 15 |
| TFT DC | 2 |
| TFT RST | -1 |
| TFT backlight | 21 |
| Touch SCLK | 25 |
| Touch MOSI | 32 |
| Touch MISO | 39 |
| Touch CS | 33 |
| Touch IRQ | 36 |

Some CYD variants use different pins. If the display is blank, white, mirrored, or touch is wrong, check your board revision and adjust `include/User_Setup.h` and the touch constants in `src/dashboard_display.cpp`.

### Setup hotspot and factory reset

On first boot (or whenever no Wi-Fi is configured), the device broadcasts a `CYD-HamClock-Setup` access point (password `hamclock`) so you can join it and open the captive portal to enter your Wi-Fi details. Once the device confirms it has joined your network, the hotspot automatically switches off. To bring it back later — for example to reach the settings page again without your router — check "Keep the setup hotspot switched on" in the Station section of the web settings page.

The board's BOOT button (GPIO0, on the back next to the USB connector) doubles as a factory reset button: hold it down for 5 seconds while the dashboard is running to wipe all saved settings (Wi-Fi credentials, callsign, locator, timezone, data sources, brightness) and reboot to defaults. The screen shows a countdown while the button is held; release early to cancel.

## Flashing A Release Binary

Every release carries one image per display driver, and they are not interchangeable:

| Asset | Board |
| --- | --- |
| `cyd-ham-dashboard-ili9341.bin` | ESP32-2432S028R with the ILI9341 panel (`esp32-2432s028r`) |
| `cyd-ham-dashboard-st7789.bin` | ESP32-2432S028R with the ST7789 panel (`esp32-2432s028r-st7789`) |

Flashing the wrong one leaves a garbled screen. Nothing is damaged, but the only way back is another flash over USB. If you do not know which panel your board has, flash the ILI9341 image first: it is the more common variant, and a garbled display tells you to try the other.

Install `esptool`:

```sh
python -m pip install esptool
```

Put the board into normal USB flashing mode, then flash the release binary. Replace `COM5` and the filename as needed:

```sh
esptool.py --chip esp32 --port COM5 --baud 460800 write_flash -z 0x10000 cyd-ham-dashboard-ili9341.bin
```

If the release includes bootloader and partition binaries, use the release instructions for those exact offsets. For PlatformIO-built firmware, the application binary normally goes at `0x10000`.

After flashing, the board will start its setup access point if Wi-Fi is not configured.

Once a release build is on the board it can update itself over the air; see [Over-The-Air Updates](#over-the-air-updates).

## Over-The-Air Updates

A board running a release build can install later releases itself, over Wi-Fi, with no USB cable. The **Firmware** section of the web settings page shows the running version and offers **Check for update** and, once an update has been found, **Install**.

The device asks GitHub for this repository's latest release, and installs it only if the release carries an asset with exactly the name its own build was compiled to expect — `cyd-ham-dashboard-ili9341.bin` or `cyd-ham-dashboard-st7789.bin`. It will never install the other variant's image, and never falls back to "whichever `.bin` is attached".

- **Checks** happen at start-up and then every six hours, and whenever you press the button. A check only records what is available; it installs nothing on its own.
- **Automatic installs** are opt-in and off by default. Tick "Install new releases automatically" in the Firmware section to have the device flash a new release as soon as it finds one.
- **While installing**, the panel shows a progress bar. The DX cluster connection, the setup hotspot and this web page all stop for the duration, so the download has the memory and sockets it needs. Do not power the device off.
- **If the install fails** the device keeps running the firmware it already has, and everything comes back. The new image is written to the spare flash slot and the board only boots from it once the whole image has been written and verified.
- **Both the check and the download use verified TLS**, against the Mozilla root CA bundle built into the ESP-IDF. Nothing else authenticates the image — the asset name only says what the release called the file — so the certificate chain is what stops a spoofed access point or a poisoned DNS answer from handing the device arbitrary code to boot. Certificate *expiry* is not checked, because this build of mbedTLS has date validation compiled out.

A build made locally rather than by CI reports its version as `dev`. That never matches a release tag, so a hand-flashed board always sees the newest release as an update rather than quietly believing it is current.

Releases are cut automatically when a pull request merges to `main`: the patch number is bumped from the newest `v*` tag and both images are built with that version stamped in. A pull request whose title contains `[skip release]` lands without shipping a release.

## Building From Source

Install:

- VS Code
- PlatformIO extension
- USB serial driver for your ESP32 board if required

Clone the repository and open it in VS Code.

Copy the example config:

```sh
copy include\app_config.example.h include\app_config.h
```

On macOS/Linux:

```sh
cp include/app_config.example.h include/app_config.h
```

You may optionally edit `include/app_config.h` before flashing, but Wi-Fi and dashboard settings can also be configured from the captive portal or local web page.

Build:

```sh
pio run
```

Upload:

```sh
pio run -t upload
```

Open Serial Monitor:

```sh
pio device monitor
```

## First Boot And Wi-Fi Setup

The ESP32 starts a setup access point while also trying to connect to saved Wi-Fi credentials.

- AP name: `CYD-HamClock-Setup`
- AP password: `hamclock`
- Portal address: `http://192.168.4.1`

Join the setup AP from a phone or computer. The captive portal may open automatically. If it does not, browse to:

```text
http://192.168.4.1
```

The setup page lets you configure:

- Callsign
- Wi-Fi SSID and password
- Timezone preset, label, and POSIX timezone rule
- Maidenhead locator
- Propagation data source
- DX source mode, JSON URL, and Telnet host/port
- DX spot modes to register
- DX watchlist callsigns and alert behaviour
- Refresh intervals
- Backlight brightness
- Display colour swap and orientation (90-degree rotate, 180-degree flip) for differently wired CYD panels

Settings are saved to ESP32 Preferences and persist after reboot.
The saved Wi-Fi password is never displayed in the settings page. Leave the password field blank to keep it, or use the checkbox to clear it for an open network.

## Local Web Settings

When connected to Wi-Fi, the same settings page is available on your LAN:

```text
http://device-ip/
```

The clock page shows the current IP address. If mDNS starts successfully, this address may also work:

```text
http://cyd-ham.local/
```

Available routes:

- `GET /` - settings and status page
- `POST /save` - save settings
- `GET /status` - JSON status, including the running version and update state
- `POST /ota/check` - ask GitHub whether a newer release exists
- `POST /ota/install` - install the release the last check found
- `POST /reboot` - restart the ESP32

This web UI is intended for a trusted local network. It does not include authentication.

## Touch Controls

- Tap left side: next page
- Tap right side: previous page
- Tap centre on HF Propagation or VHF Conditions page: manual propagation refresh
- Tap centre on DX Spots or DX Watch page: manual DX refresh

The footer shows Wi-Fi status, NTP status, and current page number.

## Dashboard Pages

### Clock

Shows:

- Large UTC time
- Configured local time
- Date
- Callsign and Maidenhead locator
- Device IP
- Uptime
- Wi-Fi/NTP/footer status

### HF Propagation

Fetches HamQSL data directly by default:

```text
https://www.hamqsl.com/solarxml.php
```

Displays:

- SFI
- A index
- K index
- X-Ray
- Sunspots
- Solar wind speed (SW) and magnetic-field Bz
- Geomag
- Noise
- Aurora
- HamQSL band condition groups for day/night
- Last update time and status

An optional JSON proxy URL can be configured from the web page.

### VHF Conditions

Same layout and data source as HF Propagation, sharing the same refresh cycle and JSON proxy setting.

Displays:

- SFI, A index, K index, X-Ray, Sunspots, Solar wind/Bz, Geomag, Noise, Aurora (identical top rows to HF Propagation)
- HamQSL VHF phenomena: VHF Aurora (with auroral latitude, when supplied), and E-Skip conditions for 6m Europe, 4m Europe, 2m Europe, and 2m North America
- Last update time and status

### Greyline

Calculates locally using the configured Maidenhead locator and NTP time.

Displays:

- Bitmap-style world map
- QTH marker
- Sun/subsolar marker
- Day/night terminator
- Night-side shading
- Sunrise, sunset, UTC time
- Location/daylight/greyline status

Changing the Maidenhead locator refreshes the Greyline calculations and map immediately; no reboot is required.

### DX Spots

Supports three source modes:

- `Auto` (default): try JSON first, then use Telnet if JSON fails or has no usable spots
- `JSON`: use only the configured JSON feed
- `Telnet`: maintain a connection to the configured DX Cluster

The default JSON endpoint is:

```text
https://web.cluster.iz3mez.it/spots.json
```

Displays recent spots with:

- Frequency
- Callsign
- Mode, inferred where possible
- UTC time
- Last update time and status

The default Telnet cluster is `dxspots.com:7300`. The configured station callsign is used for login, or `NOCALL` if no callsign is set. Both sources are configurable from the web settings page.

The device keeps the last good spot list when a refresh or connection fails. The DX page shows whether the active data came from JSON, Telnet, or the last good result.

Callsigns on the DX watchlist are drawn in green in the spot list.

#### Mode filter

**DX spot modes** in the web settings page decides which spots the device registers at all. Tick only `FT8`, for example, and everything else is dropped as it arrives:

```text
FT8   FT4   CW   SSB   USB   LSB   RTTY   SSTV   PSK
```

The filter is applied at the point a spot is read, before anything else sees it, so it covers all three sources - the JSON feed, the Telnet stream, and the history backfill - and both pages. A mode that is not ticked never reaches the spot list, the DX Watch rows, or the alerts. Ticking every box, or none, means no filtering.

**Unrecognised modes** is a separate switch, because it is the one that decides how full the page looks:

```text
[x] Include spots whose mode cannot be worked out
```

The mode is taken from the spotter's comment, falling back to the FT8 and FT4 calling frequencies when the comment says nothing useful. Many spotters label nothing, so a large share of any feed lands here as `Unknown` - in a sample of 100 spots from the default endpoint, 54 were unlabelled against 35 FT8 and 5 SSB. Leave the box ticked to keep those spots; untick it to see only spots that state their mode, accepting that a genuine SSB or CW contact whose spotter said nothing goes with them.

When a filter is active, the DX page and the DX Watch page show it on their status lines, an empty list reads `No matching spots` rather than `No spots loaded`, and a feed that held nothing in the wanted modes reports `No matching modes` rather than a parse failure. In `Auto` source mode that also hands over to Telnet, which keeps listening for the modes you asked for instead of re-reading the same unmatched feed.

The JSON feed is scanned one spot at a time. With no filter it stops at the eight spots the page holds, exactly as before. With a filter it keeps reading - the whole of a typical feed if need be, bounded by a five second budget - so the page fills with the modes you asked for rather than whatever the feed happened to send first. A narrow filter can still leave fewer than eight rows when the feed genuinely holds fewer, and those spots are correspondingly older.

### DX Watch

Monitors up to eight callsigns and announces them the moment they are spotted, so the dashboard can be left unattended while waiting for a DXpedition to come on the air.

Each watched callsign gets its own row showing whether it has been heard, and if so on what frequency, in what mode, and how long ago:

```text
Call         Freq     Mode   Age
3Y0J         14.074   FT8    4m
VP6D         not heard
FT8WW        21.023   CW     52m
```

A row is one of three states:

- Green call: heard within the active window, so the station is probably on the air now
- White call: heard, but longer ago than the active window
- Grey pattern: not heard since the device started

Configure the list in the web settings page under **DX Watchlist**. Separate callsigns with commas, spaces, or new lines:

```text
3Y0J, VP6D, FT8WW
```

Matching rules:

- `3Y0J` matches the bare call and any slash form, such as `3Y0J/MM` or `FT4/3Y0J`
- `VP6*` matches any call starting with `VP6`, for prefix hunting

Other settings:

- **Active for (minutes)**: how long after a spot the call counts as on the air. Default 15. Further spots inside this window update the row without alerting again, so a pileup does not alert repeatedly.
- **Flash the backlight on a new hit**: pulses the backlight three times. Default on.
- **Alert page jump**: switches the display to the DX Watch page on a new hit, held back for 15 seconds after a screen touch so it cannot interrupt you mid-tap. Default on.

Set **DX source mode** to `Auto` or `Telnet only` for expedition monitoring. A Telnet cluster streams every spot as it is posted, which is what makes the alert timely. JSON polling only reads the newest few spots each refresh and will miss most appearances; the DX Watch page shows a warning when JSON-only mode is selected.

Cluster-side filters set against your callsign login still apply, so the watchlist can be combined with a narrowed cluster feed.

The [mode filter](#mode-filter) applies here too: a watched call spotted on a mode you excluded is not registered and does not alert. Saving a tighter filter also clears any row already heard on a mode that is now excluded, rather than leaving it showing as on the air for the rest of the hold window — the callsign stays watched, and the next backfill can refill the row from a mode you still want. Leave the modes you are hunting on ticked, or leave the filter off entirely, when waiting on an expedition.

#### History backfill

Live sources only report spots that arrive *after* the device connects, so a station spotted an hour ago leaves its row blank. The backfill closes that gap by seeding the rows from recent history at start-up, whenever the watchlist is edited, and on an interval.

The default source is DXSummit:

```text
http://www.dxsummit.fi/api/v1/spots?limit=500
```

DXSummit's API has no per-callsign filter, only a row limit, so the device pulls a wide window and matches it on the fly. The response is streamed and parsed one object at a time in bounded chunks across loop iterations, so memory stays flat and the display keeps rendering while it downloads.

Window sizes, measured against the live feed:

| `limit` | history covered | transfer |
| --- | --- | --- |
| 100 | ~9 minutes | 24 KB |
| 500 | ~45 minutes | 123 KB |
| 1000 | ~95 minutes | 246 KB |

Backfilled rows carry the spot's own timestamp, so a spot from 30 minutes ago reads `30m`, not `now`, and never raises an alert. A live spot always takes precedence over recovered history.

DXSummit's HTTPS endpoint is unreliable; the plain `http://` URL is used deliberately. Leave the field blank to disable the backfill.

Watch state is also published in `GET /status` as `dx_watch`, with backfill status in `dx_backfill`, for polling from a phone or another host on the LAN.

## Configuration Files

### `include/app_config.h`

Local default settings. Copy from `include/app_config.example.h`.

Useful defaults:

```cpp
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"
#define MAIDENHEAD_LOCATOR "FF46"
#define PROPAGATION_JSON_URL ""
#define DX_SPOTS_URL ""
```

Values saved through the captive portal or web settings page override most defaults at runtime.
When no DX URL has been saved, `DX_SPOTS_URL` is used if configured; otherwise the public IZ3MEZ endpoint is used.

### `include/User_Setup.h`

TFT_eSPI display configuration for the CYD hardware.

PlatformIO includes it automatically via:

```ini
build_flags =
  -D USER_SETUP_LOADED=1
  -include include/User_Setup.h
```

## Timezones

The web settings page includes common timezone presets. The firmware uses POSIX timezone strings.

UK default:

```text
GMT0BST-1,M3.5.0/1,M10.5.0/2
```

UTC:

```text
UTC0
```

If your country is not listed, choose `Custom POSIX TZ` and enter a POSIX rule manually.

## Data Refresh

Default refresh intervals:

- HF Propagation: 15 minutes
- DX JSON: 5 minutes
- DX Telnet: persistent connection with reconnect attempts limited to once every 30 seconds
- Greyline calculations: once per minute
- Clock: once per second
- Firmware update check: at start-up, then every 6 hours

Propagation and DX refresh intervals can be changed in the web settings page.

## Project Structure

```text
include/
  User_Setup.h          TFT_eSPI CYD pin setup
  app_config.example.h  Example local config
  greyline_map.h        Embedded Greyline map bitmap

src/
  main.cpp              App entry point
  connectivity.*        Wi-Fi, NTP, timezone handling
  settings.*            Preferences-backed settings
  setup_portal.*        Captive portal and LAN web settings
  dashboard_display.*   TFT UI, touch, page rendering
  propagation.*         HamQSL fetch and parsing
  greyline.*            Solar and greyline calculations
  dx_spots.*            DX JSON fetch plus Telnet connection and parsing
  dx_json_scanner.h     Resumable JSON object splitter used by the DX feed scan
  dx_watch.*            Watched callsign matching, state, and alerts
  dx_backfill.*         Streams recent spot history to seed the watchlist
  ota.*                 GitHub release check and over-the-air firmware install

tools/
  ota_assets.py         Names and verifies the per-variant release assets
  test_dx_json_scanner.cpp  Host test for the JSON scanner, run in CI
  testdata/             A captured DX cluster feed for that test
```

## Notes And Limits

- The setup AP remains available while the device runs.
- The web UI is for trusted LAN use only.
- Telnet reading is non-blocking; connection attempts use a short bounded timeout. The DX JSON feed is read the same way, at most 2 KB per loop, so a mode filter that rejects most of a feed no longer holds the panel while the whole thing is scanned.
- SD card storage is not required.
- LVGL is not used.
- The embedded Greyline map uses flash space; current firmware size is close to the default app partition limit. The `min_spiffs` partition table is used, which gives two 1.9 MB app slots so an over-the-air update can be written to the spare one. The build currently sits at about 70% of one slot, roughly 64 KB of which is the root CA bundle the updater needs.
- The firmware update check is a blocking HTTPS request, unlike the rest of the networking here. It runs once every six hours and takes about a second. The install blocks the loop for its whole duration, deliberately, because the device is about to reboot.
- The DX watchlist holds up to eight callsigns. Heard state lives in RAM and is cleared by a restart.
- Watch ages use the spot's own timestamp where the source provides one, falling back to when the device saw it.
- The backfill only reaches as far back as its window. A station last spotted before that window shows as not heard until it appears again.
- A watched call absent from both the cluster and DXSummit is not being spotted anywhere; check the callsign before assuming a device fault.

## Troubleshooting

### Display is blank or white

Check that your CYD uses the same display pins as `include/User_Setup.h`.

### Touch is inaccurate

The touch calibration constants are in `src/dashboard_display.cpp`. CYD touch panels vary slightly.

### Time does not sync

Check Wi-Fi status and confirm your network allows NTP. The footer shows `NTP OK` when synced.

### Web page does not open

Check the IP shown on the clock page and browse to:

```text
http://that-ip/
```

If `cyd-ham.local` does not resolve, use the IP address instead.

### Propagation or DX shows fetch failed

Confirm Wi-Fi is connected and that your network allows HTTPS requests to the configured data source.

## Credits And License

This project is a fork of [HenrysCat/esp32-cyd-ham-dashboard](https://github.com/HenrysCat/esp32-cyd-ham-dashboard) by HenrysCat, who wrote the original dashboard, and it carries on from that work. Everything upstream builds stays theirs; the additions here are the DX Watch page, the spot history backfill, and the DX spot mode filter.

Licensed under the GNU General Public License v3.0, the same licence as the original. See [LICENSE](LICENSE).
