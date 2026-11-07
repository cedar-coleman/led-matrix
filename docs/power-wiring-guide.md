# Power wiring and test guide

How to wire the twelve P5 panels with the two Mean Well LRS-100-5 supplies you have now,
how to measure what the wall really draws, and how to wire a single Mean Well UHP-500-5 if
you move to one.

> **Mains safety.** The supplies have exposed 120 V AC terminals. Do all wiring with the
> cord unplugged, cover the AC terminals before plugging in, and connect every earth (FG / ⏚)
> terminal. If you're not comfortable with mains wiring, have an electrician do the AC side;
> the 5 V side is low voltage but carries enough current to melt undersized wire.

## 1. Which panel is which

The data chain snakes across the wall. Row 2 is mounted upside down and runs right to left.
Numbers are chain positions, as seen from the front:

```
          col 1     col 2     col 3     col 4
row 1   [  1  ] → [  2  ] → [  3  ] → [  4  ]
row 2   [  8  ] ← [  7  ] ← [  6  ] ← [  5  ]      (mounted upside down)
row 3   [  9  ] → [ 10  ] → [ 11  ] → [ 12  ]
```

With two supplies, **supply A powers panels 1 to 6 and supply B powers panels 7 to 12**:

```
row 1   [ A ] [ A ] [ A ] [ A ]
row 2   [ B ] [ B ] [ A ] [ A ]
row 3   [ B ] [ B ] [ B ] [ B ]
```

Keep this split. With "Power supplies" set to 2 (Settings tab, the default), the power
limiter assumes exactly these panels on each supply (`web/power.js`). If you wire it
differently, the limiter protects the wrong supply.

## 2. Current per panel (what to plan for)

| Figure | Per panel | 6 panels (one LRS-100-5) | 12 panels |
| --- | --- | --- | --- |
| Full white, 100% (seller spec) | 4.9 A | 29.5 A | 59 A |
| Full white, 100% (Waveshare, PMD Way) | 4.0 A | 24 A | 48 A |
| LRS-100-5 rating | | **18 A** (firmware budget 15.3 A) | |

With the LRS-100-5s, all-white can't reach 100%. The firmware dims any frame that would pass
15.3 A on either supply. Section 5 measures where the real limit is.

## 3. Wiring with the two LRS-100-5 supplies

### Parts

| Part | Quantity | Notes |
| --- | --- | --- |
| Mean Well LRS-100-5 | 2 | You have these |
| 3-prong power cord (18 AWG or heavier) | 2 | Or one cord into a fused IEC inlet per supply |
| Switched power strip with breaker | 1 | Both supplies plug in here: one switch for the wall |
| 14 AWG stranded wire, red and black | about 3 m each | Supply to each branch |
| Inline blade fuse holder, 14 AWG, with 15 A fuse | 4 | One per branch of 3 panels |
| Lever connectors rated 20 A (e.g. WAGO 221 series), 3-port and 5-port | about 8 | To split a branch to 3 panel pigtails |
| Ring or fork terminals for the supply screws, plus ferrules | 1 pack | No bare stranded wire under a screw |
| 14 AWG jumper wire | 20 cm | Ties the two supplies' −V together |
| Terminal cover or small enclosure | 2 | Covers the AC terminals |
| Multimeter | 1 | DC volts |
| **DC-capable** clamp meter | 1 | Many clamp meters measure AC only. Check it says "DC A" |

The panels' own power pigtails (red and black, usually one cable powers two panels) connect
the last stretch to each panel. Use one pigtail lead per panel; don't daisy-chain more than
the cable was made for.

### LRS-100-5 terminals

Left to right on the terminal block (confirm against the label on your units):

```
 [ L ] [ N ] [ ⏚ ] [ -V ] [ -V ] [ +V ] [ +V ]   (V.ADJ knob next to the terminals)
   AC in       earth     5 V output, two pairs
```

**Before the first power-up, check the input voltage switch on the side of each supply is set
to 115 V.** At 230 V it won't run properly on a US outlet.

### Diagram

