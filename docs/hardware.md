# Hardware

The interface board sits between a Raspberry Pi Pico 2 and a 5.25" drive.
It is a 145 x 82 mm two-layer PCB, through-hole only, the width of the
drive, meant to be mounted behind it. It carries:

- the Pico 2, soldered flat by its castellated edge;
- two SN7407 open-collector buffers for the signals going to the drive,
  and pull-ups and series resistors for the signals coming back;
- the 34-pin header, placed so that the ribbon runs straight to the
  drive's card edge;
- a drive power supply: 19.5 V from a laptop adapter, switched by the
  firmware, converted to 12 V (LM2678, 5 A) and 5 V (LM2676, 3 A), each
  followed by a second LC filter stage.

The KiCad project is in `hardware/kicad/`, a PDF of the schematic in
`hardware/floppy-revival-schematic.pdf`. The GPIO-to-drive mapping and
the drive's jumpers are in [pinmap.md](pinmap.md); the build procedure is
in [assembly.md](assembly.md).

## Design choices

- **A supply on the board, not a separate PSU.** A 5.25" drive needs 12 V
  and 5 V with about 1 A of spindle start current on 12 V. Using an old
  ATX supply or a bench supply works, but it brings a power-sequencing
  rule the user has to remember: the Pico's inputs are only 5 V tolerant
  while the Pico is powered. With the supply on the board and controlled
  by the firmware, the drive can only be powered while the firmware runs.
- **Fixed-output regulators.** No feedback divider to get wrong, a
  guaranteed output tolerance, and a control loop compensated by TI
  (simulated phase margin 65 degrees). The adjustable LM2678-ADJ with an
  external soft start was evaluated: better start-up, but 33 degrees of
  phase margin without extra compensation, and an open feedback resistor
  would drive the rail toward 19.5 V. Making that safe needed about 25
  more parts. Not worth it for a drive that draws 1 A.
- **Two filter stages and snubbers.** The drive's specified ripple limits
  are loose (200 mV on 12 V, 100 mV on 5 V); the targets here were 50 mV
  and 20 mV, met with a 4.7 uH / 220 uF second stage on each rail.
- **Open-collector buffers, non-inverting.** The drive interface is
  open-collector, active low, terminated in the drive. The SN7407 sinks
  40 mA, enough for any terminator pack, and being non-inverting, a
  floating Pico pin (reset, boot) leaves the line released. An inverting
  7406 would assert every line, the write gate included, at power-on.
- **Through-hole parts at 2.54 mm pitch or wider,** resistors lying flat,
  so the board can be built by someone with ordinary soldering skills.

## Layout

Seen from above with J1 at the bottom (the edge facing the drive):
**data on the left** (Pico with USB on the rear edge, SIP networks along
its left pin row, inputs fanned down the left edge, the two 7407s above
J1, J1 bottom-left with pin 1 at its right end like the drive's edge
connector) and **power on the right** (19.5V terminal J4 at the rear-right
corner, two stacked regulator cells on the right edge, toroids in the
middle, output filters and J5 bottom-right). A row of ground vias at
x = 120.5 mm marks the boundary; no data track enters the power area. The
one power net on the data side is the 19.5V feed of the adapter-sense
divider (R31/R32/C24 below J3, 0.4 mA DC), which arrives along the bottom
edge.

## Bill of materials

References are those of the schematic. Part numbers are the ones used for
the built board (they are also in each symbol's description, and in
`hardware/fabrication/bom.csv`).

One row per identical part, grouped by type; quantities are totals.

### Module, ICs, transistors, diodes

| Qty | Refs | Part | Purpose / notes |
|-----|------|------|-----------------|
| 1 | A1 | Raspberry Pi **Pico 2** (RP2350), without headers | controller: not the pin-compatible RP2040 Pico. Soldered flat by its castellations |
| 2 | U1, U2 | TI **SN7407N**, DIP-14 (hex open-collector non-inverting buffer, 40mA IOL) | Pico → drive outputs, 8 gates used. Non-inverting ONLY: never 7406/74LS06 (inverting = every released line asserted at boot, write gate included). Mounted in sockets (see Connectors) |
| 1 | U3 | TI **LM2678T-12/NOPB**, TO-220-7 (12V 5A fixed, 260 kHz) | 12V regulator |
| 1 | U4 | TI **LM2676T-5.0/NOPB**, TO-220-7 (5V 3A fixed, 260 kHz) | 5V regulator: **do not swap with U3**: an LM2678T-12 in U4 puts 12V on the drive logic |
| 1 | Q1 | Infineon **IRF9540NPBF**, P-MOSFET TO-220 (G-D-S) | high-side drive power switch, tab = drain (live), no heatsink |
| 1 | Q2 | **BC547B**, TO-92 (C-B-E) | level shifter GP18 → Q1 gate; wide-spaced footprint: bend the outer legs out |
| 2 | D6, D7 | Vishay **SB560-E3/54**, 60V 5A Schottky, DO-201AD | catch diodes, mounted standing (anode up); simulated reverse peak ≤ 35V with the snubbers. A long hard short on J5 (regulator current limits 8.75A / 5.4A) is outside their rating: accepted, the drive draws ~1A |
| 1 | D3 | Littelfuse **P6KE27A**, DO-15 | input TVS (standoff 23.1V: 18–21V adapters only). Its cathode lead passes 3 mm from Q1's live tab: short and straight |
| 1 | D4 | Littelfuse **P6KE15A**, DO-15 | 12V rail TVS at J5: a shorted regulator switch would put 19.5V on the drive; it clamps, then fails short and lets the adapter's limit / F1 act |
| 1 | D5 | Littelfuse **P6KE6.8A**, DO-15 | 5V rail TVS at J5, same role |
| 1 | D1 | LED **3mm green** | ACT (GP15), horizontal mount, dome flush with the rear edge |
| 1 | D2 | LED **3mm red** | DIRTY (GP16), same mount |

