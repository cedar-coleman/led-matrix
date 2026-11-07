# Strength Tank LED wall

A 256x96 LED wall (twelve 64x32 P5 panels, 4 wide by 3 high) driven by a Waveshare
ESP32-S3-RGB-Matrix. A web console on the gym Wi-Fi manages a rotation of screens: the PR
whiteboard, a New PRs screen for each athlete who set a PR in the last few days, coaches'
announcements, uploaded images and videos, and @thestrengthtank's Instagram story and latest
posts.

The New PRs screen celebrates one athlete at a time: their handle on a gold "NEW PR" banner,
the lifts they raised (the newest three), the new weight and how much it went up, and when.
It uses the same window as the whiteboard's green highlight (Settings, 7 days by default),
shows at most 20 athletes a pass, newest PR first, and is skipped when nobody qualifies.
See `design.md` section 4d and `docs/new-prs-mockup.png`.

Announcements are typed on the console's Notices tab: up to 50, each up to 200 characters
with up to 6 new lines, optionally between a start and an end date. Each one showing today
gets a screen of its own, white on the brand teal, in the biggest letters that fit. The
editor shows each one exactly as the wall will draw it and warns about characters the wall
can't draw (they show as "?"), with a button that swaps curly quotes, dashes and "..." for
plain ones. See `design.md` section 4e and `API.md` section 6a.

Nothing here has run on the real hardware yet. Everything below "Bring-up" needs the board.

## Try the console now (no hardware)

```
python3 -I mock_server.py            # needs Pillow for the Instagram sample tiles
open http://localhost:8080
```

The mock is a stand-in for the board with the same API. It keeps its data in `mock-data/`
and starts with the 31 athletes transcribed from the chalkboard photo and one announcement. Set a 6 to 12 digit
admin PIN on first load.

## Build and flash the firmware

```
cd firmware
../.tools/pio-venv/bin/pio run -e wall                 # build
../.tools/pio-venv/bin/pio run -e wall -t upload       # flash over the board's USB-C port
../.tools/pio-venv/bin/pio device monitor              # serial log, 115200
```

The web console is compiled into the firmware, so one upload updates both. Later updates can
go over Wi-Fi (`POST /api/firmware`, see `API.md` section 15) with automatic rollback if the
new firmware isn't healthy within ten minutes.

`.tools/pio-venv` is a project-local PlatformIO; its compilers live in `~/.platformio`.
Deleting both undoes the install.

## Bring-up (Phase 0, in this order)

1. Panel order and orientation: flash the test pattern and check each panel shows its chain
   number (1 top left) with the arrow pointing up.
   `PLATFORMIO_BUILD_FLAGS="-DWALL_TEST_PATTERN" ../.tools/pio-venv/bin/pio run -e display_dev -t upload`
2. If the panels stay blank or noisy with the default driver, add `-DWALL_DRIVER_FM6126A`.
3. If the board boot-loops, change `board_build.arduino.memory_type` in `platformio.ini`
   from `opi_opi` to `qio_opi` (Waveshare's Arduino screenshot shows QIO flash; the schematic
   shows an octal-flash module).
4. Wire and measure the supplies as in the [power wiring and test guide](docs/power-wiring-guide.md):
   each supply's current on a full-white and a mid-grey screen at 25% and 100%. If one panel
   at full white draws more than about 2.5 A, the two LRS-100-5 supplies are too small: move
   to one UHP-500-5 (or LRS-200-5 / LRS-350-5) and set Settings > Power supplies to match.
   Then calibrate `PANEL_FULL_WHITE_A`.
5. Note the refresh rate the display prints at boot, and film the wall with a phone. Expect
   some banding on camera: a 768-pixel chain on one output can't refresh fast enough for
   phone cameras. Tell the gym before mounting.
6. Check the 3.3 V signal into the first panel's 74HC245 (it wants 3.5 V at 5 V supply). Keep
   that first ribbon short, or add a 74HCT245.
7. The rest of the list (TLS with the display running, RTC, LittleFS above 16 MB, OTA
   rollback, Instagram JPEGs, reset behaviour) is in `design.md` section 10.

First boot: the wall shows a setup Wi-Fi name and password. Join it from a phone, open the
setup page, set the admin PIN and the gym's Wi-Fi. The console is then at
`http://gymboard.local`. To clear a lost PIN, switch the wall off and on and, as soon as it
lights up, press and hold BOOT for 10 seconds (holding BOOT through power-on puts the
controller into programming mode instead).

## Tests

| Command | What it checks |
| --- | --- |
| `node --test tests/` | Whiteboard page fitting, Instagram, New PRs and announcement slide layout (wrapping, size, which ones show by date), power model (one or two supplies) |
| `python3 -I -m unittest discover -s tests` | The API contract on the mock, including the display schedule on an injected clock and the announcement rules in `tests/announcement_cases.json` |
| `python3 tests/run_e2e.py` | Real Chrome: media conversion, phone UI flows, screenshots in `tests/e2e/screenshots/` |
| `cd firmware && ../.tools/pio-venv/bin/pio test -e native` | C++ renderers match the browser byte for byte; firmware API logic matches the mock (announcement rules from the same case file); the rotation, including announcements by date; the schedule across daylight saving; the display idling while off |
| `WALL_URL=http://gymboard.local WALL_PIN=... python3 tests/device_contract.py --yes` | The API on a real wall. It writes data, so only run it on a fresh wall |

## Where things are

| Path | What |
| --- | --- |
| `design.md` | Hardware, power, memory, architecture, Instagram setup, build plan |
| `docs/power-wiring-guide.md` | Wiring the panels to one or two supplies, the current measurements, and the "Power supplies" setting |
| `API.md` | The exact REST contract the firmware and mock implement |
| `web/` | The console. `layout.js`, `igslides.js`, `prslides.js`, `announceslides.js`, `power.js` are the on-wall spec the firmware ports |
| `mock_server.py` | Stand-in for the board |
| `firmware/` | PlatformIO project. `include/wall_shared.h` is the interface between the two halves |
| `firmware/lib/render/` | C++ port of the renderers (host-testable) |
| `firmware/lib/netcore/` | Validation, PIN, time zones, JSON (host-testable) |
| `firmware/src/display/` | HUB75 driver, render task, GIF player, power limiter |
| `firmware/src/net/` | Wi-Fi, setup AP, web server, storage, time, Instagram, OTA |
| `athletes-from-chalkboard.csv` | Athletes transcribed from the photo. Check before loading |
| `slides/`, `whiteboard/`, `mock-assets/` | Logo slides, early whiteboard renders, Instagram samples |

## Open items

1. Check `athletes-from-chalkboard.csv` against the real board (the photo cut off a column).
2. Set up the Meta app and Instagram token (`design.md` section 8).
3. Everything in "Bring-up" above.