```
                       120 V outlet
                            │
                  switched power strip (breaker)
                    │                    │
            cord A (L N ⏚)        cord B (L N ⏚)
                    │                    │
         ┌──────────┴─────────┐ ┌────────┴───────────┐
         │   LRS-100-5  "A"   │ │   LRS-100-5  "B"   │
         │ -V -V        +V +V │ │ -V -V        +V +V │
         └──┬──┬─────────┬──┬─┘ └─┬──┬─────────┬──┬──┘
            │  │         │  │     │  │         │  │
            │  │  ┌──────┘  │     │  │  ┌──────┘  │
            │  │  │      ┌──┘     │  │  │      ┌──┘
            │  │ [15A]  [15A]     │  │ [15A]  [15A]          fuses on +V
            │  │  │      │        │  │  │      │
            │  │  A1     A2       │  │  B1     B2            14 AWG to each branch
            │  │  │      │        │  │  │      │
            │  │ panels  panels   │  │ panels  panels
            │  │ 1,2,3   4,5,6    │  │ 7,8,9   10,11,12
            │  │  │      │        │  │  │      │
            └──┴──┴──────┴── GND ─┴──┴──┴──────┴──           −V returns
                     │
            14 AWG jumper: −V of A to −V of B (never join +V to +V)
                     │
              controller GND
```

Each branch: the fused +V and its −V return go to a pair of lever connectors near the three
panels, and each panel's pigtail goes into those connectors.

| Branch | Supply terminals | Panels | Position on the wall |
| --- | --- | --- | --- |
| A1 | A, first +V / −V pair | 1, 2, 3 | top row, left three |
| A2 | A, second +V / −V pair | 4, 5, 6 | top row right, middle row right two |
| B1 | B, first +V / −V pair | 7, 8, 9 | middle row left two, bottom row left |
| B2 | B, second +V / −V pair | 10, 11, 12 | bottom row, right three |

### Steps

1. Mount both supplies on a non-flammable surface with space around them for air. Don't
   put them in a sealed box or squeezed behind the panels.
2. Set each supply's input switch to **115 V**.
3. AC side, cord unplugged: brown/black (hot) to L, white/blue (neutral) to N, green to ⏚.
   Fit the terminal cover.
4. Run the four branches as in the table: 14 AWG from each +V terminal through a 15 A fuse,
   and from the matching −V terminal, to the lever connectors near those panels. Plug in the
   panel pigtails.
5. Join the two supplies' −V terminals with the 14 AWG jumper. The data ribbon between
   panels 6 and 7 crosses from one supply to the other, so the grounds must be common.
   **Never connect +V of A to +V of B.**
6. Controller power: during testing, power the ESP32 board from its USB-C port (from your
   computer, which you need for the serial monitor anyway). For the permanent install, use a
   5 V USB charger on the same power strip, or feed the board's 5 V input from supply A
   through a 2 A fuse. Check Waveshare's interface diagram for which terminal is the board's
   own input. Never run panel current through the board.
7. Connect the board's ground to the −V bus as well as through the ribbon, with a short wire.
8. HUB75 data: board to panel 1, then panel to panel following the chain. Keep the
   board-to-panel-1 ribbon short (the first panel's input chip wants a stronger signal
   than the 3.3 V board gives; short cable helps).

## 4. Wiring with one Mean Well UHP-500-5

### Mounting (required, not optional)

Mean Well's datasheet rates the UHP-500 for its full current only when it's **bolted to an
aluminum plate 450 × 450 × 3 mm** (or a metal cabinet of that size): centred, flat, with
thermal grease, using the four M3 holes in the bottom. Without the plate, at 115 V it's good
for only about 60% (about 48 A). The aluminum backing or frame the panels hang on can be the
plate if it's that size and the supply has open air around it.

### Terminals (from the datasheet)

```
 AC input TB1:   1 = L    2 = N    3 = ⏚ earth             (torque 5 kgf·cm)
 DC output TB2/TB3:  1, 2 = −V     3, 4 = +V                (torque 8 kgf·cm)
```

Input is 90 to 264 V AC, full output from 110 V up, so no voltage switch to set.

