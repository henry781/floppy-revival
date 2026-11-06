# Pin mapping: Pico 2 to the 34-pin drive connector

Checked against the YE Data YD-380B (jumpers and measurements below) and
in use on the built board. GP2 to GP14 are in the order the signals reach
the drive connector on the board, so the data bus is routed without
crossings. No PIO constraint ties any two
signals together (the read program runs on pio0, the write program on pio1,
each on single pins), so the order is free.

On the 34-pin interface all **odd pins are ground**; signals are on even pins,
active-low, open-collector. The drive's terminator pack terminates only the
drive's **inputs**: **measured 1kΩ on this unit** (see
[connector measurements](#connector-measurements)); the drive's **outputs** are bare
open-collector (measured open), so the pull-ups for those are on the interface board
(see electrical notes).

Signal names below are the **drive-side** names for the planned cabling:
straight-through 34-way cable, drive jumpered DS1 (its factory default).

| Drive pin | Signal | Dir (Pico view) | Pico 2 GPIO | Notes |
|-----------|-----------|-----------------|-------------|-------|
| 2 | /REDWC (density) | out | GP12 | Active on the YD-380B (JP13 fitted): firmware switches it per probed format (HD released, DD asserted) |
| 4 | reserved (head load on some drives) | — | — | Leave unconnected (measured open) |
| 6 | DS3 / reserved | — | — | Leave unconnected (measured open) |
| 8 | /INDEX | in | GP14 | One pulse per revolution |
| 10 | DS0 (unused by drive) | — | — | Leave unconnected: measured unterminated; motor is NOT read from pin 10 |
| 12 | DS1 (/DRVSB) | out | GP13 | The select the drive obeys (DS1 jumper, straight cable) |
| 14 | DS2 | — | — | Leave unconnected (only needed for the twisted-cable option, unsupported by firmware) |
| 16 | /MOTOR ON | out | GP11 | **Mandatory**: the YD-380B takes Motor Enable ONLY from pin 16; GP11's buffer gate drives this pin alone |
| 18 | /DIR | out | GP10 | Low = step inward (toward higher tracks) |
| 20 | /STEP | out | GP9 | Head moves on the trailing (rising) edge of the active-low pulse |
| 22 | /WDATA | out | GP8 | PIO-generated MFM flux transitions |
| 24 | /WGATE | out | GP7 | Enables write head: interlock with WPT in firmware |
| 26 | /TRK00 | in | GP2 | Low when head on track 0 |
| 28 | /WPT | in | GP3 | Low when disk is write-protected |
| 30 | /RDATA | in | GP4 | PIO flux capture input |
| 32 | /SIDE1 | out | GP6 | Low = head 1 |
| 34 | /DSKCHG | in | GP5 | Disk change on the YD-380B (JP5+JP7 factory default); some 360KB drives put READY here instead |

GP0/GP1 stay reserved for UART0 debug stdio.

Optional status LEDs, jumper and drive power switch (disable with
`-DFLOPPY_LED_*_PIN=-1` / `-DFLOPPY_READONLY_PIN=-1` /
`-DFLOPPY_DRIVE_POWER_PIN=-1` at cmake configure time):

| Pico 2 GPIO | Function | Wiring / meaning |
|-------------|----------|------------------|
| GP15 | activity LED | GPIO → 330Ω → LED anode; cathode → GND. Steady while a readable disk is mounted, off with no disk; flickers (dark first) on host requests and drive work |
| GP16 | status LED | same wiring. Steady: unflushed writes: **don't remove the disk while lit**. Slow blink (1 Hz): disk spins but reads in no format (reported to the host as unformatted; format it or change it). 2 Hz: a write to this disk failed and was reported to the host (sectors dropped, or writes lost with a pulled disk): check the file, change the disk. Fast blink (4 Hz): drive fault (TRK00 latched, or drive init failed) |
| GP17 | read-only jumper | 2-pin header, GPIO to GND when closed (internal pull-up). Closed = firmware refuses ALL writes, whatever the host or the disk notch say |
| GP18 | drive power | GPIO → 10kΩ → Q2 (BC547) → Q1 (IRF9540N high-side switch on the 19.5V input) and → 1kΩ → the regulators' ON/OFF pins (`DRV_EN`). High = drive 12V/5V rails on; low = regulators stop at once. 100kΩ base pull-down and 4.7kΩ `DRV_EN` pull-down keep the drive OFF while the pin floats (reset, BOOTSEL, Pico unpowered). Held low 3.5 s at every boot (`FLOPPY_DRIVE_POWER_CYCLE_MS`). Drive on its own supply: build with `-DFLOPPY_DRIVE_POWER_PIN=-1` |
| GP26 | adapter sense (ADC0) | `19V5_F` ÷ 9.46 through R31 330k / R32 39k, 100nF at the pin: 19.5V = 2.06V. The firmware enables the drive above 16V and drops GP18 below 15V (`FLOPPY_ADAPTER_SENSE_PIN`, -1 to disable) |

## Target drive: YE Data YD-380B (board PN128206-01 Rev. B)

Factory jumper defaults, all compatible with this design as-is
(jumper reference: the PN128206-01 Rev. B sheet on [wiki.joschy.net](https://wiki.joschy.net)):

- **DS1** fitted → straight cable, select on pin 12. Do not move it.
- **JP5 + JP7** (0Ω) → pin 34 outputs **Disk Change**, which the firmware
  requires. Do not rejumper it to READY.
- **JP13** (0Ω) → pin 2 density select is **connected**, so /REDWC must be
  driven correctly (the firmware does, per probed format).
- **JP12 absent** → spindle fixed at 360 RPM (360KB media handled at
  300 kbps by `FORMAT_525_360K_HD_DRIVE`).
- **JP8 absent** → motor controlled by Motor Enable (pin 16), not by select.
- **In PC-AT mode the drive reads Motor Enable ONLY from pin 16**: GP11's
  buffer gate drives pin 16 alone; pin 10 stays unconnected (measured
  unterminated on this unit).
- Terminator: **measured on this unit: 1kΩ pack, fitted** (see
  [connector measurements](#connector-measurements)). Single drive on the cable ⇒ it stays
  fitted. Per-line sink is then only ~5.7mA (VOL ≤ 0.4V vs the drive's
  0.5V input-low limit). A 330Ω-pack drive is still covered; a 150Ω pack
  (35mA, VOL up to 0.7V) only typically.

## Electrical notes

- **Inputs (drive → Pico):** the drive's outputs are bare open-collector
  (7438-class, no drive-side pull-up: the terminator only serves the
  drive's inputs). Each line gets a **mandatory 1kΩ pull-up to 3.3V** on our
  board plus a 330Ω series resistor to the Pico pin. The RP2350's internal
  pull-ups (~50kΩ) cannot recharge RDATA between MFM pulses through the
  cable capacitance: without the 1kΩ pull-ups, reads fail outright.
  RP2350 GPIOs are 5V-tolerant only with IOVDD at 3.3V and the chip powered
  (see the power-sequencing rule in [hardware.md](hardware.md#wiring-rules)); with the pull-ups
  on 3.3V the lines never sit at 5V anyway.
- **Outputs (Pico → drive):** ~4.6mA sink per line against this unit's
  measured 1kΩ terminators (~35mA if a 150Ω-pack drive is ever substituted,
  beyond the RP2350's ~12mA pad rating, and at the SN7407N's 0.7V VOL
  point). Use the **non-inverting
  open-collector SN7407N (40mA IOL)**. Never an inverting 7406/74LS06: the
  firmware releases lines by tri-stating its GPIO, a floating TTL input
  reads high, and an inverter would then assert every released line (write
  gate included) from power-on. Do not drive these pins push-pull from the
  Pico directly.
- **Power:** drive takes +5V and +12V on the 4-pin Molex; ~9–10W peak during
  seek+write. Keep drive power separate from USB bus power; common ground
  between Pico, buffer board, and drive is mandatory.
- **/WGATE safety:** 10kΩ pull-up **to 3.3V** on the GP7/7407-input node so
  the buffer output stays released while the Pico floats during reset/boot
  (a stray write gate pulse can corrupt a track). Same 10kΩ to 3.3V on every
  other used 7407 input.

## Connector measurements

Resistance from each pin of the drive's edge connector to its +5 V rail,
drive unpowered and disconnected (YD-380B, board PN128206-01 Rev. B,
factory jumpers). X = open.

| Pin   | 5V resistance | Pin   | 5V resistance |
|-------|---------------|-------|---------------|
| 2     | 0.99KΩ        | 1     | 8.37KΩ        |
| 4     | X             | 3     | 8.37KΩ        |
| notch |               | notch |               |
| 6     | X             | 5     | 8.37KΩ        |
| 8     | X             | 7     | 8.37KΩ        |
| 10    | X             | 9     | 8.37KΩ        |
| 12    | 0.99KΩ        | 11    | 8.37KΩ        |
| 14    | X             | 13    | 8.37KΩ        |
| 16    | 0.99KΩ        | 15    | 8.37KΩ        |
| 18    | 0.99KΩ        | 17    | 8.37KΩ        |
| 20    | 0.99KΩ        | 19    | 8.37KΩ        |
| 22    | 0.99KΩ        | 21    | 8.37KΩ        |
| 24    | 0.99KΩ        | 23    | 8.37KΩ        |
| 26    | X             | 25    | 8.37KΩ        |
| 28    | X             | 27    | 8.37KΩ        |
| 30    | X             | 29    | 8.37KΩ        |
| 32    | 0.99KΩ        | 31    | 8.37KΩ        |
| 34    | X             | 33    | 8.37KΩ        |

### Interpretation

- **Termination is 1kΩ** (0.99kΩ measured), not the 150Ω of early drives.
  Worst-case sink per asserted line ≈ (5V − V_OL)/1k ≈ 4.6mA: the SN7407N
  (40mA IOL) has huge margin.
- **Terminated (drive inputs):** pins 2 (REDWC), 12 (DS1, the fitted select),
  16 (MOTOR ON), 18, 20, 22, 24, 32.
- **Open (X):**
  - Pin 10 (DS0) and 14 (DS2): select lines are terminated only *after* the
    DS jumper (matches maintenance-manual drawing 138012-11) → **pin 10 is
    unterminated and unused; leave it unconnected**, drive pin 16 alone.
  - Pins 8, 26, 28, 30, 34 (INDEX, TRK00, WPT, RDATA, DSKCHG): the drive's
    **outputs are bare open-collector with no internal pull-up**: the 1kΩ
    pull-ups to 3.3V on the interface board are mandatory.
  - Pins 4 and 6: not connected on this drive. Leave unconnected.
- Odd pins → +5V read ≈ 8.37kΩ: the drive's internal 5V-rail-to-ground
  resistance seen from the ground plane; all odd pins are ground, as expected.