### Inductors

| Qty | Refs | Part | Purpose / notes |
|-----|------|------|-----------------|
| 2 | L1, L3 | Feryster **DTMSS-16/0.033/6.0-V** (33µH, 6A, 24.5mΩ, sendust toroid Ø20 × 10 mm, on edge) | buck inductors. Stock of this part is thin at distributors: order it first. Footprint per the Feryster "V" drawing: both leads leave the bottom of the ring, 9 mm apart across the 10 mm thickness |
| 3 | L2, L4, L5 | Ferrocore **COIL0608-0.0047** (4.7µH, 5A, 23mΩ, Ø7.5 × 9.5 mm radial, 3 mm pitch) | L5: input π filter (keeps the 260 kHz input current off the adapter cable); L2/L4: second-stage output filters. Holes 1.1 mm |

### Capacitors

| Qty | Refs | Part | Purpose / notes |
|-----|------|------|-----------------|
| 3 | C1, C2, C3 | Murata **RCER71H104K0DBH03A** 100nF 50V X7R, 2.5 mm (3.6 × 2.5 mm body) | C1/C2 at the 7407 VCC pins, C3 at the Pico 3V3(OUT) |
| 3 | C9, C10, C24 | Murata **RDER71H104K0K1H03B** 100nF 50V X7R, 5 mm | C9/C10: rail HF decoupling at J5; C24: adapter-sense filter at GP26 |
| 3 | C14, C18, C23 | Murata **RDER71H103K0K1H03B** 10nF 50V X7R, 5 mm | C14/C18: bootstrap capacitors (CB–SW); C23: `DRV_EN` filter at the ON/OFF pins |
| 3 | C13, C17, C22 | Murata **RDER71H105K2K1H03B** 1µF 50V X7R, 5 mm | C13/C17: HF input bypass closest to each VIN (pin 2); C22: Q1 gate–drain (Miller) capacitor, sets the switch-on ramp |
| 1 | C11 | TDK **FG20X7R1H106KRT06** 10µF 50V X7R, 5 mm (5.5 × 7 × 4 mm) | Q1 gate–source: 10 × C22, so an adapter hot-plug cannot couple Q1 on (simulated: Vgs −2.7V, rails stay at 0). Do not swap with C22 (code 106 vs 105) |
| 2 | C20, C21 | Murata **RDE5C2A471J0M1H03A** 470pF 100V C0G, 5 mm | SW-node snubber capacitors, with R27/R28: **required** |
| 2 | C4, C5 | Panasonic FR **EEUFR1E470** 47µF 25V, Ø5 × 11, 2.0 mm | C5 on VBUS (total VBUS capacitance kept ≤ ~100µF for USB inrush), C4 on 3.3V |
| 1 | C8 | Panasonic FC **EEUFC1V101** 100µF 35V, Ø8 × 11.5, 3.5 mm | switched-node bulk, first cap of the input π filter |
| 3 | C12, C15, C16 | Panasonic FR **EEUFR1V471L** 470µF 35V, Ø8 × 20, 3.5 mm | C12/C16: regulator input bulk (1.56A ripple rating); C15: 12V output cap (its ESR sets the loop stability, datasheet 7.1.3) |
| 1 | C19 | Panasonic FR **EEUFR1C471** 470µF 16V, Ø8 × 11.5, 3.5 mm | 5V output cap: **do not swap with C15** (read the voltage) |
| 1 | C6 | Panasonic FR **EEUFR1E221** 220µF 25V, Ø8 × 11.5, 3.5 mm | 12V second-stage filter cap at J5 |
| 1 | C7 | Panasonic FR **EEUFR1C221** 220µF 16V, Ø6.3 × 11.2, 2.5 mm | 5V second-stage filter cap at J5 |

### Resistors

All metal film **1%**, 0207 body, any rating from 0.25W up (one 0.4W or
0.6W family for every value is fine: only R26 (173 mW) and R33 (53 mW)
dissipate more than 25 mW), laid flat on 10.16 mm pitch except R31/R32 (7.62 mm, leads bent
closer). 1% matters for R31/R32: with 5% parts the divider drifts ±9% and
an 18V adapter can read below the 16V enable threshold.