### Parts (in addition to the cord, strip, meters and connectors above)

| Part | Quantity | Notes |
| --- | --- | --- |
| Aluminum plate 450 × 450 × 3 mm, thermal grease, 4 M3 screws | 1 | Or the wall's frame, if it's that size |
| Bus bar pair (+ and −), rated 100 A or more | 1 | Or two high-current distribution blocks |
| 8 AWG wire, red and black, about 0.5 m each | 2 of each | Supply to bus bars, one per terminal pair |
| 12 AWG wire, red and black | about 3 m each | Bus bars to branches |
| Inline fuse holder, 12 AWG, with 20 A fuse | 4 | One per branch |

### Diagram

```
   120 V outlet ── switched strip ── cord (L N ⏚)
                                         │
              ┌──────────────────────────┴───────────────────────────┐
              │        Mean Well UHP-500-5  (on 450×450×3 mm plate)   │
              │  TB1: 1 L  2 N  3 ⏚        TB2/TB3: 1 −V 2 −V 3 +V 4 +V│
              └──────────────────────────────────┬────┬────┬────┬────┘
                                                  │    │    │    │
                                 8 AWG, short     │    │    │    │
                     ┌────────────────────────────┘    │    │    │
                     │    ┌────────────────────────────┘    │    │
                     │    │         ┌───────────────────────┘    │
                     │    │         │    ┌───────────────────────┘
                 ┌───┴────┴───┐ ┌───┴────┴───┐
                 │  − bus bar │ │  + bus bar │
                 └─┬──┬──┬──┬─┘ └─┬──┬──┬──┬─┘
                   │  │  │  │     │  │  │  │
                   │  │  │  │   [20A][20A][20A][20A]     12 AWG branches, fuse on +V
                   │  │  │  │     │  │  │  │
                  panels 1-3   panels 4-6   panels 7-9   panels 10-12
                  (each branch: +V and −V to lever connectors, then panel pigtails)

   controller GND ── − bus bar
```

At full white each branch carries about 12 to 15 A, comfortably inside 12 AWG and a 20 A fuse.
Use both terminal pairs on the supply (one 8 AWG lead per terminal), not just one pair.

Firmware: after switching, open the console's Settings tab and, under "Power supplies", set
**Supplies: 1, Amps each: 80, Budget %: 85**, then Save. The wall then counts all twelve
panels against one 68 A budget (80 A x 85%), so full white at 100% (about 59 A) is no longer
dimmed. The same over the API:
`PUT /api/settings {"power": {"supplies": 1, "rated_a": 80, "budget_pct": 85}}`.

Only use 80 A with the supply bolted to its aluminium plate. Without the plate it is good
for about 48 A at 115 V: set Amps each to 48 until the plate is fitted. Going back to the two
LRS-100-5s, set Supplies: 2, Amps each: 18, Budget %: 85.

## 5. Tests

Do these in order. Record the numbers in the table at the end and send them over; they set
the firmware's power model to your real panels.

### Firmware builds you'll use

From the `firmware/` folder, with the board on USB-C:

```
# Panel order and orientation
PLATFORMIO_BUILD_FLAGS="-DWALL_TEST_PATTERN" ../.tools/pio-venv/bin/pio run -e display_dev -t upload

# Current measurements, 18 A supplies (brightness capped at 50% so a stray key can't trip them)
PLATFORMIO_BUILD_FLAGS="-DWALL_CALIBRATE -DWALL_CALIBRATE_MAX_PCT=50" ../.tools/pio-venv/bin/pio run -e display_dev -t upload

# Current measurements, UHP-500-5 (no cap)
PLATFORMIO_BUILD_FLAGS="-DWALL_CALIBRATE" ../.tools/pio-venv/bin/pio run -e display_dev -t upload

# Serial monitor (keys are sent from here)
../.tools/pio-venv/bin/pio device monitor
```

In calibration mode the power limiter is off. Keys: `w` white, `h` mid grey, `r` `g` `b`
single colours, `k` off; `1` to `9` set 10 to 90% brightness, `0` sets 100%. It starts black
at 10%.

