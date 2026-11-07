# Gym LED Wall: design mockup

Waveshare ESP32-S3-RGB-Matrix driving twelve 64x32 HUB75 panels as one 256x96 display,
with a Wi-Fi web console for a media playlist and a PR whiteboard.

Status: design draft, 2026-11-06 (API revised the same day after an IoT review; the
contract is `API.md`). Nothing built or measured yet. Items marked **VERIFY**
are assumptions that a hardware spike has to confirm before the design is final.

---

## 1. What we know and what we don't

| Known (from Waveshare docs) | Source |
| --- | --- |
| ESP32-S3-N32R16: dual core 240 MHz, 512 KB SRAM, 32 MB flash, 16 MB PSRAM, 2.4 GHz Wi-Fi | Product page |
| Vendor examples use the `ESP32-HUB75-MatrixPanel-DMA` library, with a `PANEL_CHAIN` length setting | Arduino page |
| TF card is wired as SDMMC 1-bit | Arduino page |
| Vendor SD example carries the comment "TF CARD STOPS WORKING WITH DMA DISPLAY ENABLED" | Arduino page, example 05 |
| Onboard RTC (PCF85063) with a battery header | Product page |

| Known (from the user, 2026-11-06) | Value |
| --- | --- |
| Panels | P5 indoor, 64x32, 320 x 160 mm, 1/16 scan, SMD2121, 800 cd/m², HUB75, 5 V |
| Panel power (seller spec) | Max 480 W/m², average 240 W/m²; seller suggests one 200 W supply per 6 to 7 panels |
| Wall | 1280 x 480 mm (4 x 3 panels) |
| Viewing distance | 5 to 25 ft |
| Units | kg only, in 0.5 kg steps (the chalkboard has 136.5, 250.5, 101.5) |
| Wi-Fi | Plain password network, 2.4 GHz |
| Panel chips (from photos) | Column drivers: Chipone ICN2037BP. Input buffer: 74HC245 (helps the signal survive a 12-panel chain) |

| Unknown | Why it matters | How we find out |
| --- | --- | --- |
| Does the library's default driver setting (`SHIFTREG`) work with the **Chipone ICN2037BP** column drivers? Expected yes: the ICN2037 is a plain 16-channel constant-current shift register. The chip that needs the FM6126A init sequence is the ICN2038S, a different part. Not confirmed for this library by any source found | Wrong setting means a blank or garbled panel | Spike test 2: run `07_Pixel_Mapping_Test` with `SHIFTREG`. If the panel is blank or noisy, try `FM6126A`, then lower the pixel clock |
| Row-select chip (marked "T4" on the board, part number not readable) | Some panels use row drivers that need a different addressing mode | Same test. If rows are doubled or skipped, read the chip marking |
| Whether SD reads work while the display DMA runs | Decides SD vs. internal flash for media | Spike test 3 (section 9) |
| Refresh rate with a 768-pixel-wide chain | Low refresh shows as rolling bands when members film lifts on their phones | The library computes it at init; confirm with a phone camera |
| Number of HUB75 outputs on the board (one is assumed) | Two outputs would let us split the wall into two shorter chains | Schematic in Waveshare Resources |
| Gym Wi-Fi has no client isolation (2.4 GHz and password are confirmed) | Client isolation stops phones on the same network from reaching the board's web page | During the spike, open the board's page from a phone on the gym Wi-Fi |

---

## 2. Hardware layout

### Panel arrangement and chain order

One HUB75 output feeds a single 12-panel serpentine chain. Rows 1 and 3 run left to right
and mount upright. Row 2 runs right to left and mounts **upside down**, so every
row-to-row hop uses a short ribbon cable instead of one that crosses the whole wall.

```
            256 px (4 panels x 64)
   ┌────────┬────────┬────────┬────────┐
   │   1 →  │   2 →  │   3 →  │   4 →  │──┐   row 1, upright
   ├────────┼────────┼────────┼────────┤  │
┌──│ ← 8 ⟲  │ ← 7 ⟲  │ ← 6 ⟲  │ ← 5 ⟲  │◄─┘   row 2, rotated 180°
│  ├────────┼────────┼────────┼────────┤        96 px (3 x 32)
└─►│   9 →  │  10 →  │  11 →  │  12 →  │      row 3, upright
   └────────┴────────┴────────┴────────┘
    ▲
    ESP32-S3 HUB75 out → panel 1 input
```

In firmware, `VirtualMatrixPanel` with `CHAIN_TOP_LEFT_DOWN` maps this to one 256x96
canvas, so the rest of the code never deals with panel coordinates.

### Power (the part most likely to go wrong)

The panels draw their current from a separate 5 V supply. That current never passes
through the ESP32 board.

Figures from the seller spec, for a 1280 x 480 mm wall (0.614 m²):