| Qty | Refs | Value | Purpose / notes |
|-----|------|-------|-----------------|
| 2 | R27, R28 | 10Ω | SW-node snubber resistors: **required**: simulated with 30–70 nH of hot-loop inductance the catch diode sees 31–40V reverse without them, 20–35V with (15–25 mW each) |
| 7 | R9, R10, R11, R12, R13, R19, R20 | 330Ω | 5× series protection on the drive → Pico inputs, 2× LED current limit |
| 1 | R33 | 470Ω | 5V minimum load at J5 (`5V_DRV` → GND, beside D5), 10.6 mA / 53 mW: **required**: with no load, U4's leakage lifts the unloaded 5V rail to ~6.1V in ~10 s (D5 clamps it); measured 4.91–4.96V with it fitted |
| 2 | R14, R29 | 1kΩ | R14: INDEX pull-up to 3.3V (same role as RN3); R29: GP18 → regulator ON/OFF pins (`DRV_EN`) |
| 1 | R26 | 2.2kΩ, **≥ 0.4W** (e.g. Vishay MRS25 0.6W) | bleed on the switched 19.5V node, 173 mW while on: 2.6 s to the regulators' lockout, see Switch-off |
| 1 | R30 | 4.7kΩ | `DRV_EN` pull-down: keeps the ON/OFF pins < 0.43V against the 2 × 45µA internal pull-ups while GP18 floats |
| 1 | R22 | 10kΩ | Q2 base series resistor |
| 1 | R25 | 22kΩ | Q1 gate divider (Q2 collector → gate) |
| 1 | R32 | 39kΩ (7.62 mm pitch) | adapter-sense divider bottom |
| 1 | R24 | 47kΩ | Q1 gate–source pull-up (switch off by default) |
| 1 | R23 | 100kΩ | Q2 base pull-down: drive power stays off while GP18 floats |
| 1 | R31 | 330kΩ (7.62 mm pitch) | adapter-sense divider top: `19V5_F` ÷ 9.46 → GP26/ADC0 (19.5V = 2.06V, the 30V clamp = 3.17V); the firmware enables the drive only above 16V and drops GP18 below 15V, see Power |
| 1 | RN1 | Bourns **4610X-101-103LF**, SIP10, 9 × 10kΩ bussed | pull-ups to 3.3V on GP2…GP9 (pin 1 = common, pin 6 unused) |
| 1 | RN2 | Bourns **4605X-101-103LF**, SIP5, 4 × 10kΩ bussed | pull-ups to 3.3V on GP10…GP13 |
| 1 | RN3 | Bourns **4605X-101-102LF**, SIP5, 4 × 1kΩ bussed | **mandatory** pull-ups to 3.3V on TRK00/WPT/RDATA/DSKCHG, drive side of the 330Ω: the drive's outputs measured open, see [pinmap.md](pinmap.md#connector-measurements) |

### Connectors, fuse, test points, mechanical

| Qty | Refs | Part | Purpose / notes |
|-----|------|------|-----------------|
| 1 | J1 | 34-pin (2×17) male box header, **2.54mm**, keyed, vertical | GND row (odd pins) toward the board interior, signals on the outer row; pin 1 at the right end, aligned with the drive's edge connector |
| 1 | J2 | 1×3 pin header, 2.54mm | UART0 debug: GND, GP0 (TX), GP1 (RX): **required**, USB stdio is disabled (USB is the MSC device) |
| 1 | J3 | 1×2 pin header, 2.54mm + jumper | GP17 read-only (closed = firmware refuses all writes) |
| 1 | J4 | Phoenix Contact **MKDS 1,5/2-5,08** (1715721), 2-way screw terminal | 19.5V input from the chassis jack, pin 1 = + |
| 1 | J5 | Phoenix Contact **MKDS 1,5/4-5,08** (1715747), 4-way screw terminal | to the drive Molex: 1 = +12V, 2 = GND, 3 = GND, 4 = +5V (Molex order) |
| 2 | — | **DIP-14 sockets**, 7.62 mm (turned-pin "tulip" preferred: firm contact, the chip goes in once; dual-wipe is fine too) | for U1, U2 |
| 1 | F1 | Bourns **MF-R300** PTC (3A hold, 6A trip, **30V**: not the 16V MF-RG300), radial 5.08 mm | input fuse |
| 9 | TP1–TP9 | test points (THT pad Ø2.0, hole 1.0; pin stubs or resistor-lead loops) | WGATE, MOTOR, RDATA, INDEX, 3V3, VBUS, GND ×2, RUN |
| 4 | H1–H4 | M3 standoffs + screws (nylon, or metal with the GND pour 0.25 mm from the holes) | mounting, holes at (44,37) (179,37) (44,109) (179,109) mm |

### Off-board

| Qty | Part | Purpose / notes |
|-----|------|-----------------|
| 1 | 19.5V laptop adapter, ≥45W (Dell/HP: cut the plug, keep +19.5V and GND, insulate the ID wire) | **18–21V only**: D3's standoff is 23.1V |
| 1 | panel-mount DC jack 5.5 × 2.5 mm, ≥3A, plastic body, + matching plug | chassis input, two wires (20–22 AWG) to J4, centre = +. Meter the polarity before screwing: reverse polarity forward-biases D3 through F1 and an adapter's current limit is below F1's trip |
| 1 | ATX peripheral (Molex) extension cable, factory-crimped | cut in half: female end on the drive, bare wires into J5 (yellow = 1, black = 2/3, red = 4); the free plug is metered by wire colour before the drive is ever connected. Fallback: Molex 8981 housing + crimps + mandatory continuity check |
| 1 | straight 34-way ribbon with a card-edge IDC connector, **2.54mm** | drive end; cut short (~5 cm), the spare half is a backup |
| 1 | 34-way IDC female socket, **2.54mm** | crimps on the cut end. NOT 2mm-pitch (needs 1.0mm ribbon) |
| 1 | USB-serial adapter, 3.3V logic | on J2 |
| — | hookup wire, 99% IPA + lint-free swabs | assembly + card-edge cleaning |

## Wiring rules