### Test 1: supplies alone (no panels connected)

1. Plug in, switch on. The supply's LED lights.
2. Measure +V to −V at the supply terminals: about 5.0 V.
3. Turn V.ADJ to **5.10 V**. (You'll fine-tune under load in test 5.)
4. Switch off and wait for the LED to go out before connecting anything.

### Test 2: one panel

Connect only panel 1 (power from branch A1, data from the board). Flash the test pattern
build. Panel 1 shows "1", a border and an up arrow. If it's blank or noisy, rebuild with
`-DWALL_DRIVER_FM6126A` added. If the board reboots in a loop, see `README.md` (flash mode).

### Test 3: the whole chain

Connect all 12 panels. Each panel shows its chain number, in the layout of section 1, with
every arrow pointing up. A panel with the wrong number is in the wrong place in the chain;
an arrow pointing down means that panel is mounted the wrong way round. Note the refresh
rate printed in the serial log at boot.

### Test 4: current measurements

Flash the calibration build (capped at 50% on the LRS-100-5s). Clamp the DC clamp meter
around **both +V wires from one supply together** (both inside the jaw). Use the +V side:
the −V wires are joined between the supplies, so return current can split between them and
the reading would be off. With the UHP-500-5, clamp both 8 AWG +V leads together. Zero the
clamp meter (DC zero button) before each reading.

For each line in the table, set the pattern and brightness, wait 10 seconds, and read:

- the current for each supply
- the voltage at the **farthest panel** on each branch (meter probes on that panel's power
  connector, + and −)

Watch for flicker or the wall blinking off and on: that's the supply's overload protection
("hiccup"). If it happens, press `k` at once and note which line it was.

### Test 5: heat soak (30 minutes)

- LRS-100-5s: white at 50%. UHP-500-5: white at 100%.
- After 30 minutes, check: no flicker; farthest-panel voltage still 4.8 V or more (if not,
  turn V.ADJ up a little, no higher than 5.3 V at the supply); the supply case is hot but
  you can keep a hand on it for a few seconds. With the UHP-500-5, the case must stay
  under 80 °C (an IR thermometer helps).

### Test 6: real content

Flash the normal firmware (`pio run -e wall -t upload`), set brightness to what you plan to
use, and measure each supply while the PR whiteboard, the logo slide and your brightest
photo or video are on screen.

### Results table

| Line | Pattern | Brightness | Supply A (A) | Supply B (A) | Farthest panel volts | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | off (`k`) | 10% | | | | baseline: board and idle panels |
| 2 | white | 20% | | | | |
| 3 | white | 30% | | | | |
| 4 | white | 50% | | | | |
| 5 | mid grey | 50% | | | | |
| 6 | red | 50% | | | | |
| 7 | white | 100% | | | | **UHP-500-5 only** |
| 8 | PR whiteboard | planned | | | | test 6 |
| 9 | brightest photo | planned | | | | test 6 |
| 10 | after 30 min soak | | | | | test 5 |

### Reading the results

- **Per-panel full-white current** ≈ line 4 (white at 50%) × 2 ÷ 6 for each LRS-100-5, or line
  7 ÷ 12 for the UHP-500-5. Lines 2 to 4 show whether current rises in a straight line with
  brightness. If line 4 isn't about 2.5 × line 2, the doubling is an estimate, so say so.
- **Keep the LRS-100-5s** if your real content (lines 8 and 9) at your planned brightness
  stays under about 15 A per supply. The limiter takes care of rare bright frames.
- **Upgrade** if lines 8 or 9 come close to 15 A, or if you want all-white at full brightness:
  white at 100% for 6 panels is roughly 2 × line 4.
- **Voltage** at the farthest panel under 4.8 V means a wire run is too thin or too long, or
  V.ADJ needs a small increase.

Send me the filled-in table. I'll set the per-panel figure (`PANEL_FULL_WHITE_A`) in the
firmware. The supply rating and budget are yours to set in Settings > Power supplies (one
or two supplies, amps each, budget %), so the limiter dims only when it has to.