| Item | Spec | Notes |
| --- | --- | --- |
| Max, 12 panels | 480 W/m² x 0.614 m² ≈ 295 W ≈ 59 A at 5 V | Full white at full brightness. The software cap keeps us well under it |
| Average, 12 panels | 240 W/m² x 0.614 m² ≈ 147 W ≈ 30 A | Seller's "average content" figure |
| Supply | **Two Mean Well LRS-100-5 (5 V, 18 A, 90 W each)**: A feeds chain panels 1 to 6 (top row plus the right half of the middle row), B feeds panels 7 to 12 | Below the seller's "200 W per 6 to 7 panels", so the software power cap is required, not optional. Budget is 15.3 A per supply (85% of 18 A). At 50% brightness the PR whiteboard draws about 2.5 A per supply; an all-white image would need about 14.7 A, and the cap lets all-white images reach about 52% |
| Margin check | By the seller's own **average** figure (240 W/m²), 6 panels draw about 14.7 A, against the 15.3 A budget per supply. That is thin: the average figure is the seller's, not a measurement | In the spike, measure one panel at full white. If it draws more than about 2.5 A, move to LRS-200-5 (40 A) or LRS-350-5 (60 A) supplies before building |
| Supply wiring | Closed-frame supplies have exposed AC terminals: mount them in a grounded enclosure and connect the FG (earth) terminal. Trim V.ADJ so the panels see about 5.1 V under load. A 20 A fuse on each DC feed protects the wiring | Tie the two supplies' grounds together, never their +5 V outputs |
| Distribution | Each panel's own power pigtail to a bus bar or distribution block; 14 AWG from each supply to its block | Don't daisy-chain power through panel pigtails |
| Ground | The ESP32 board's GND tied to panel ground | Without a shared ground the data lines glitch |
| Software cap | Brightness setting; a per-file cap for media (the console measures the brightest frame's current on upload, `peak_a`, and the wall plays the whole slide at `slideBrightness(peak_a, setting, power)`); and a per-frame limiter as a backstop that dims any frame that would push a supply past its budget (15.3 A for each of today's two supplies; the "Power supplies" setting, `settings.power`, sets one or two supplies, their rating and the budget share; `web/power.js` is the model the firmware ports) | Keeps a bright image from tripping a supply's overload protection, which shows as flicker or a reset. Capping the whole slide keeps a video's brightness steady instead of pumping frame to frame |

### Readability at 5 to 25 ft (P5)

| Text | Height on the wall | Comfortable (~10 ft per inch) | Max legible (~30 ft per inch) |
| --- | --- | --- | --- |
| Capitals and digits, 7 px | 35 mm (1.4 in) | ~14 ft | ~41 ft |
| Lowercase body, 5 px | 25 mm (1.0 in) | ~10 ft | ~30 ft |

These distances come from the sign-industry rule of thumb, not a measurement. The whole
5 to 25 ft range is within legible distance for the dense table, so it is the default layout.
Viewers at the far end are past the "comfortable" mark for lowercase handles. Confirm on
the real wall, and switch to the large layout (section 4c) if handles are hard to read
from the back. At 5 ft, photos and video will show individual dots, which is normal for
P5. A diffuser sheet softens them.

The LED wall (1.28 m wide) is smaller than the chalkboard it replaces, so the table shows
9 athletes at a time and pages through them instead of showing everyone at once.

### Bill of materials (outside the board)

- 12 matching 64x32 panels, **bought from one batch** (mixed batches often differ in scan
  type and color balance)
- Fused distribution (20 A per feed), 14 AWG wire, ring terminals, a grounded enclosure for
  the two LRS-100-5 supplies (see Power above)
- HUB75 ribbons: panels usually ship with short ones; rows need 2 to 3 slightly longer ones
- Mounting: 2020 aluminium extrusion or a plywood backer with magnetic panel screws
- Optional: smoked acrylic or diffuser film (softens pixels, raises perceived contrast)
- CR1220/SH1.0 RTC battery (keeps time across power cuts)

---

## 3. Memory and refresh budget

Display memory is the tightest constraint on this board.

The DMA library keeps a bit-plane buffer in internal SRAM, roughly:

```
chain_width_px x row_pairs x color_depth_bits x 2 bytes
= 768 x 16 x 8 x 2  ≈ 192 KB   (8-bit color, single buffer)
= 768 x 16 x 6 x 2  ≈ 144 KB   (6-bit color, single buffer)
```

This is an estimate from the library's design, not a measurement. Wi-Fi plus the
async web server need roughly 80 to 120 KB of internal RAM. Out of 512 KB total:

| Option | DMA RAM | Verdict |
| --- | --- | --- |
| 8-bit color, double buffer | ~384 KB | Will not fit with Wi-Fi |
| 8-bit color, single buffer | ~192 KB | Fits; small tearing on fast video |
| 6-bit color, double buffer | ~288 KB | Possible but tight. Try in the spike |
| 6-bit color, single buffer | ~144 KB | Safest; 6-bit is hard to tell from 8-bit on LEDs at gym brightness |

**Plan:** compose every frame in a 256x96 RGB888 canvas in PSRAM (72 KB, PSRAM has 16 MB
to spare), then copy it into the DMA buffer in one pass. Single buffering then only tears
during that short copy, not while drawing. **The default is 6-bit color.** Raise it only if
the spike shows both the RAM and the refresh rate to spare.

Refresh: a wider chain means fewer full refreshes per second, and each extra bit of color
depth halves it again. Raise the pixel clock to the highest value the panels tolerate without
ghosting, and record the refresh figure the library reports at 6-bit with the full 12-panel
chain (Phase 0). **Tell the gym:** a 768 px chain leaves a low refresh rate, so phones filming
the wall (members filming lifts in front of it) will likely show rolling bands. That is the
camera's shutter catching the refresh, not a fault, and the wall looks steady to the eye.

---

## 3a. Branding: The Strength Tank, San Diego

- Background color: teal `rgb(0, 150, 150)`. LED panels shift colors (gamma and
  green-heavy mid-tones), so tune the exact value on the real wall from Settings.
- Logo source: `strength-tank-logo-source.png` (black on transparent, 1400x1400).
  `make_logo_slides.py` renders it at 256x96 into `slides/`, as a PNG, a single-frame GIF
  (the format the board plays), and an LED-dot preview.
- Recommended logo slide: **`logo-wordmark-white`**. White is lit and stands out on teal. In
  the black variants, black means LEDs switched off, so the logo appears as dark holes in a teal glow.
- At 96 px tall, the round logo's small text ("THE", "SAN DIEGO · CA") is about 3 px high
  and can't be read. The wordmark layout repeats the name in large DIN Condensed type next
  to the logo, so the name stays readable.
- Whiteboard: modeled on the chalkboard (black field, blue column lines, yellow headers,
  Instagram glyph and "THE STRENGTH TANK" in the title), with brand teal for the outer
  frame and header rules. See section 4b.

---

## 4. The display: what it shows

The playlist rotates through media, the PR whiteboard, New PRs, announcements and the two
Instagram screens (section 8).

### 4a. Image and video slides

Media is shown full-bleed at 256x96 (an 8:3 aspect ratio). The browser crops and converts
on upload (section 6), so the board only plays files that are already the right size.

### 4b. PR whiteboard: the chalkboard, redrawn for the wall (9 athletes per page)

Pixel-exact renders using the board's own font (Adafruit GFX 5x7, the HUB75 library
default) and the 31 athletes readable in the chalkboard photo:
`whiteboard/page-1.png` … `page-4.png` (native 256x96), `*-led-preview.png`, and
`whiteboard/contact-sheet.png`. Rebuild with `render_whiteboard.py`.

```
x:  0    3                                     115   143   171   199   227   255
y 0 [IG] THE STRENGTH TANK                                               1/4   ← title, 11 px
 11 ═══════════════════════════ teal ═══════════════════════════════════════
 12 ║ Name                                     │  S  │ CJ  │ Ttl │ BS  │ FS  ║ ← yellow
 20 ═══════════════════════════ teal ═══════════════════════════════════════
 21 ║ rytownusa                                │  90 │ 112 │ 200 │ 172 │ 137 ║ ← 8 px rows
 29 ║ barbie_lifter                            │  91 │ 106 │ 193 │ 145 │ 120 ║ ← navy band
    ║ …                                        │     │     │     │136.5│     ║
 85 ║ cynthialbrown_                           │  75 │  92 │ 166 │ 127 │ 109 ║
 95 ═══════════════════════════ teal ═══════════════════════════════════════
```

How it fits in 256x96:

- Name column 114 px = **19 characters**. The longest handle on the chalkboard,
  `the_peoples_master`, is 18 and fits. Longer names get a `~` at the end.
- Number columns are 27 px. In numbers, the decimal point is a single pixel with a 2 px
  advance, so `250.5` is 25 px wide and leaves 1 px clear of each column line.
- 8 px rows with no horizontal lines (a 1 px line per row would cost a whole row per page).
  Every other row has a dim navy band instead.
- Lowercase descenders (g, j, p, y) use the 8th pixel row of each line, so they're kept.

Data rules taken from the chalkboard:

- **Ttl is its own PR, entered by hand, not S + CJ.** In 23 of 31 rows the chalk total is
  lower than snatch plus clean and jerk (for example lawrence_revsci 97 + 126 = 223, Ttl 221),
  because a total has to come from one session. The editor offers "= S + CJ" as a shortcut,
  and warns if a total is higher than S + CJ, since that can't happen.
