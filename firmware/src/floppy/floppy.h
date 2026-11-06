// Low-level drive control + DMA flux engine.
// Derived from jackdoe/pico-mfm-floppy src/floppy.h (MIT). Changes for this
// project: runtime floppy_format_t (set via floppy_set_format before track
// I/O) and double-step support for 360KB media in 96tpi drives. Pin 34 must
// be DISK CHANGE: media swaps are detected only through it.

#ifndef FLOPPY_H
#define FLOPPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "disk.h"
#include "format.h"
#include "hardware/pio.h"

#define FLOPPY_FLUX_RING_BITS 12u
#define FLOPPY_FLUX_RING_BYTES (1u << FLOPPY_FLUX_RING_BITS)
#define FLOPPY_FLUX_RING_WORDS (FLOPPY_FLUX_RING_BYTES / 4u)
#define FLOPPY_TX_HALF_BYTES (FLOPPY_FLUX_RING_BYTES / 2u)
// Motor off (and deselect) after this long without I/O: the IBM PC BIOS
// value (INT 1Eh motor-off wait, 37 ticks = ~2 s). Spares media and heads;
// a resumed access pays ~0.5 s of spin-up. Stays above DRIVE_FLUSH_IDLE_MS
// so pending writes are flushed before the spindle stops.
#ifndef FLOPPY_IDLE_TIMEOUT_MS
#define FLOPPY_IDLE_TIMEOUT_MS 2000u
#endif
// Deadline of the longest drive operation (a track or sector write, with
// its retries and read-back). Every wait inside an operation is checked
// against its deadline, so no operation runs much longer than this.
#define FLOPPY_OPERATION_MAX_MS 20000u

typedef enum {
  FLOPPY_OPERATION_IDLE = 0,
  FLOPPY_OPERATION_CONTROL,
  FLOPPY_OPERATION_RAW,
  FLOPPY_OPERATION_READ,
  FLOPPY_OPERATION_WRITE,
} floppy_operation_t;

typedef struct {
  uint8_t index;
  uint8_t track0;
  uint8_t write_protect;
  uint8_t read_data;
  uint8_t disk_change;
  uint8_t drive_select;
  uint8_t motor_enable;
  uint8_t direction;
  uint8_t step;
  uint8_t write_data;
  uint8_t write_gate;
  uint8_t side_select;
  uint8_t density;
} floppy_pins_t;

typedef struct {
  PIO pio;
  uint sm;
  uint offset;
  uint16_t half;
  bool half_valid;
  uint16_t prev;
  bool primed;
} floppy_pio_t;

typedef struct {
  uint16_t delta;
  bool index;
} floppy_pulse_t;

typedef struct {
  uint32_t reads;
  uint32_t retries;
  uint32_t recovered;
  uint32_t failed;
  uint32_t timeout;
  uint32_t crc;
  uint32_t wrong_track;
  uint32_t wrong_side;
  uint32_t overruns;
  uint32_t underruns;
  uint32_t media_changes;
  uint32_t flux_words;
  uint32_t ring_peak;
  uint32_t dma_writes;    // full-track (format) writes
  uint32_t sector_writes; // sectors rewritten in place
  uint32_t late_gate;     // sector skipped: gap 2 ran out before /WGATE
  uint32_t not_found;     // sector ID never came round
  uint32_t collateral;    // neighbour sector unreadable after a write, rewritten
} floppy_stats_t;

typedef struct floppy {
  uint32_t flux_ring[FLOPPY_FLUX_RING_WORDS]
      __attribute__((aligned(1u << FLOPPY_FLUX_RING_BITS)));
  floppy_pins_t pins;
  const floppy_format_t *fmt;
  floppy_pio_t read;
  floppy_pio_t write;
  int dma_ch;    // RX: flux timestamps into flux_ring
  int dma_tx_ch; // TX: pulse operands into the write PIO
  uint32_t ring_cpu;
  uint8_t cylinder; // physical track under the head
  int8_t last_step_dir; // DIR_INWARD/DIR_OUTWARD of the last pulse, -1 none
  uint32_t last_step_ms; // when it was issued (ms since boot)
  bool track0_confirmed;
  bool disk_change_active;
  bool media_observed;
  bool read_overrun;
  bool motor_on;
  bool selected;
  bool motor_qualified;
  bool head_confirmed;
  uint8_t head;
  bool read_program_loaded;
  bool write_program_loaded;
  bool read_sm_claimed;
  bool write_sm_claimed;
  bool dma_claimed;
  bool dma_tx_claimed;
  bool gpio_configured;
  bool flux_active;
  uint32_t last_io_time_ms;
  uint32_t media_generation;
  uint32_t motor_generation;
  uint32_t flux_generation;
  floppy_operation_t operation;
  uint32_t operation_start_ms;
  uint32_t operation_limit_ms;
  uint32_t lifecycle;
  floppy_stats_t stats;
  // Called at the start of every operation and of every write attempt, so
  // a supervisor can tell a drive at work from a hung one (drive.c: core-1
  // heartbeat for the watchdog). NULL: none. Set after floppy_init.
  void (*heartbeat)(void);
} floppy_t;

