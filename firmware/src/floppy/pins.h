// Default Pico 2 GPIO assignment for the 34-pin drive interface.
// Must match docs/pinmap.md; GP0/GP1 are reserved for UART0 debug stdio.
// Rev C order: GP2..GP14 follow the board routing (the Pico's left column,
// top to bottom, in the order of the drive connector pins), so the bus to
// the 7407s and J1 runs without crossings. Any GPIO works for any signal:
// every pin is driven or sampled on its own (PIO programs use one pin each).

#ifndef FLOPPY_PINS_H_
#define FLOPPY_PINS_H_

#include "floppy.h"

#define FLOPPY_PINS_DEFAULT                                              \
    (floppy_pins_t) {                                                    \
        .track0 = 2, .write_protect = 3, .read_data = 4,                 \
        .disk_change = 5, .side_select = 6, .write_gate = 7,             \
        .write_data = 8, .step = 9, .direction = 10, .motor_enable = 11, \
        .density = 12, .drive_select = 13, .index = 14,                  \
    }

// Optional status LEDs (set to -1 to disable), both written by core 0 only
// (see bdev_update_leds in drive.c for the patterns).
#ifndef FLOPPY_LED_ACTIVITY_PIN
#define FLOPPY_LED_ACTIVITY_PIN 15 // disk mounted; flickers on activity
#endif
#ifndef FLOPPY_LED_DIRTY_PIN
#define FLOPPY_LED_DIRTY_PIN 16 // unflushed writes; blinks on disk/drive fault
#endif

// Read-only jumper (set to -1 to disable): input with pull-up, close to GND
// to block all writes regardless of the disk's notch or the host's wishes.
#ifndef FLOPPY_READONLY_PIN
#define FLOPPY_READONLY_PIN 17
#endif
// Build-time hard read-only (-DFLOPPY_FORCE_READ_ONLY=1): for imaging
// sessions on precious disks.
#ifndef FLOPPY_FORCE_READ_ONLY
#define FLOPPY_FORCE_READ_ONLY 0
#endif

// Drive power switch (rev B/C boards; set to -1 on rev A / external PSU).
// GPIO -> Q2 -> Q1 high-side P-MOSFET on the board's 19.5V input and, on
// rev C, -> R29 -> the regulators' ON/OFF pins. High = drive rails on; low
// stops the regulators at once. R23 and R30 hold it off while the pin
// floats (reset, BOOTSEL, Pico unpowered), so the drive can never be
// powered before the Pico is.
#ifndef FLOPPY_DRIVE_POWER_PIN
#define FLOPPY_DRIVE_POWER_PIN 18
#endif
// Time for the drive supply (Q1's ~30 ms Miller ramp, then the regulators;
// simulated: rails up 66-77 ms after GP18) and the drive's own power-on reset
// to settle before core 1 starts probing. The TEAC FD-55GFR spec gives
// < 400 ms for the drive's power reset including auto-recalibration.
#ifndef FLOPPY_DRIVE_POWER_SETTLE_MS
#define FLOPPY_DRIVE_POWER_SETTLE_MS 1000
#endif
// At every boot the drive is held OFF this long before power-up, so it
// always gets a full power-on reset (a hung write after a watchdog reset
// really ends in a drive power cycle, a quick reboot is no brown-out). On
// rev C GP18 low stops both regulators at once through their ON/OFF pins
// (the rails fall in ~10 ms on 5V, ~40 ms on 12V) while Q1 turns off ~0.7 s
// later, unloaded; the ~1 mF behind Q1 then bleeds through R26 (2k2) and is
// below the regulators' 7.9V lockout 2.6 s after GP18 dropped (simulated,
// specs/sim/run_off.py). Re-enabling earlier would be a hard ON/OFF start
// into a charged input: the fixed LM2678/LM2676 have no soft start and
// overshoot to 12.7V / 5.7V (simulated). 3.5 s covers the +20% capacitor
// tolerance.
#ifndef FLOPPY_DRIVE_POWER_CYCLE_MS
#define FLOPPY_DRIVE_POWER_CYCLE_MS 3500
#endif

// Adapter voltage sense (rev C): 19V5_F (before the Q1 switch) through
// R31 330k / R32 39k onto ADC0 (GP26): 19.5V reads 2.06V. The drive rails
// are only enabled while the adapter is present, and GP18 drops at once
// when it goes away (re-seated plug, brown-out): otherwise Q1 stays
// enhanced for seconds and a re-plug is an 18 A step into the input
// capacitors that also restarts the regulators hard (simulated). Set to -1
// on a board without the divider (rev B): GP18 is then driven blindly.
#ifndef FLOPPY_ADAPTER_SENSE_PIN
#define FLOPPY_ADAPTER_SENSE_PIN 26
#endif
// Divider ratio (R31 + R32) / R32 and the hysteresis thresholds in millivolts
// at the adapter: enable above 16.0V, disable below 15.0V (18-21V adapters;
// the regulators need > 13.6V for 12V out).
#define FLOPPY_ADAPTER_DIVIDER_X1000 9462u
#define FLOPPY_ADAPTER_ON_MV 16000u
#define FLOPPY_ADAPTER_OFF_MV 15000u

// Bring-up build (floppy_revival_debug target): per-second line status,
// probe step tracing and a single-key monitor on UART0.
#ifndef FLOPPY_DEBUG
#define FLOPPY_DEBUG 0
#endif

// The auxiliary pins are compile-time constants: refuse duplicates here
// (the runtime check only guards against the drive-interface pins).
#define FLOPPY_PINS_SAME_(a, b) ((a) >= 0 && (a) == (b))
#if FLOPPY_PINS_SAME_(FLOPPY_LED_ACTIVITY_PIN, FLOPPY_LED_DIRTY_PIN) || \
    FLOPPY_PINS_SAME_(FLOPPY_LED_ACTIVITY_PIN, FLOPPY_READONLY_PIN) || \
    FLOPPY_PINS_SAME_(FLOPPY_LED_ACTIVITY_PIN, FLOPPY_DRIVE_POWER_PIN) || \
    FLOPPY_PINS_SAME_(FLOPPY_LED_DIRTY_PIN, FLOPPY_READONLY_PIN) || \
    FLOPPY_PINS_SAME_(FLOPPY_LED_DIRTY_PIN, FLOPPY_DRIVE_POWER_PIN) || \
    FLOPPY_PINS_SAME_(FLOPPY_READONLY_PIN, FLOPPY_DRIVE_POWER_PIN)
#error "FLOPPY_LED_*_PIN, FLOPPY_READONLY_PIN and FLOPPY_DRIVE_POWER_PIN must be distinct"
#endif

#endif // FLOPPY_PINS_H_
