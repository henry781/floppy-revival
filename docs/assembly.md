# Assembly guide: Pico 2 to YD-380B interface board (rev C.1)

Companion to [hardware.md](hardware.md) (BOM, wiring rules, smoke test)
and [pinmap.md](pinmap.md) (pin assignment). PCB: rev C.1, 145 × 82 mm
(the width of the drive), KiCad project
`hardware/kicad/floppy-revival.kicad_pro`. The data side is on the left
(Pico, resistor networks, 7407s, 34-pin ribbon header at the bottom left,
aligned with the drive's connector). The drive supply is on the right
(19.5 V to 12 V / 5 V through two discrete switching regulators, switched
by GP18).

This guide assumes you can solder through-hole parts and use a
multimeter. It covers what is specific to this board: checks, order,
orientation traps and the power-up sequence.

## Should the Pico be programmed before soldering?

No, but test it first (section 4.2). Once soldered it stays accessible
(USB on the rear edge, BOOTSEL on top, RUN on TP9) and can be reflashed at
any time, whereas a castellated module soldered flat is very hard to
remove. Flashing the project firmware beforehand is safe (on a bare Pico,
GP18 and the other pins drive nothing) but is only needed from section 7
(see [building.md](building.md)). **Never have a USB cable plugged into
the Pico while soldering.**

## 1. Protecting the drive

The drive is the only part that cannot be replaced. It does not touch the
board until section 11, after everything has been measured without it.

1. **No soldering, rework or probing with the drive connected** (ribbon or
   Molex), nor with the adapter or USB plugged in.
2. The regulators are checked three times: markings before soldering
   (section 4.3: LM2678T-**12** in U3, LM2676T-**5.0** in U4), voltages on
   J5 with no drive (section 7), and the cable wire by wire at the drive
   end (yellow = 12 V). **12 V on the 5 V line kills the drive electronics
   instantly.**
3. **SN7407N only** (non-inverting). **A 7406 / 74LS06 asserts every line
   at boot, write gate included, and erases the inserted disk.**
4. The drive-less smoke test (section 9) is mandatory: /WGATE (pin 24)
   must stay high under all circumstances.
5. Ribbon: red stripe = pin 1 at both ends, checked with the meter
   (section 10.1). Crimped the wrong way round, it sends the signals to
   the ground pins.
6. **No hot plugging**: ribbon and Molex are connected and disconnected
   with both the adapter and the USB unplugged.
7. Touch an earthed metal object before handling the Pico, the 7407s and
   the ribbon.
8. Write tests on sacrificial disks only. A precious disk gets its notch
   covered and the J3 jumper fitted.

## 2. Tools

- Temperature-controlled iron (≥ 50 W), 2–2.4 mm chisel tip, plus a 3 mm
  tip for the large thermal masses: J1 ground row, J4, J5, U3/U4, D6/D7,
  L1/L3 and C12/C15/C16. These need the larger tip or 20–30 °C more.
- Multimeter with continuity beeper.
- Oscilloscope, useful for the power checks (inrush, ripple).
- USB-serial adapter, **3.3 V**, for the UART on J2.

## 3. Checks on the bare PCB (before any soldering)

Multimeter in continuity mode.

| # | Check | Expected |
|---|-------|----------|
| 1 | GND ↔ 3V3 (TP8 ↔ TP5), GND ↔ VBUS (TP8 ↔ TP6), 3V3 ↔ VBUS | **no beep** |
| 2 | J4 "+" ↔ GND, J5.1 (12V) ↔ GND, J5.4 (5V) ↔ GND, J5.1 ↔ J5.4, J4 "+" ↔ J5.1 | **no beep** |
| 3 | J1 pin 24 (/WGATE) → pin 6 of the U1 socket footprint | beep |
| 4 | J1 pin 24 → any Pico castellation **directly** | **no beep** (the 7407 must be in the path) |
| 5 | All **odd** pins of J1 → TP7 (GND) | beep on every one |
| 6 | **Even** pins of J1 → TP7 (GND) | **no beep** |
| 7 | A1 castellation pads: clean, tinned, no solder mask overlap | visual |
| 8 | M3 holes 3.2 mm, non-plated; TP holes 1.0 mm; L1/L3 holes 1.7 mm | visual / calipers |

A failure on checks 1–6 means: fit nothing, investigate first.

## 4. Preparation

### 4.1 Parts

The parts list with values and part numbers is the
[bill of materials in hardware.md](hardware.md#bill-of-materials). Sort
the parts by reference before starting, and measure every resistor before
fitting it. Parts that are easy to mix up:

- R14 and R29 (1 kΩ) sit among 330 Ω parts; a wrong value does not show
  afterwards.
- R31 and R32 (330 kΩ and 39 kΩ, 1 %) have a 7.62 mm pitch, not 10.16 mm.
- D3, D4 and D5 look the same: P6KE27A, P6KE15A and P6KE6.8A. Read the
  marking.
- U3 is the LM2678T-**12**, U4 the LM2676T-**5.0**. Swapped, the drive's
  logic gets 12 V.
- C15 (470 µF **35 V**) and C19 (470 µF **16 V**) have the same value.
- C11 (10 µF, "106") and C22 (1 µF, "105") are both 5 mm ceramics.
- F1 is the MF-R300 (30 V), not the 16 V MF-RG300.

Silkscreen conventions: the square pad is pin 1 of every part. On
electrolytics it is "+", and the **white half** of the silkscreen circle
is the "−" side, like the stripe on the capacitor. On diodes it is the
cathode ("K"). On the LEDs (lying flat) it is the cathode (short lead);
the small tab drawn at the edge of the body marks the rim on the
**anode** side, so the LED's flat ends up opposite the tab.

### 4.2 Test the Pico 2

Hold BOOTSEL, plug the USB in, release. An RP2350 drive must appear, and
`INFO_UF2.TXT` must mention RP2350 (otherwise it is a Pico 1, RP2040; do
not solder it). Optionally drag a Pico 2 "blink" UF2 or
`floppy_revival_vdisk.uf2` onto it and check the LED. Unplug.

### 4.3 Regulators, toroids and jack

1. **U3 and U4 look alike.** Read the marking: **LM2678T-12** = U3 (12 V),
   **LM2676T-5.0** = U4 (5 V). Keep them in labelled trays and write
   "12" / "5" on the back of the tab. **An LM2678T-12 in place of U4 puts
   12 V on the drive's logic.**
2. Offer L1/L3 up to the bare board. The two leads leave the bottom of the
   ring one behind the other (9 mm apart). The toroid stands upright **with
   the ring along the length of the silkscreen rectangle** (left to right),
   leads in the two middle holes, without forcing or twisting the winding.
   If it only fits across the rectangle, or the leads do not drop in by
   themselves, do not solder it; check the part.
3. Prepare the panel-mount DC jack (used in section 7): two 20–22 AWG
   wires of about 15 cm, heat-shrunk at the tags, red on the **centre** tag
   (+), black on the other. Check with the adapter: red = +19.5 V relative
   to black. Unplug.

## 5. Soldering order

Lowest parts first, tallest last, with one exception: the Pico goes on
early, because the C3/C4/C5/TP column just to its right would get in the
way of the iron later. The toroids come last. Axial resistors and diodes
lie flat at 10.16 mm pitch unless stated otherwise.

**Step 1: R22–R26, R29–R32 (supply control).** Five around Q2 (between
the LEDs and L1), R29 to the left of C15, R30 between U3 and U4, R31 and
R32 below J3. R22, R23, R26, R29 are oriented top to bottom; R24, R25,
R30, R31, R32 left to right. **R31 and R32 have a 7.62 mm pitch**: bend
the leads right at the body, they will not fit otherwise.

**Step 2: Pico 2 (A1), soldered flat.** Nothing under the module: its
back test pads must rest on the solder mask, with no solder crumbs. **USB
towards the rear edge** (top of the board, J2 side). Tack two diagonally
opposite castellations (for example pins 1 and 21), check that every
half-hole is centred, then solder the rest alternating sides. The 8 GND
pins (3, 8, 13, 18, 23, 28, 33, 38) are on the ground plane and need
3–4 s or 20 °C more. Check: pin 38 → TP8, pin 36 → TP5, pin 40 → TP6
beep; no beep between neighbouring castellations, especially 36–40 (3V3,
3V3_EN, GND, VSYS, VBUS). A very short beep between a supply and ground
(39 ↔ 38, 3V3 ↔ GND, later VBUS ↔ GND with C5) is the meter charging the
Pico's capacitors: the ohmmeter reading rises to OL and starts again if
you swap the probes. A real bridge reads a steady few Ω.

**Step 3: resistors and SIP networks.**

- R10–R13 (330 Ω) are the four staggered ones between RN3 and RN1. R9
  (330 Ω) and R14 (**1 kΩ**) sit side by side to the right of U2. R19 and
  R20 (330 Ω, LEDs) are to the right of the Pico.
- **RN1, RN2, RN3**: pin 1 (common, dot or line on the body) in the
  square pad, at the top for RN1 and RN3, **at the bottom for RN2**. RN3
  is the 1 kΩ ("102") on the left, RN1/RN2 the 10 kΩ ("103") along the
  Pico.
- R27, R28 (10 Ω), below D6 and D7. With C20/C21 they snub the switching
  node and are required: simulation gives up to 40 V on the diode without
  them (see Power in [hardware.md](hardware.md)).

**Step 4: TVS diodes D3, D4, D5 and R33.** Stripe on the square "K" pad.
**Read the marking before each one:**

- D3 = P6KE**27**A, top right near F1. Its K-side lead passes 3 mm from
  the live tab of Q1: keep it short and straight, it must never touch
  the tab.
- D4 = P6KE**15**A, bottom, left of J5 (12 V rail).
- D5 = P6KE**6.8**A, top to bottom, right of J5 (5 V rail). A D5 in place
  of D4 would conduct permanently on the 12 V rail.
- R33 (470 Ω), top to bottom, between J5 and D5. Without it the unloaded
  5 V rises towards 6.1 V (leakage through U4, measured). On rev C boards
  without the R33 footprint, solder R33 across D5's pads.

**Step 5: ceramics and DIP-14 sockets.**

- C1, C2, C3, C9, C10 (100 nF), C13, C17 (1 µF), C11 (10 µF, below Q1),
  C22 (1 µF, below J4), C14, C18, C23 (10 nF), C24 (100 nF, below J3 next
  to R32), C20, C21 (470 pF, below R27/R28). No polarity.
- **Do not swap C11 ("106") and C22 ("105").** A swap removes the
  soft-start ramp (see the troubleshooting table).
- C13/C17 are just left of each regulator (against pin 2), C14/C18 just
  below: do not swap 1 µF ("105") and 10 nF ("103").
- U1, U2 sockets: both face the **same way**, notch and pin 1 on the
  **left**. U1 is at the top left, U2 just above J1. **Do not insert the
  7407s yet.**

**Step 6: test points, J2, J3.** TP1–TP9 as test pins or small loops of
clipped lead. J2 (UART): "G" is pin 1, at the top. J3 (RO): jumper not
fitted.

**Step 7: Q2, then Q1.** Q2 (BC547): spread footprint (2.54 mm), so bend
the two outer leads outwards, middle lead straight, flat side on the flat
of the silkscreen, 3–5 mm of lead under the body. Q1 (IRF9540N, vertical):
**metal tab towards "TAB" / J4 / the rear edge**; the wrong way round,
gate and source are swapped. **The tab sits at 19.5 V: no heatsink, and
it must not touch any metal.**

**Step 8: U3, U4, D6, D7.**

- **U3 (LM2678T-12, upper cell), U4 (LM2676T-5.0, lower cell)**: read the
  marking again. The staggered leads only go in one way: tab towards the
  rear edge (double line on the silkscreen), leads 1-3-5-7 towards the
  front. The tab is grounded, no heatsink. Pin 4 (ground, centre of the
  rear row) needs more heat.
- D6, D7 (both SB560), **standing**: bend the anode lead (no stripe)
  through 180° against the body, stand the body on the square "K" pad
  (stripe at the bottom), bent lead in the round "A" pad. Keep the body
  vertical and flush: these diodes are in the switching loop, which must
  stay short.

**Step 9: electrolytics, fuse, radial inductors.**

- C4, C5 (47 µF), right of the Pico: C5 has its "+" on the Pico side
  (VBUS), C4 on the TP5/TP8 side (+3.3 V).
- C8 (100 µF 35 V, top right), C12, C16 (470 µF 35 V, right-hand edge,
  next to each regulator), **C15 (470 µF 35 V, left of L1), C19 (470 µF
  16 V, left of L3)**, C6 (220 µF 25 V), C7 (220 µF 16 V, bottom between J1
  and J5). Read the voltage on C15 and C19 before soldering, and check
  afterwards that each "−" stripe is on the white half.
- F1, L2, L4, L5: no polarity. L5 is the input filter (top right), L2 and
  L4 the 12 V and 5 V output filters (bottom, between J1 and J5).

**Step 10: LEDs D1 (ACT, green), D2 (DIRTY, red).** Lying flat, dome
towards the rear edge, cathode in the square pad (see the tab convention
in section 4.1). Bend both at the same distance from the dome so they
line up.

**Step 11: J1.**

- **Pin 1 (triangle) at the right-hand end**, like pins 1/2 of the
  drive's connector, which J1 lines up with when the board sits behind the
  drive. Keying slot towards the inside of the board (U1/U2 side), over the
  opening drawn on the silkscreen. Do not "correct" it. The ribbon leaves
  upwards and folds back towards the drive without a twist.
- The 17 odd pins (inner row) are on the ground plane: 3 mm tip or
  +20 °C, 3–4 s. **Go easy on the solder**: traces pass 0.27 mm from these
  pads on the component side, under the shroud, where a bridge would be
  invisible and impossible to repair.
- Then repeat section 3, check 6: no even pin of J1 beeps to TP7.

**Step 12: J4, J5.** J4 (2-way): wire openings towards the **rear edge**.
J5 (4-way): openings towards the **front edge**. Thick pins on wide
traces: 3 mm tip or +20–30 °C, 4–5 s. A cold joint here heats up at 2 A.

**Step 13: toroids L1 (12 V, upper cell) and L3 (5 V, lower cell),
last.** Identical parts, no polarity. Standing as checked in section 4.3,
resting on the board with 2 mm gap at most. Flux, 3 mm tip or +20–30 °C,
4–5 s; the solder must fill the whole hole. Do not pull on the enamelled
winding: it is held only by its two leads.

## 6. Cleaning and inspection

Clean the soldered side and around the Pico with IPA and let it dry.
Inspect every joint with a loupe for bridges, dull joints and untrimmed
leads (the Pico has 40). Repeat checks 1, 2 and 4 from section 3: still
no beep.

## 7. Power block check (J5 empty, no drive)

This needs a Pico flashed with **`floppy_revival.uf2`**. The `vdisk`
firmware never drives GP18, so with it J5 stays at 0 V. If needed, do
section 8 first (adapter unplugged), flash `floppy_revival.uf2` and come
back.

1. Jack wires into **J4: red (+) on the "+" terminal**, black on the
   other. Do not tin wires going into a screw terminal; twist them or use
   a ferrule.
2. Adapter plugged in, **USB unplugged**: J5.1 and J5.4 = **0 V** (Q1 and
   the regulators are off as long as GP18 floats).
3. Oscilloscope on J4 "+". Plug in the USB: 3.5 s after the UART banner
   the firmware prints `drive power on (GP18, adapter 19xxx mV)` (18.5–
   20.5 V expected; `adapter not detected` points to R31/R32/C24 or the
   jack) and starts the rails. The adapter voltage may dip briefly at
   inrush (see Power in [hardware.md](hardware.md)) but must not collapse
   and restart in a loop. Without an oscilloscope, check that the adapter's
   LED stays steadily lit.
4. Rails up: J5.1 = 11.5–12.5 V, J5.4 = 4.75–5.25 V, J5.2 / J5.3 = 0 V
   relative to GND (TP7).
   - **12 V on J5.4: U3 and U4 are swapped. Unplug immediately.**
   - 5 V on J5.1: the same, the other way round.
   - 0 V on both: F1, Q1, L5, or the regulator joints.
   - One output at 0 V: its inductor (L1/L3), diode (D6/D7) or regulator.
   - Unstable, or close to 19 V: faulty regulator.

   In all these cases, unplug and fix the fault before going further.
5. After one minute, touch U3, U4, D6, D7 briefly: barely warm with no
   load. Burning hot means a short or a reversed part.
6. With an oscilloscope, measure the ripple under load (power resistors on
   J5) as in step 0 of the smoke test in [hardware.md](hardware.md).
7. Pull the adapter with the USB still in: the UART prints `drive power
   OFF` within 50 ms and J5 falls; plug it back: `drive power on` 3.5 s
   later. Unplug the USB: J5 drops to 0 V within a few milliseconds.
   Unplug the adapter.
8. Power cable (section 10.2): wires in J5, drive **not connected**,
   adapter unplugged: continuity from J5.1 to the **yellow** wire's
   contact in the plug, and from J5.4 to the **red** one. Then, with
   adapter and USB plugged in, measure **inside the female plug**: yellow
   = 11.5–12.5 V, red = 4.75–5.25 V. Unplug everything.

## 8. First USB power-up (no 7407s, no drive, adapter unplugged)

This needs a built firmware (see [building.md](building.md)).

1. USB into a port directly on the computer (no unpowered hub).
2. Relative to TP8: VBUS = 4.75–5.25 V (TP6), 3V3 = 3.25–3.40 V (TP5).
3. Flash `floppy_revival_vdisk.uf2` (BOOTSEL + USB, drag the file). The
   Pico's LED blinks fast, then slowly (1 s) once the computer has
   recognised the device; a virtual disk appears. ACT/DIRTY off.
4. UART: a **3.3 V** USB-serial adapter on J2 (G → GND, board TX → adapter
   RX, board RX → adapter TX), 115200 baud (on macOS for example
   `screen /dev/tty.usbserial-XXXX 115200`): banner
   `FloppyRevival starting`.

## 9. Inserting the 7407s and the smoke test

Power off. Insert U1 and U2 **with the IC notch over the socket notch**
(both on the left), checking that no pin folds under the package.

Flash `floppy_revival.uf2` and run the **complete** drive-less smoke test
in [hardware.md](hardware.md) (steps 0–7), adapter included. **J1 pin 24
(/WGATE, TP1) must stay high through USB plug-in, boot, reset
(TP9 → GND), BOOTSEL entry, reflash and 10 minutes of idle.** Sequencing:
12 V only appears on J5.1 after the UART line `drive power on (GP18)`;
RUN → GND cuts it; BOOTSEL cuts it.

## 10. Wiring

### 10.1 Ribbon

Square cut, female **2.54 mm** IDC connector crimped in a vice (pliers
crimp it crooked), red stripe on the pin 1 side at both ends. Meter check:
stripe → J1 pin 1 → drive connector pin 1; pins 1, 8, 16, 24 end to end;
no continuity between adjacent pins.

### 10.2 Drive power

A factory-crimped Molex extension (ATX peripheral), cut, female plug at
the drive end, bare wires into J5: **yellow (+12 V) on 1, blacks on 2 and
3, red (+5 V) on 4**, the same order as the Molex. Strip 6 mm, twist, no
tinning. Check it pin by pin and measure inside the plug as in section 7,
point 8, before it goes anywhere near the drive.

### 10.3 Drive edge fingers

Clean with 99 % IPA (a soft white vinyl eraser if oxidised, never an
abrasive pen). Push the ribbon on once only, straight.

## 11. First contact with the drive

Everything unplugged. Connect the ribbon, then the Molex, to the drive.
Then the adapter and USB, in either order: the firmware switches the drive
on (line `drive power on (GP18)`). Follow the bring-up checklist in
[hardware.md](hardware.md), steps 4–8 (probe with no disk, then a known
disk, then density validation BY WRITING on sacrificial disks, then first
writes). J3 jumper fitted and notch covered for any precious disk.
Day-to-day use is in [user-guide.md](user-guide.md).

To disconnect: eject on the computer, wait for DIRTY to go out, pull the
USB (the drive switches off), then the adapter.

## 12. Troubleshooting

| Symptom | Where to look |
|---------|---------------|
| No RP2350 drive in BOOTSEL | charge-only USB cable? try another cable |
| No UART banner | TX/RX crossed? adapter set to 3.3 V? 115200? |
| `floppy_init` ≠ 0 | pin collision; check the castellation joints |
| VBUS < 4.75 V | unpowered hub, cable too long; bridge or short to GND |
| J5 at 0 V although the UART says `drive power on` (if the UART never says it: `vdisk` firmware loaded instead of `floppy_revival.uf2`; if it says `adapter not detected`: R31/R32/C24, jack, F1) | jack wired wrongly, F1 tripped (unplug for 1 min), J4 joint, Q1 reversed, R29/R30 (ON/OFF), L5 or the U3/U4 joints |
| The adapter cuts out and restarts in a loop when the rails start | no soft-start ramp: is C22 (1 µF) soldered? C11 and C22 swapped? (Power and Inrush in [hardware.md](hardware.md)) |
| `probe ...: 9 (MEDIA_CHANGED)` repeating, no disk | **normal** (DSKCHG latch) |
| `probe ...: 4 (TIMEOUT)` with no disk | DSKCHG line (J1.34/GP5), or drive not powered |
| `TRK00 fault latched` | pin 26/GP2 wiring; unplug and replug the USB after fixing it |
| Motor never spins | continuity GP11 → U2.3, U2.4 → J1.16; 12 V present on J5.1? |
| 12 V dips or oscillates when the motor starts | U3, L1, C15 joints; measure with the oscilloscope on J5 (Power in [hardware.md](hardware.md)) |
| Whistling, or SW node ringing on the oscilloscope | snubbers R27/C20 (12 V), R28/C21 (5 V): soldered? joints? |
| Noisy reads / CRC errors | 1 k pull-ups soldered? clean edge fingers? J1 odd-pin joints? |
| `recovered by watchdog` at boot | investigate before any write |