- **All odd pins (1–33) of the 34-pin connector go to GND**, commoned with
  Pico GND and the adapter's GND (on the board: a 1 mm bottom-layer bus
  along J1's GND row plus both pours). A missing common ground is the
  classic "reads garbage flux" failure.
- **Cutting the ribbon:** cut square with a sharp blade, crimp the IDC
  socket evenly (vice, not pliers). Keep pin 1 (red stripe) on the same
  edge at both connectors, and check with a meter that stripe → edge
  connector pin 1 → box header pin 1 before first power-on: a reversed
  IDC crimp lands signals on the grounded odd pins.
- **Buffer power:** the two SN7407N run from **Pico VBUS (pin 40, USB 5V)**:
  they need 4.75–5.25V, not 3.3V. Plug the Pico into a root USB port or
  a powered hub, never an unpowered hub (USB allows sag to 4.40V there,
  below the 7407's floor); the smoke test measures VBUS at the 7407s. The
  drive's 5V rail stays on the drive side only.
- **Outputs (Pico → drive)**, each through a 7407 gate: REDWC(2, GP12),
  DS1(12, GP13), MOTOR(16, GP11), DIR(18, GP10), STEP(20, GP9),
  WDATA(22, GP8), WGATE(24, GP7), SIDE1(32, GP6). U1 carries SIDE1, WGATE,
  WDATA; U2 carries STEP, MOTOR, DS1, REDWC, DIR (gate map in the
  schematic). **Connector pin 10 stays unconnected** (the YD-380B reads
  Motor Enable only from pin 16). The drive's 1kΩ terminators provide the
  high level; no pull-ups on the drive side of the buffers.
- **7407 inputs:** 10kΩ pull-up to **3.3V** on every used input (RN1 pins
  7–10, RN2). On /WGATE (GP7) this is the safety: a floating Pico pin
  during reset/boot keeps the buffer output released and the write gate
  cannot glitch on. Never pull these nodes to 5V: they are Pico GPIOs.
  The 4 unused 7407 inputs are tied to GND (U1 pins 3, 11, 13; U2 pin 9),
  their outputs left open.
- **Inputs (drive → Pico):** TRK00 (GP2), WPT (GP3), RDATA (GP4), DSKCHG
  (GP5), INDEX (GP14). The drive's outputs are bare open-collector with
  **no drive-side pull-up**. Each line: **1kΩ pull-up to 3.3V** on the
  board (RN3, R14; mandatory: internal pull-ups cannot recharge RDATA
  between MFM pulses), then 330Ω in series to the Pico pin, which also
  carries a 10kΩ pull-up (RN1 pins 2–5; harmless, ~0.1V on the low level).
- **Power sequencing:** RP2350 pads are 5V-tolerant only while the Pico is
  powered. Rule: **the Pico is powered before the drive, and the drive is
  off before the Pico goes away.** The board enforces it in hardware: the
  drive rails only exist while GP18 is driven high by running firmware (see
  Power). Plug the adapter and the USB in any order: the firmware
  switches the drive on only once it measures the adapter (GP26) above
  16V, and switches it off within 50 ms when the adapter goes away. On
  USB unplug GP18 floats and the regulators stop at once through their
  ON/OFF pins; the drive's outputs are open-collector anyway.
- **No hot-plugging:** never mate/unmate the 34-pin ribbon or the Molex
  with the adapter or the USB plugged. Touch the adapter's chassis before
  handling the board or ribbon: the drive's TTL inputs and the header have
  no ESD protection.
- **Card-edge care:** before first mating, inspect the drive's edge fingers
  under light; clean with 99% IPA on a lint-free swab (soft white vinyl
  eraser first if oxidized: never abrasive fiberglass pens). Mate squarely,
  once: do the drive-less smoke test first.
- **Drive jumpers (YD-380B):** keep the factory DS1; straight cable. See
  [pinmap.md](pinmap.md). A **twisted** PC cable (select on pin 14, motor on
  pin 10) needs different wiring AND firmware: not supported as-is.
- **Terminator:** measured present, 1kΩ, on this unit: it stays fitted
  (single drive on the cable).

## PCB layout musts (before sending to fab)

- **Box header footprint:** the library IDC footprint
  (`Connector_IDC:IDC-Header_2x17_P2.54mm_Vertical`): IDC numbering
  alternates odd/even between the two rows to match flat ribbon. A
  hand-numbered sequential-row footprint grounds every signal. J1 sits
  bottom-left with pins 34…1 at x 57.9…98.5 mm, aligned with the drive's
  card edge (15 mm from the drive's rear-right edge, pin 1 on the right
  seen from above); mock up with the real ribbon before ordering: it must
  not fold.
- **Pico module nets:** 3.3V pull-up rail = **3V3(OUT), pin 36** (never
  VSYS/VBUS); 7407 VCC = VBUS, pin 40; **all eight GND castellations
  (pins 3, 8, 13, 18, 23, 28, 33, 38)** stitched to both pours; official
  Pico footprint; no top-layer tracks, vias or pads under the module body
  (rule area `pico_body_nocopper`; the GND pour is allowed there and the
  footprint's own keep-outs clear the module's underside pads). Only +3.3V
  and the GP15 LED line cross under it, on the bottom layer.
- **Data routing (by hand):** every GPIO leaves the Pico's left column on its
  own row through its SIP pin (RN1/RN2 column, one pin per row); the input
  rows cross the staggered 330Ω and RN3 and drop down the left edge into
  J1 from below, under the shroud; the output rows fall as nested L's onto
  the 7407s, whose output pins sit 1.27 mm right of their J1 pin and pass
  straight through the GND-row gap. INDEX runs below the Pico to R9/R14
  and enters J1 from below. No crossing on the top layer; on the data side
  the bottom layer only carries +3.3V, VBUS, GP15, GP26 (down the Pico's
  right side to R31/R32), the TP3 stub and the J1 GND bus.
- **Power layout:** each regulator cell keeps its switching loop tiny:
  1µF ceramic on VIN pin 2, catch diode on SW pin 1 standing next to it,
  explicit 1 mm bottom-layer GND ties pin 4 → ceramic → diode anode, with
  the toroid between the cell and its output cap. The SW copper is one
  short 1.5 mm track; FB senses the first-stage output on the bottom layer
  away from SW; the second-stage LC filters and their TVS sit at J5. 1.5 mm
  tracks on the power nets (`Power` class, 0.3 mm clearance), ground vias
  at every power GND pad and a fence at x = 120.5 mm (only L2's body and
  D4's ground pad sit left of it, both quiet). GP18 and Q2 stay
  outside the power area; the ON/OFF line `DRV_EN` runs under L1's body
  between the two converter cells and reaches the pins from the east, so
  the bottom ground pour stays continuous from the input capacitors to
  both switching loops. The 12V feedback sense runs on the bottom layer at
  y = 60 mm, away from the toroid and from the pulsed input feed. The
  adapter-sense divider R31/R32/C24 sits below J3; its 19.5V feed is a
  0.4 mm track along the east and bottom edges (0.4 mA, DC).
- **Test points:** WGATE, MOTOR, RDATA, INDEX in a row under J1, each in
  front of its pin, with TP7 (GND) at the end; 3V3, VBUS, GND, RUN beside
  the Pico (RUN + BOOTSEL allows reflash without replugging USB). The
  screw terminals J4/J5 are the probe points for the power rails.
- **Mechanical/silkscreen:** 4× M3 NPTH mounting holes at (44,37) (179,37)
  (44,109) (179,109) mm (the GND pour runs close to the holes: plastic or
  GND-tolerant standoffs); LED domes flush with the rear edge; pin-1 marks
  on J1, the DIP sockets and the SIP networks; labels G/TX/RX (UART),
  ACT / DIRTY, RO, 19V5 IN with + / −, TAB (Q1), 12V G G 5V (J5),
  "YD-380B interface • rev C.1", the logo along the left edge, and
  "DRIVE PWR = GP18" on the back. Silk text ≥ 1.0 mm /
  0.15 mm stroke.
- **Fab rules used:** 0.4 mm signal tracks, 0.2 mm clearance, 0.5 mm
  supply tracks, 0.6/0.3 mm vias (0.8/0.4 mm for GND stitching), 0.5 mm
  copper-to-edge; checked against the JLCPCB 2-layer limits.
- ERC, DRC and schematic parity at zero (refill zones from the GUI first:
  the kicad-cli fill reports two 4 µm edge-clearance artifacts at the
  rounded corners), and a Gerber viewer pass before ordering.
- **Fab package:** `hardware/fabrication/gerbers/`: F/B copper,
  F/B mask, F/B silk, Edge.Cuts, job file, Excellon drill (mm, decimal,
  PTH + NPTH merged, 301 holes, 0.3–3.2 mm) and the drill map, exported
  with `kicad-cli pcb export gerbers --subtract-soldermask` and
  `pcb export drill --format excellon --excellon-units mm
  --excellon-zeros-format decimal`. Order as a 2-layer 1 oz, 1.6 mm board;
  nothing needs editing. Zip the folder's contents if the fab wants a zip.

## Power

- 5.25" drives need +5V (logic) and +12V (spindle/stepper). Sizing basis
  (the YD-380 manual has no power spec; TEAC FD-55GFR spec p.105): 12V
  0.22A typical / 0.54A max average, < 1.0A peak for 400 ms at spindle
  start; 5V 0.38A max average, 0.46A peak; ripple < 200 mV p-p on 12V,
  < 100 mV p-p on 5V. Design margin: 12V ≤ 2.9A peak (the LM2678's minimum
  ICL/2, see below) / 1.5A continuous, 5V 2A continuous; ripple targets
  50 mV / 20 mV. The drive rails never come
  from USB; they share only ground with the Pico.
- **Chain.** Chassis jack → J4 (`19V5_IN`) → F1 (MF-R300) → D3 (P6KE27A to
  GND) (`19V5_F`) → Q1 high-side switch → C8 100µF + R26 bleed
  (`19V5_SW`) → L5 4.7µH (input π filter) → `19V5_BUCK`: C12/C16 470µF +
  C13/C17 1µF at each regulator's VIN →
  - 12V: U3 LM2678T-12, D6 SB560, L1 33µH, C15 470µF (`12V_FB`, sensed by
    U3's FB pin) → L2 4.7µH → C6 220µF + C9 100nF + D4 P6KE15A
    (`12V_DRV`) → J5 pin 1;
  - 5V: U4 LM2676T-5.0, D7 SB560, L3 33µH, C19 470µF (`5V_FB`) → L4
    4.7µH → C7 220µF + C10 100nF + D5 P6KE6.8A + R33 470Ω minimum load (`5V_DRV`) → J5 pin 4.

  GP18 drives both Q1 (through Q2) and the regulators' ON/OFF pins
  (`DRV_EN`: R29 1kΩ series, R30 4.7kΩ pull-down, C23 10nF at the pins),
  so the drive is unpowered whenever the Pico is not running firmware,
  and the rails stop within microseconds when GP18 drops. Logic stays on
  VBUS/3V3.
- **Why these parts.** Fixed-output regulators: no feedback divider to get
  wrong, ±3% guaranteed over temperature, and the loop TI compensated
  (simulated phase margin 65°). The adjustable parts with an external
  soft start were evaluated and not used (see Design choices and
  `hardware/simulation/README.md`). The 12V rail uses the 5A LM2678 rather than the LM2676: with
  Vout > 6V the LM2676 datasheet (SNVS031N 7.1.7) warns of current-limit
  hysteresis with large output caps when the load nears ICL/2 (1.8A
  minimum); the LM2678's ICL/2 (2.9A minimum) stays well above the drive's
  1A peak (the same caveat applies to it above 2.9A: run the datasheet's
  short-and-release test on 12V at the final acceptance test). 33µH gives ~0.5A p-p inductor ripple on either rail at 260 kHz;
  the sendust toroids are rated 6A and keep their field inside the core.
  Each output then goes through a 4.7µH / 220µF second stage (corner
  ~4.9 kHz, ~70 dB ideal attenuation at 260 kHz, in practice limited by
  the caps' ESR), which is what brings the ripple under the targets. The
  input π filter keeps the switching current off the adapter cable.
- **Snubbers (R27/C20, R28/C21)** are fitted. Simulated with the
  regulator's real switch edge and 30–70 nH of hot-loop inductance, the
  catch diode sees 31.5–40.4V reverse without them (a 40V part would be at
  its limit), 20–35V with 10Ω + 470pF, which also ends the ringing;
  15–25 mW each. D6/D7 are 60V parts for the same reason.
- **Light load.** With the spindle off the drive draws 30–40 mA on 12V
  (TEAC spec), below TI's 100 mA test condition. Simulated over a settled
  120 ms window: 0.3 mVpp at 20 mA, 0.9 mVpp at 40 mA, 11.97V. The
  simulation did not show the problem the built board had with no load
  at all: the 5V rail crept up to about 6.1V within seconds, through
  leakage in U4 that the model does not include (D5 then clamps it). R33,
  470Ω across the 5V output, fixes it (10.6 mA, measured 4.91–4.96V).
  On rev C boards, which have no footprint for it, R33 is soldered across
  D5's pads.
- **Switch.** GP18 → R22 10kΩ → Q2 base; R23 100kΩ base pull-down; Q2
  collector → R25 22kΩ → Q1 gate; R24 47kΩ gate–source; C22 1µF
  gate–drain and C11 10µF gate–source (see Inrush). GP18 high ⇒
  Vgs ≈ −13V (≤ 20V max) ⇒ rails on. GP18 floating (reset, BOOTSEL, Pico
  unpowered) ⇒ Q2 off ⇒ Q1 off ⇒ drive off. The firmware raises GP18 in
  `bdev_init` only after `floppy_init` released every interface line, then
  waits `FLOPPY_DRIVE_POWER_SETTLE_MS` (1 s; the rails are up 66–77 ms
  after GP18, the TEAC spec gives < 400 ms of drive power reset including
  auto-recalibration) before core 1 probes. At every boot it first holds
  GP18 low for `FLOPPY_DRIVE_POWER_CYCLE_MS` (3.5 s). Two reasons: a
  ~30 ms reboot (watchdog, reflash, RUN tap) would otherwise only brown the
  drive out (suspected on the YD-380B: after quick reboots DSKCHG came
  back clear instead of set), and the ~1 mF behind Q1 needs 2.6 s to bleed
  below the regulators' 7.9V lockout (R26, simulated, +20% capacitor
  tolerance covered by 3.5 s): re-enabled before that, the fixed
  LM2678/LM2676 start hard into a charged input and overshoot to 12.7V /
  5.7V (no internal soft start, simulated). Started after the hold, the
  rails rise through Q1's ramp and peak at 12.10V / 5.12V. The firmware
  never switches the drive off on its own otherwise: a power cycle
  re-arms the DSKCHG latch, which would look like a disk swap to the host.
  There is no "force on" jumper: the power block is checked with the
  firmware running and nothing on J5.
- **Inrush.** There is ~1 mF behind Q1 (C8 + C12 + C16). A gate–source capacitor alone only delays Q1, it does not
  limit the current. C22 (gate–drain) turns Q1 into a Miller integrator:
  at the gate plateau R25 pulls ~0.62 mA, so the switched node rises at
  Ig / C22 ≈ 0.6 V/ms: a ~31 ms ramp drawing 1.86 A peak from the adapter
  (2.4 A with X7R DC-bias derating; simulated, `hardware/simulation/run_inrush.py`),
  within a 45W adapter's rating. C11 = 10 × C22 keeps Q1 off when the
  adapter is plugged in (simulated hot-plug: Vgs −2.7V, the switched node
  stays below 2V, rails at 0). Turn-off is slow (~0.7 s: the gate
  discharges through R24) but unloaded, since the regulators stop first.
  Check at the final acceptance test: scope 19.5V at J4 and `19V5_SW`
  while the firmware raises GP18 (3.5 s after the UART banner): a clean
  ~30 ms ramp, no adapter hiccup.
- **Switch-off.** GP18 low stops both regulators at once (the rails fall
  into the drive in ~10 ms on 5V, ~40 ms on 12V), Q1 opens ~0.7 s later
  with nothing drawing, and R26 2.2kΩ (173 mW while on) discharges the
  ~1 mF below the regulators' lockout in 2.6 s and to a few volts in 4 s.
  Simulated without the ON/OFF control, Q1 alone left the bus sitting at
  the 7.9V lockout with 5V still on and 2.5W in Q1, which is why the
  ON/OFF pins are driven. A power cut during a write can leave
  the sector being written unreadable (writes are per sector; bench test
  B9 checks that nothing else is affected).
- **Adapter sense.** R31 330kΩ / R32 39kΩ divide `19V5_F` (before Q1) by
  9.46 onto GP26 (ADC0), C24 100nF at the pin: 19.5V reads 2.06V, the
  D3 clamp (30V) 3.17V, 57µA at most into an unpowered Pico pad. The
  firmware (`drive_power_poll`, every 50 ms; `FLOPPY_ADAPTER_SENSE_PIN`,
  `FLOPPY_ADAPTER_ON_MV` 16000, `FLOPPY_ADAPTER_OFF_MV` 15000 in
  `pins.h`) enables the drive only with the adapter above 16V and drops
  GP18 at once below 15V, then re-applies the 3.5 s hold. With no adapter
  at boot the log says `adapter not detected` and the drive stays off
  until one appears. Without it, an adapter re-seated or browning out while the
  drive is on would find Q1 still enhanced (its gate follows the sagging
  bus): the re-plug is an 18 A step into the 1 mF (adapter and L5
  saturating, Q1 150 W for 100 µs) and a hard regulator restart with the
  12.7V / 5.7V overshoot (simulated). With the
  sense, the regulators stop as the bus falls, Q1 opens unloaded and the
  re-plug is either a small step or the normal soft ramp.
- **Rail sequencing.** At switch-on the 5V rail regulates as soon as the
  input passes ~7–8V, the 12V rail only above ~14V: 5V leads 12V by a few
  milliseconds, like an ATX supply (simulated: ~10 ms; the drive tolerates
  either order, 12V within 320 ms of 5V).
- **Thermal.** At the drive's load (well under 1A average per rail) the
  TO-220 regulators and catch diodes need no heatsink; check their case
  temperature after 10 minutes of seeking at the first bring-up. Q1's tab
  is live (drain = switched 19.5V): nothing may touch it.
- **Drive power pinout (drive side, pin 1 nearest the drive's edge
  connector): 1 = +12V (yellow), 2 = GND (black), 3 = GND (black),
  4 = +5V (red).** Wire the cut extension into J5 by colour, then
  meter-check the harness pin by pin at the free plug against the wire
  colours, adapter unplugged, and again live at the power-block test
  (yellow = 12V, red = 5V) before the drive is ever connected: a 12V/5V
  swap kills the drive's logic.
- The Pico itself is powered from USB as usual; its VBUS feeds the 7407s.
  Never feed the drive's 12V/5V into the logic side.

## Drive-less smoke test (mandatory before the drive is ever connected)

Board assembled, ribbon NOT connected to the drive, nothing on J5, adapter
unplugged:

0. **Power block, before anything else** (nothing on J5,
   `floppy_revival.uf2` flashed; the vdisk build never raises GP18;
   7407s may still be absent): shorts check 19V5 IN + to GND, J5
   pin 1 to GND, J5 pin 4 to GND, J5 pin 1 to pin 4: no beep. Read U3/U4
   markings once more (LM2678T-**12** in U3, LM2676T-**5.0** in U4). Meter
   the polarity of the jack leads before they go into J4. Plug the adapter,
   USB unplugged: J5 pins 1 and 4 read **0V** (Q1 and the regulators are
   off while GP18 floats). Plug the USB with the scope on J4: 3.5 s after
   the UART banner the firmware prints `drive power on (GP18, adapter
   19xxx mV)` (the measured adapter voltage must read 18.5–20.5V for a
   19.5V adapter; `adapter not detected` = R31/R32/C24 or the jack) and
   the adapter sees the ~30 ms inrush ramp (see Power); then J5 pin 1 =
   11.5–12.5V, pin 4 = 4.75–5.25V, pins 2/3 = 0V to GND. Anything else
   (5V on pin 1, 12V on pin 4, near 0V, ~19V) → stop and find out why
   before going further. Then load each rail (e.g. 24Ω 10W on 12V, 10Ω 5W
   on 5V) and check the ripple at J5 with the probe's ground spring: target
   < 50 mV p-p on 12V, < 20 mV p-p on 5V. Pull the adapter plug with the
   USB still on: the UART prints `drive power OFF` within 50 ms and J5
   falls; plug it back: `drive power on` follows 3.5 s later with the
   same soft ramp. Unplug the USB: J5 must drop to 0V within
   milliseconds. Unplug the adapter.
1. Shorts check (meter, unpowered): VBUS–GND, 3V3–GND, every header even
   pin to GND (must NOT beep except odd pins).
2. Power via USB. Measure VBUS at each 7407 VCC pin: ≥ 4.75V. Measure 3V3.
3. **/WGATE proof:** scope or meter on header pin 24 (TP1). It must stay
   HIGH (≈5V via the drive-side pull-up; for this test, temporarily pull
   the header line to 5V through 1kΩ, or watch the 7407 output with the
   board's 3.3V pull-up on its input) through: USB plug-in, firmware boot,
   RUN reset, BOOTSEL entry, UF2 reflash, and 10 minutes of idle. Any low
   blip = stop, debug before the drive ever sees this board.
4. Same check, quicker, on MOTOR (16, TP2), DS1 (12), STEP (20): released
   at idle.
5. UART logs at 115200 on J2: `floppy_init: 0` expected (no drive: probe
   failures are normal).
6. Ground each input line (8, 26, 28, 30, 34) briefly through the header:
   the firmware should see INDEX/TRK00/WPT/RDATA/DSKCHG change (visible in
   probe behaviour/logs where applicable).
7. **Sequencing proof:** adapter plugged, USB plugged: the UART shows
   `drive power on (GP18, ...)` 3.5 s after `floppy_init: 0` and J5 pin 1 goes
   to 12V only then. Hold RUN to GND for a second: the rails must drop at
   once and come back 3.5 s after the banner. A brief tap must do the same
   (the firmware holds the drive off for 3.5 s at every boot). Enter
   BOOTSEL: rails off.

## Reflashing with the drive attached

1. Eject/unmount the volume on the host; wait for the GP16 status LED to go
   out (steady = unflushed writes; LED meanings in [user-guide.md](user-guide.md)).
2. Reflash: hold BOOTSEL + short RUN to GND (TP9), or unplug USB, hold
   BOOTSEL, re-plug. Entering BOOTSEL floats GP18 and the drive rails drop
   by themselves. Copy the UF2.
3. After the firmware boots (UART banner) it re-powers the drive itself,
   3.5 s later, once it measures the adapter.

## Protecting original disks

- Modern hosts **write to a floppy at mount time** (metadata, indexing).
  Any disk you cannot afford to lose goes in with the write notch covered
  AND the GP17 read-only jumper (J3) closed; for imaging sessions build
  with `-DFLOPPY_FORCE_READ_ONLY=1`.
- All write testing (REDWC polarity included) happens on sacrificial disks.
- The firmware watchdog (8 s) resets the Pico if the firmware locks up.
  Core 0 feeds it from its main loop (and while a host command waits on
  the drive), and stops feeding it once core 1's heartbeat, refreshed at
  the start of every drive operation and write attempt, is older than
  22 s (the longest write with its retries is allowed 20 s). A hung drive
  core is therefore reset within about 30 s, a hung USB core within 8 s.
  The reset releases every drive line via the pull-ups (the write gate
  cannot stay asserted) and the firmware power-cycles the drive at boot
  (GP18 held low for 3.5 s). Nothing survives a watchdog reboot: writes
  still in the cache are lost, and the host is not told beyond seeing the
  device disconnect and re-enumerate (the volume vanishes without being
  ejected): acceptable versus a stuck write gate. Only the UART log
  records it: if it shows "recovered by watchdog", investigate before
  writing again.
- Repeated TRK00 failures latch after 3 attempts (log:
  `TRK00 fault latched`) instead of endlessly ramming the head against the
  stop; reset the Pico (USB replug) after fixing the wiring. A missing
  adapter is reported as such (`adapter not detected` / `drive power
  OFF`, the drive is never enabled); a tripped F1 or a dead rail with the
  adapter present looks like an empty drive and does not trip this latch.

## Bring-up checklist

Build instructions: [assembly.md](assembly.md).

1. Flash `floppy_revival_vdisk.uf2` first: verifies USB/UFI enumeration and
   mounting with no drive attached. **That build never raises GP18** (it
   has no drive code): the power rails stay off under it.
2. Connect UART0 on J2 (GP0 TX → USB-serial RX, 115200).
3. Flash `floppy_revival.uf2`, then run the full drive-less smoke test
   above (its step 0 needs `drive power on (GP18)` from this build). Only
   then cable the drive.
4. Plug the adapter (the firmware switches the drive on as soon as it
   measures it, 3.5 s after boot at the earliest), no disk inserted: log
   shows `floppy_init: 0`, `drive power on (GP18, adapter … mV)` and
   periodic probe attempts failing with **`MEDIA_CHANGED`**:
   the DSKCHG latch cannot clear without a disk (the motor stays off
   during these attempts: the latch is tried before spin-up). `TIMEOUT`
   instead would point at the DSKCHG line (pin 34/GP5) or at an
   unpowered drive.
5. Scope 12V at J5 during the first spindle start: no dip below 11.4V, no
   ringing; check the regulators' temperature after a few minutes of use.
6. Insert a known-good formatted disk: expect `probe <format>: 0` then
   `media: ...`, and the volume mounts on the host.
7. Validate the density line polarity **by writing, on sacrificial disks
   only**: write a file to a 1.2MB disk, power-cycle the drive, read it
   back; same with a 360KB disk. A read-only test cannot discriminate:
   wrong write current degrades the written track silently. If one of the
   two fails read-back, rebuild with `-DFLOPPY_DENSITY_HD_RELEASED=0` and
   retest.
8. First real writes: sacrificial disk; verify with a read-back on a real
   vintage machine if available.
9. If reads are marginal, tune in `firmware/src/floppy/floppy.c`: `FLOPPY_STEP_MS`
   (360KB-era drives may want much larger values: prior art uses up to
   65ms/step, see Adafruit_Floppy), `FLOPPY_STEP_REVERSE_MS` (15 ms rest
   before a step in the other direction), `FLOPPY_HEAD_SETTLE_MS`, and check
   `stats` counters (crc/overruns) via a debug print.