disk_err_t floppy_init(floppy_t *f, floppy_pins_t pins);
// Select the media format for subsequent seeks and track I/O; drive must be
// idle. Resets track-0 calibration (double-step semantics may change).
disk_err_t floppy_set_format(floppy_t *f, const floppy_format_t *fmt);
disk_err_t floppy_poll(floppy_t *f);
disk_err_t floppy_select(floppy_t *f, bool on);
disk_err_t floppy_motor_on(floppy_t *f);
disk_err_t floppy_motor_off(floppy_t *f);
disk_err_t floppy_disk_changed(floppy_t *f, bool *changed);
// Idle-time DSKCHG check: briefly selects a deselected drive to sample pin
// 34 (a swap then shows up as a new media generation). Drive must be idle.
disk_err_t floppy_sample_disk_change(floppy_t *f);
disk_err_t floppy_media_generation(floppy_t *f, uint32_t *generation);
// Empty-drive check without motor or head movement, the drive selected for
// at most ~2 ms (FLOPPY_QUICK_CLEAR_MAX_US in floppy.c; its front LED barely
// lights): with the head at TRK00, one outward step pulse (the drive ignores
// it there) clears DSKCHG if a disk is in. *disk_in: the latch is (or
// became) clear, so a full probe is worth running. DISK_ERR_NO_TRACK0: the
// head is not at TRK00 and nothing was done; a full probe homes it. Drive
// must be idle, deselected.
disk_err_t floppy_quick_media_check(floppy_t *f, bool *disk_in);
disk_err_t floppy_write_protected(const floppy_t *f, bool *write_protected);
disk_err_t floppy_stats(const floppy_t *f, floppy_stats_t *stats);

disk_err_t floppy_flux_begin(floppy_t *f, uint32_t expected_generation);
disk_result_t floppy_flux_read(floppy_t *f, floppy_pulse_t *pulses, size_t capacity);
disk_err_t floppy_flux_end(floppy_t *f);

disk_err_t floppy_read_track(floppy_t *f, uint32_t expected_generation,
                             track_t *track);
// Format probing: as floppy_read_track, but a data rate that shows no MFM
// at all fails after one revolution instead of going through head-jog
// recovery (the next candidate format is the cure).
disk_err_t floppy_probe_track(floppy_t *f, uint32_t expected_generation,
                              track_t *track);
// Rewrite the sectors in `mask` in place, uPD765 WRITE DATA style: each
// sector is written right after its own ID came by, then the track is read
// back and every sector in track->valid compared. *written collects the
// sectors of `mask` confirmed on the medium, whatever the return code;
// *damaged the other sectors of track->valid the write disturbed and could
// not restore within its attempts (still bad on the medium).
disk_err_t floppy_write_sectors(floppy_t *f, uint32_t expected_generation,
                                const track_t *track, uint32_t mask,
                                uint32_t *written, uint32_t *damaged);
// FORMAT: write the whole track index to index (gap 4a, IAM, every sector,
// BIOS gap 3), then read it back. The head position is trusted from the
// step count when the track carries no readable ID; an ID naming another
// cylinder still forces a recalibration.
disk_err_t floppy_format_track(floppy_t *f, uint32_t expected_generation,
                               const track_t *track);
// Forget the head position: the next seek recalibrates on TRK00 (used at
// the start of a format). Drive must be idle.
disk_err_t floppy_invalidate_position(floppy_t *f);

disk_device_t floppy_device(floppy_t *f);

// FLOPPY_DEBUG builds only (bench monitor): one raw step / recalibrate with
// no index qualification. Read-only head moves; /WGATE is never touched.
disk_err_t floppy_debug_step(floppy_t *f, bool inward);
disk_err_t floppy_debug_home(floppy_t *f);

#endif