- kg only, 0.5 kg resolution. Shown without ".0" (`136.5`, `120`).
- Any lift can be blank (alli_garver, justin_ghost have no squats).
- The name is free text: an Instagram handle without the `@` (as on the chalkboard), or a
  plain name such as "Brad B.".
- Annotations like `>152` or "115 clean / 120 jerk" (ishlie) go in a notes field, shown only
  in the web console, never on the wall.
- A PR set in the last 7 days can show in green. This needs the editor to record a date,
  which the chalkboard never had, so existing entries start with no date. The wall stamps
  the date, and the value the PR replaced (`pr_prev`), when a lift goes up (`API.md`
  section 6).
- Order is entry order, as on the chalkboard. Sorting by total or by name is a setting.
- Page time: 31 athletes at 9 per page is 4 pages. At 8 s per page, a full pass takes 32 s.

### 4c. PR whiteboard: large layout (optional, for the back of the room)

8x16 font, about 32 characters across, 5 athletes per page, fewer columns per page:

```
┌────────────────────────────────┐
│ the_peoples_master             │
│   S 153   CJ 175   Ttl 322     │
│ chrisajuan                     │
│   S 112   CJ 140   Ttl 252     │
│ ...                            │
└────────────────────────────────┘
then a second pass of pages showing BS / FS
```

### 4d. New PRs (built)

One athlete per screen, for everyone with a PR inside the highlight window (Settings,
"Show new PRs in green", 7 days by default). `web/prslides.js` is the spec and
`docs/new-prs-mockup.png` shows three screens.

```
y 0 ███ NEW PR  Brad B. ██████████████████████████████████  ← brown banner, handle at 2x
 20 ═══════════════════════════ gold ═══════════════════
 28                    Snatch                              ← one lift: name at 2x,
 48             95.5 kg  +2.5                              ← weight at 3x, increase in green
 80 +2 more                                                ← several lifts: up to 3 rows at 2x
 89 ■ ■ ■                                         today    ← one dot per athlete, and when
```

- A rotation slide of type `new_prs` expands to one screen per athlete, newest PR first,
  at most 20 a pass. Its screen time is per athlete. It is skipped when nobody qualifies,
  when the window is 0 days, and while the wall's clock isn't set.
- Who qualifies is the whiteboard's green rule: a visible athlete, a lift with a value, and
  a PR date from today back to `days - 1` days ago, in the wall's time zone.
- Each screen shows the athlete's recent lifts newest first: one lift large, or up to three
  rows with the weights in one column and "+N more" below. The increase over `pr_prev`
  shows in green when the wall recorded one; a lift that was blank before, or was stamped
  before `pr_prev` existed, shows the weight alone.
- A handle that doesn't fit at double size (more than 17 characters) drops to normal size;
  past 34 characters it is cut with `~`.
- The list is worked out again when athletes or settings change and when the wall's date
  changes, so a screen that has run out of window drops off at midnight. A screen on the
  wall stays put if the athlete moves in the list.
- From the Athletes tab, "See it on the preview" after a new PR opens that athlete's New PRs
  screen, or the whiteboard page with them when the New PRs slide is off.

### 4e. Announcements (built)

One coach's message per screen: white text on the brand teal, `rgb(0, 150, 150)`, the
logo slide's background. `web/announceslides.js` is the spec.

```
y 0 ████████████████████████ teal ████████████████████████
  5     Welcome to          ← centred, the biggest size that fits inside 4 px:
 32   The Strength          ← 3x (13 characters a line, 3 lines),
 59       Tank              ← 2x (20 a line, 5 lines), or 1x (41 a line, 9 lines)
 88           ■ ■ ■         ← one dot per announcement showing, the current one white
```

- A rotation slide of type `announcements` expands to one screen per announcement showing
  today, in list order, at most 20 a pass. Its screen time is per announcement. It is
  skipped when none is showing.
