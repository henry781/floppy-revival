// Shared between drive.c (the drive agent) and drive_debug.c (the bench
// monitor of the floppy_revival_debug build). Nothing here is for the rest
// of the firmware: the public interface is blockdev.h.

#ifndef DRIVE_INTERNAL_H_
#define DRIVE_INTERNAL_H_

#include <stdbool.h>
#include <stdint.h>
#include "disk.h"
#include "floppy.h"
#include "format.h"
#include "pins.h" // FLOPPY_DRIVE_POWER_PIN, tested below

enum {
    MEDIA_NONE = 0,
    MEDIA_UNFORMATTED, // disk spinning, no format reads (the host may FORMAT it)
    MEDIA_READY,
};

extern floppy_t drive;
extern volatile uint8_t media_state;
extern const floppy_format_t *volatile media_fmt;
#if FLOPPY_DRIVE_POWER_PIN >= 0
// Last adapter reading (mV) taken by core 0, for the debug status line.
extern volatile uint32_t adapter_last_mv;
#endif
// Outcome of the last probe attempt (core 1 only; see drive.c for the
// DISK_ERR_MEDIA_CHANGED = "no disk" convention).
extern disk_err_t last_probe_err;

// Probe the inserted disk now (core 1 only). True when a format mounted.
// Only the probe itself: the retry pacing, unformatted reporting, parking
// and TRK00 fault counting of the automatic probe stay in drive.c.
bool core1_try_probe(void);
// Refresh the core-1 heartbeat the watchdog check reads (bdev_healthy).
void core1_beat(void);

#if FLOPPY_DEBUG
void drive_debug_init(void);
void drive_debug_poll(void);
// False while the monitor has paused automatic probing of an empty drive.
bool drive_debug_auto_probe(void);
// Set by the monitor's 'B' (core 1) once it has parked the drive, acted on
// by core 0 (core0_background, also during command waits); core 1 then
// stops for good. On the RP2350 the ROM reboots through a 10 ms watchdog
// timeout, which core 0's watchdog_update would keep reloading if core 1
// called it alone.
extern volatile bool drive_debug_bootsel_request;
// Set by the monitor's 'P' (core 1), served by core 0: power-cycle the drive.
extern volatile bool drive_debug_power_cycle_request;
#endif

#endif // DRIVE_INTERNAL_H_