- Showing today: switched on and, if it has dates, today (the wall's date) from the start
  date to the end date, both included. While the wall's clock isn't set, only undated ones
  show.
- Text wraps at spaces and at each new line (up to 6). Size 3x or 2x is used only if no
  word has to be split; at 1x a word longer than a line (41 characters) is split, and text
  that needs more than nine lines is cut with `~`. The editor warns about that before
  saving, and about characters the 5x7 font doesn't have (each shows as `?`).
- With one announcement there are no dots, and the text is centred on the whole screen;
  with several, it is centred above the dots.
- A teal screen lights every LED, so it draws more than most screens: `web/power.js`
  estimates 14.1 A on the busier supply at 100% brightness (full white is 29.4 A), under
  the default 15.3 A budget. The power limiter still dims it if the supplies are set
  smaller. To be measured on the wall.
- Like New PRs, the list is worked out again when the announcements change and when the
  wall's date changes, so one whose end date was yesterday drops off at midnight.

---

## 5. Web console (built)

`web/` is the console, plain HTML and ES modules with no build step, so the board serves the
files as they are. `mock_server.py` serves it locally with the same API the firmware will
have: `python3 -I mock_server.py`, then open http://localhost:8080. Screenshots of every tab,
desktop and phone width, are in `tests/e2e/screenshots/`.

| Tab | What it does |
| --- | --- |
| Wall | LED-dot preview of the rotation with the real screen times, a strip with one thumbnail per screen (click one, or use ← →, to hold it), a panel-edge overlay, and controls for the real wall: whether the real wall is lit, in words ("On until 9:00 pm", "Off until Tue 5:00 am (schedule)"), with "Turn on now" and "Turn off now" (the preview keeps playing and says when the real wall is off). Status shows what the wall is showing now and any damaged config file; a collapsed "Diagnostics" section has firmware and console versions, last restart reason, lowest free memory, refresh rate, power limiter, clock and recent errors |
| Athletes | Edit names and lifts (kg, 0.5 kg steps, blanks allowed, Ttl typed in with a Σ shortcut and a warning above S + CJ), order, hide, notes, CSV import (merge by name or replace) and export. Below it, every whiteboard page exactly as the wall will draw it. The wall stamps PR dates on save; if someone else saved first, the edits stay on screen with a "Load the latest" button |
| Media | Upload an image, GIF or video. The browser crops it to 256x96 (fill or fit, zoom, pan), trims video, converts to GIF and uploads it with its peak current (`peak_a`). Library with storage meter (against the 90% usable) and a note on files the wall will dim |
| Notices | Announcements: one card each with the LED preview exactly as the wall draws it, the text with a character count, on/off, optional start and end dates, move up or down and delete. Warns about characters the wall shows as "?" and offers to replace curly quotes, dashes and "..." with plain ones. Saves like Athletes (PIN, "someone else saved first" with "Load the latest") |
| Rotation | Order, enable and time every screen, 5 to 999 seconds each (per page for the whiteboard, per story for Instagram, per athlete for New PRs, per announcement for Announcements), with the total time for one pass. The New PRs and Announcements rows say how many screens they would show today, or why they are skipped |
| Settings | Title, brightness with the power estimate, power supplies (1 or 2, amps each, budget %), athlete order, PR highlight days, time zone, the display schedule (on/off, one or two time ranges per day, copy Monday to weekdays or every day, mistakes shown under the day), Instagram connection (refresh and "Find it" run on the wall in the background; the page polls), change admin PIN |

First run: while the wall has no admin PIN, the console shows only a "Set an admin PIN" step
(6 to 12 digits, typed twice). Every write needs the PIN; after 5 wrong PINs the wall locks
that phone out for 30 s and longer after that, and the console shows the wait.

Not built yet from the original plan: firmware upload from the console (the route exists,
`API.md` section 15), backup and restore. Wi-Fi and the first admin PIN are set through the board's setup access
point (section 7).

### How lifters are fitted onto pages

`fitWhiteboard()` in `web/layout.js`:

- Fewest pages, at most 9 athletes per page, and balanced: 31 athletes become 8, 8, 8, 7.
- Rows stretch to fill short pages (8 to 10 px, with a thin rule between rows when there's room).
- Columns are measured once over all athletes so they don't jump between pages. The name
  column takes what the longest name needs; the rest widens the number columns. Names that
  can't fit are cut with `~` and listed in the console.

## 6. Media pipeline (built)

The browser does all decoding (`web/media.js`): JPG, PNG, WebP, animated GIF (decoded with
`omggif`, because the browser's own frame decoder needs https, which the board doesn't have),
MP4, MOV and WebM, whatever the browser can play. Clips are capped at 60 s and 900 frames;
a converted file must be under 8 MB. Each frame is quantised to 256 colours and written with
`gifenc`. The board only ever plays 256x96 GIFs (`AnimatedGIF`).

Storage: about 23 MB of flash for files after firmware and OTA partitions, of which 90% is
usable (LittleFS slows down and wears unevenly when nearly full; uploads past that get 507).
A 256x96 video GIF is about 2 to 20 KB per frame depending on motion (the 3 s, 45-frame test
clip was 191 KB). Each upload carries `peak_a`, the brightest frame's current on the busier
supply at 100%, so the wall can dim the whole slide once (section 2, Power).

## 7. Firmware architecture

```
┌──────────────────── Core 0 ────────────────────┐   ┌──────────── Core 1 ────────────┐
│ Wi-Fi STA (AP fallback for first-time setup)   │   │ Render task (fixed fps)        │
│ mDNS  gymboard.local                           │   │  ├─ Rotation scheduler         │
│ Async web server: static UI + REST             │   │  ├─ Slide renderers:           │
│ Upload handler → LittleFS /media               │   │  │   GIF / Whiteboard /        │
│ Config store (JSON on LittleFS)                │──►│  │   New PRs / Announcements / │
│                                                │   │  │   IG story, posts           │
│ Instagram worker (HTTPS, JPEG → tiles)         │   │  ├─ Compose into PSRAM canvas  │
│ NTP sync → RTC                                 │   │  ├─ Power limiter (per supply) │
│ OTA update with rollback                       │   │  └─ Copy to HUB75 DMA buffer   │
└────────────────────────────────────────────────┘   └────────────────────────────────┘
   config changes reach the renderer through a queue, so it never reads a half-written file
```

The display schedule is worked out on core 0, which owns the time: netcore's pure
`evaluateSchedule()` (host-tested across daylight-saving changes) runs every 15 s and after
every schedule save or wake/sleep, and tells the display half through
`display_set_power()` in `wall_shared.h`. Off, the render task blanks the panels and idles;
`render::PowerGate` keeps the screen's timers so the rotation resumes where it was. A clock
that isn't valid keeps the display on.

Stack: PlatformIO, Arduino-ESP32 core 3.x, `ESP32-HUB75-MatrixPanel-DMA`,
`ESPAsyncWebServer` + `AsyncTCP` (maintained fork), `ArduinoJson` 7, `AnimatedGIF`,
`JPEGDEC` (Instagram images), `WiFiClientSecure` + `HTTPClient`.

Flash partitions (32 MB): two 4 MB app slots (for safe OTA rollback), NVS, and about 23 MB
LittleFS, placed above the first 16 MB of the octal flash (checked in Phase 0). The web
console is embedded in the app image, gzipped, so the UI and the API always ship together and
an OTA update replaces both at once.

Wi-Fi and setup access point:

- The board keeps retrying the saved network indefinitely. A dropped network never opens the
  access point.
- The access point `GymBoard-Setup` opens only on first boot (no Wi-Fi saved) or when BOOT is
  pressed within 1.5 s of the wall lighting up and held for 5 s. (Holding BOOT through
  power-on puts the ESP32 into download mode instead, so the gesture starts after boot.) It
  uses WPA2 with a password shown on the wall, together with the setup URL.
- The first admin PIN is set there: while no PIN is set, the board accepts a new PIN only from
  a client on the access point.
- Pressing BOOT within 1.5 s of the wall lighting up and holding it 10 s clears the PIN (and opens the access point), for when the
  PIN is lost.

The renderers in `web/layout.js`, `web/igslides.js`, `web/prslides.js`,
`web/announceslides.js` and `web/power.js` are the spec: the firmware ports them line for line, and a parity test compares the C++
output with the JS output pixel for pixel.

New PRs and announcements depend on the date as well as the content. The render task keeps
the lists it built with the date it used (`wall_local_date()`, "" while the clock isn't
valid, which gives no New PRs and only undated announcements), checks the date once a
second, and treats a change like the same content arriving again: the current screen
finishes unless it has gone.

### Data files (the mock server writes the same files)

The exact schemas, defaults and start-up recovery are in `API.md` section 13. In short:

| File | Contents |
| --- | --- |
| `athletes.json` | `{"rev", "athletes": [{"id", "name", "snatch", "clean_jerk", "total", "back_squat", "front_squat", "notes", "visible", "pr_dates": {lift: "YYYY-MM-DD"}, "pr_prev": {lift: kg or null}}]}`, weights in kg or null. `pr_dates` and `pr_prev` (the value each PR replaced) are stamped by the wall |
| `playlist.json` | `{"rev", "slides": [{"id", "type", "duration_s", "enabled", "file"?}]}`, type is `whiteboard`, `media`, `instagram_story`, `instagram_posts`, `new_prs` or `announcements` |
| `announcements.json` | `{"rev", "announcements": [{"id", "text", "enabled", "start_date", "end_date"}]}`, dates `YYYY-MM-DD` or null |
| `settings.json` | title, brightness (5 to 100), sort (`board`, `total`, `name`), pr_highlight_days (0 to 60), timezone, `pin_salt`, `pin_hash`, `pin_iter` (PBKDF2-HMAC-SHA256, 16-byte salt, 10000 iterations) |
| `instagram.json` | enabled, account_id, access token (never returned by the API), refresh_minutes, last_fetch, last_error, stories and posts with their tile files (`"<gen>/story-0.bmp"`) |
| `media_index.json` | per GIF: frame count (informational), `peak_a` and upload date |
| `media/<name>.gif` | The GIFs. Uploads land as `.part` and are renamed; leftover `.part` files are deleted at start |
| `ig/<gen>/` | One generation of Instagram tiles. A refresh builds a new generation and switches to it only when every image downloaded and decoded |

Config files are written to `.tmp` and renamed. A file that won't parse is renamed to
`<name>.json.bad`, replaced by defaults, and reported in `/api/status` as `config_errors`; a
corrupt `media_index.json` is rebuilt from the files in `media/`.

### REST API

`API.md` is the contract, exact to the status code; the firmware implements it and must pass
`tests/test_mock_server.py`. In short: reads are open. Every write needs the admin PIN in
`X-Admin-Pin`; until a PIN is set every write gets 403 `pin_not_set` except setting the first
PIN. 5 wrong PINs lock that client out for 30 s, doubling to 15 min; 20 wrong PINs in 10 min
from anyone lock all writes for 15 min (429 `locked` with `Retry-After`). No CORS headers are
ever sent. Errors are `{"error": "message for the coach", "code": "machine_code"}`.

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/status` | Paused or playing, display on or off and why (`schedule_state`, `next_change`), now playing, Wi-Fi, uptime, storage, clock and local date, diagnostics (reset reason, lowest free heap, refresh rate, limiter, versions, config errors, last 20 errors) |
| GET / PUT | `/api/athletes` | Read or replace the whole list with its `rev` (409 `stale` on an old rev). The wall stamps PR dates in its time zone |
| GET / PUT | `/api/playlist` | Read or replace the rotation with its `rev` (duration_s whole seconds 5 to 999; media must exist) |
| GET / PUT | `/api/announcements` | Read or replace the announcements with their `rev` (409 `stale` on an old rev) |
| GET / PUT | `/api/settings` | Settings including time zone; `new_pin` sets the PIN (6 to 12 digits); PIN never returned |
| GET / POST | `/api/media` | List with storage (`total_bytes`, `usable_bytes`), or upload a GIF (`?name=x.gif&frames=n&peak_a=amps`; 256x96, under 8 MB, within the usable 90%) |
| DELETE | `/api/media/{name}` | Delete (409 while the rotation uses it) |
| GET / PUT | `/api/instagram` | Instagram settings, current stories and posts, and the background job state (`busy`, `accounts`) |
| POST | `/api/instagram/refresh` | Start a fetch in the background (202; 409 `busy` while a job runs) |
| POST | `/api/instagram/accounts` | Start looking up the accounts the token can see, for "Find it" (202) |
| POST | `/api/control` | `{"action": "next" \| "prev" \| "pause" \| "resume" \| "wake" \| "sleep"}`; wake and sleep turn the display on or off until the schedule next changes |
| GET | `/media/{name}`, `/ig/{gen}/{tile}.bmp` | The GIFs, and the Instagram tiles as BMP for the console preview |

## 8. Instagram screens

Two screen types show @thestrengthtank's current story and latest two posts. The board
fetches them itself every 15 minutes (5 to 1440, set in the console), crops each image to a
tile (stories 54x96, posts 78x78) and draws them with the wall's font
(`web/igslides.js`). Stories older than 24 hours disappear from Instagram's API, so when
there's no current story that screen is skipped.

Fetching runs on a worker task, never in a web request: "Refresh now" and "Find it" start a
job (202) and the console polls `GET /api/instagram` about every 1.5 s, giving up after 90 s.
One job runs at a time. Each refresh downloads into a new tile generation (`ig/<gen>/`) and
switches `instagram.json` to it only after every image downloaded and decoded; any failure
keeps the tiles the wall already had and records `last_error`. A half-finished fetch can
never leave the wall with missing or mixed tiles.

What Meta's API allows (checked against Meta's IG User Stories reference, 2026-11-06):

- Stories are only readable through the Instagram API with **Facebook Login**
  (graph.facebook.com), with a token holding `instagram_basic` and `pages_read_engagement`
  (plus `ads_management` or `ads_read` if the Page role comes through Business Manager). The
  newer "Instagram Login" API is not documented for stories. Posts work with the same token.
- Reels and videos: the wall shows the cover image (`thumbnail_url`) with a ▶ badge. The board
  can't decode video, and Meta omits `media_url` for video with licensed audio anyway.
- Live stories and reshared stories aren't returned.

Setup (Meta changes these screens often; check against their current docs):

1. In Instagram, make @thestrengthtank a Professional account (Business or Creator) and
   link it to the gym's Facebook Page.
2. At developers.facebook.com, create an app (Business type) and add Instagram with the
   Facebook Login setup.
3. Make a token that doesn't expire: in Meta Business Settings, create a system user, give
   it the Page and the app, and generate a token with `instagram_basic` and
   `pages_read_engagement`. (Alternative: a Page token derived from a long-lived user token,
   which also doesn't expire.)
4. In the console, Settings → Instagram: paste the token and Save, press **Find it**, pick
   @thestrengthtank, Save, then **Refresh now**.

Unknowns for the firmware (to settle in the spike):

- `JPEGDEC` only decodes baseline JPEG. If Instagram's CDN serves progressive JPEGs, the board
  can't decode them and needs another decoder or a small resizing relay.
- HTTPS (TLS) needs about 40 to 50 KB of RAM per connection, competing with the display's DMA
  buffers. It may need mbedTLS to allocate from PSRAM.

## 9. Tests

| Command | Covers |
| --- | --- |
| `node --test tests/` | Page fitting, column widths, decimals, PR colours, sorting, weight and time validation, Instagram slide layout, power model and slide brightness |
| `python3 -I -m unittest discover -s tests` | API contract: limits, mandatory PIN and its storage, lockout timing (injected clock), revisions, PR dates in a time zone, background Instagram jobs, atomic tile swap on failing downloads (faked network), storage reserve, leftover and corrupt file recovery, status fields, no CORS. The firmware must pass the same cases |
| `python3 tests/run_e2e.py` | Real Chrome: converts an MP4, WebM, animated GIF and PNG and uploads them with `peak_a`; drives the console on a phone (PIN prompt, PR message, stale save, Instagram polling, lockout); screenshots every tab at desktop and phone width, plus the first-run PIN step, diagnostics, a stale save and a lockout |

Each test suite was checked by breaking the code on purpose (unbalanced pages, a 1000 s
limit, a server accepting 4 s; for this revision: no lockout, no doubling, PIN checked while
locked, no global lock, no rev check, old tiles deleted before the download, switching before
the download, only HTTP errors caught) and confirming it failed.

## 10. Build plan

**Phase 0: hardware spike (do this first, with 1 panel, then all 12)**

1. Run vendor example `07_Pixel_Mapping_Test` on 1 panel with the `SHIFTREG` driver
   (ICN2037BP), then on the 12-panel serpentine chain with `VirtualMatrixPanel`. Confirm
   every panel's orientation.
2. With the display running, read a file from the TF card. This settles SD vs. flash.
3. Record the refresh rate and free internal RAM at 6-bit and 8-bit color, with Wi-Fi up
   and with an HTTPS request in flight. Write down the refresh figure the library reports at
   **6-bit with the full 12-panel chain**: that is the number the gym will live with.
4. Film the wall with a phone at a fast shutter speed. Check for banding.
5. Measure each supply's current on a full-white frame at 25% brightness and compare with
   `web/power.js`. Adjust `PANEL_FULL_WHITE_A` if they differ. Measure **one panel at full
   white** too: above about 2.5 A, switch to LRS-200-5 or LRS-350-5 supplies (section 2). And
   measure a **mid-grey** frame next to full white, to see how far current is from linear in
   pixel value (the power model assumes linear).
6. Download one of @thestrengthtank's post images on the board and decode it with `JPEGDEC`.
7. **Level check:** measure the logic level at the first panel's 74HC245 input. With the
   245 at 5 V VCC, its VIH is 3.5 V, above the ESP32's 3.3 V output. If the first panel
   misreads, add a level shifter (for example a 74HCT245) between the board and panel 1.
8. **LittleFS above 16 MB:** format and use a LittleFS partition placed above the first
   16 MB of the octal flash, and confirm reads and writes there while the display runs.
9. **TLS under load:** complete an HTTPS handshake to graph.facebook.com while the display
   DMA and the web server are both running, and note the free internal RAM during it.
10. **Panels during reset:** note what the panels show while the board resets or is being
    flashed (blank, frozen, or full-bright noise). Full-bright noise at power-up would need
    the output-enable line held off until the display starts.

**Phase 1: display core.** Render task, PSRAM canvas, GIF slides, whiteboard and power limiter.

**Phase 2: Wi-Fi, API and updates.** Setup AP (rules in section 7), mDNS, the REST API in
`API.md`, the console embedded gzipped in the app image, and **OTA with rollback** (a new
image that doesn't come up healthy reverts to the previous slot). OTA comes this early
because every fix after the wall is mounted arrives that way.

**Phase 3: Instagram.** Worker task, tile generations, the two screen types.

**Phase 4: hardening.** Backup and restore. (The display schedule and the power-supply
setting are built: `API.md` sections 7.4 and 7.5.)

Still open:

1. Check `athletes-from-chalkboard.csv` against the board. It was transcribed from the
   photo: the third column of names was cut off, `chrisaydin_`'s numbers were too faint to
   read, and `meng.wooo` / `justin_ghost` were already cut off in chalk.
2. Set up the Meta app and token (section 8).
