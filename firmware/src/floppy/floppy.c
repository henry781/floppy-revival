// Derived from jackdoe/pico-mfm-floppy src/floppy.c (MIT).
// Project-specific changes:
//  - geometry/data-rate comes from a runtime floppy_format_t (floppy_set_format)
//  - f->cylinder tracks the PHYSICAL track; logical cylinders are multiplied
//    by two when the format requests double-stepping (360KB media in a 96tpi
//    drive)

#include "floppy.h"
#include "flux_read.pio.h"
#include "flux_write.pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "mfm.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include <string.h>

// Bring-up tracing (-DFLOPPY_DEBUG=1, the floppy_revival_debug target):
// every select/motor/DSKCHG/index/TRK00 decision is printed on UART0.
#ifndef FLOPPY_DEBUG
#define FLOPPY_DEBUG 0
#endif
#if FLOPPY_DEBUG
#include <stdio.h>
#define FTRACE(...) printf("  [fd] " __VA_ARGS__)
#else
#define FTRACE(...) ((void)0)
#endif

#define DIR_INWARD 1
#define DIR_OUTWARD 2
#define FLOPPY_FLUX_DMA_COUNT 0x0FFFFFFFu
#define FLOPPY_READ_REVOLUTIONS 4u
#define FLOPPY_READ_TRANSITIONS_MAX 500000u
#define FLOPPY_READ_DEADLINE_MS 5000u
#define FLOPPY_WRITE_DEADLINE_MS FLOPPY_OPERATION_MAX_MS
#define FLOPPY_CONTROL_DEADLINE_MS 7000u
#define FLOPPY_WRITE_ATTEMPTS 3u
#define FLOPPY_HEAD_SETTLE_MS 20u
#define FLOPPY_MOTOR_DEADLINE_MS 2000u
// 150..250ms covers both 360 RPM (166.7ms) and 300 RPM (200ms) spindles.
#define FLOPPY_INDEX_PERIOD_MIN_MS 150u
#define FLOPPY_INDEX_PERIOD_MAX_MS 250u
#define FLOPPY_INDEX_PERIOD_SLOP_MS 25u
#define FLOPPY_TX_FIFO_DEPTH 8u
#define FLOPPY_TX_POLL_INTERVAL 32u
#define FLOPPY_WRITE_TRANSITIONS_MAX 200000u
// Longest track any format has: one revolution at 500 kbps and 300 rpm.
#define FLOPPY_TRACK_BYTES_MAX 12500u
#define FLOPPY_LIFECYCLE 0x464C5059u
// Sector writes: index boundaries without any sector written before the
// pass gives up (3 boundaries = at least two full revolutions).
#define FLOPPY_SECTOR_PASS_BOUNDARIES 3u
// /WGATE is asserted this long before the first write pulse so the write
// current is established (drive specs ask for a few microseconds).
#define FLOPPY_WGATE_SETUP_US 4u
// Gate opening later than this past its nominal time skips the sector for
// this revolution instead of writing into the old data field's sync.
#define FLOPPY_GATE_LATE_LIMIT_BYTES 2u

#ifndef NUM_BANK0_GPIOS
#define NUM_BANK0_GPIOS 30u
#endif

#ifndef FLOPPY_FLUX_WAIT_TIMEOUT_MS
#define FLOPPY_FLUX_WAIT_TIMEOUT_MS 250u
#endif

#ifndef FLOPPY_INDEX_TIMEOUT_MS
#define FLOPPY_INDEX_TIMEOUT_MS 500u
#endif

#ifndef FLOPPY_WRITE_FLUX_TIMEOUT_MS
#define FLOPPY_WRITE_FLUX_TIMEOUT_MS 1000u
#endif

// Rest after every step pulse before the next one (TEAC FD-55GFR spec:
// 3 ms step rate; twice that for margin).
#ifndef FLOPPY_STEP_MS
#define FLOPPY_STEP_MS 6u
#endif
// Minimum time from the last step pulse to a pulse in the other direction
// (TEAC FD-55GFR spec: 10 ms).
#ifndef FLOPPY_STEP_REVERSE_MS
#define FLOPPY_STEP_REVERSE_MS 15u
#endif
// DIR setup before /STEP falls, and /STEP low width. No drive figure is
// recorded here: 10 us is generous against the microsecond-scale minimums
// of the Shugart-style interface and the 7407 + pull-up edges.
#define FLOPPY_STEP_PULSE_US 10u
// /DRVSB settle after a select or deselect, before the drive's outputs
// (DSKCHG, TRK00, index) are trusted.
#define FLOPPY_SELECT_SETTLE_MS 10u
// TEAC FD-55GFR: no step, deselect or side change within 590 us after
// /WGATE goes false; the retry paths re-seek right after a write.
#define FLOPPY_WGATE_HOLD_US 1000u

// Idle DSKCHG sample: how long /DRVSB is pulsed before pin 34 is read.
// Long enough for the drive's output gating and the 1k pull-up, far too
// short to light the drive's front LED.
#ifndef FLOPPY_DSKCHG_SAMPLE_US
#define FLOPPY_DSKCHG_SAMPLE_US 50u
#endif

// floppy_quick_media_check: how long DSKCHG may take to release after the
// step pulse. YD-380B bench 2026-11-03: released before the first poll
// (0 us). The full window is spent only on an empty drive, so it bounds
// the LED blink of every empty-drive check.
#define FLOPPY_QUICK_CLEAR_MAX_US 2000u

// Seek-home limit: 80 physical cylinders on a 96 tpi drive plus margin.
#define FLOPPY_SEEK_HOME_STEPS_MAX 90u

// Flux loops check the operation deadline once every 256 transitions: the
// clock read stays off the per-pulse path (256 pulses: 0.5-2 ms of flux).
#define FLOPPY_DEADLINE_POLL_MASK 0xFFu
// Sector writes: read-backs of one pass before concluding (a read that broke
// off says nothing about the sectors it did not reach).
#define FLOPPY_VERIFY_READS 3u
// Sector writes, timing-critical part: TX FIFO preload and the wait for a
// fresh flux word after the ID. Both normally take microseconds; a miss
// means a stuck DMA or a dead read channel.
#define FLOPPY_SECTOR_PRELOAD_TIMEOUT_US 1000u
#define FLOPPY_SECTOR_WORD_TIMEOUT_US 2000u

// Polarity of the /REDWC (density) line: 1 = released selects high density,
// asserted low selects reduced write current for 250/300 kbps media (the
// common PC-AT convention; the YD-380B samples this line, JP13 fitted by
// default). Set to 0 for drives that behave inverted.
#ifndef FLOPPY_DENSITY_HD_RELEASED
#define FLOPPY_DENSITY_HD_RELEASED 1
#endif

typedef struct {
  uint32_t start_ms;
  uint32_t limit_ms;
} floppy_deadline_t;

static floppy_t *active_floppy;

#if FLOPPY_DEBUG
// Overrun forensics: when the current flux session started and how many
// samples it has consumed, to tell a slow consumer from a stall.
static uint32_t dbg_session_us;
static uint32_t dbg_session_samples;
static uint32_t dbg_max_gap_us;
static uint32_t dbg_last_sample_us;
#endif

static uint32_t now_ms(void) {
  return (uint32_t)to_ms_since_boot(get_absolute_time());
}

static bool elapsed(uint32_t start, uint32_t limit) {
  return (uint32_t)(now_ms() - start) >= limit;
}

static uint32_t remaining(uint32_t start, uint32_t limit) {
  uint32_t used = (uint32_t)(now_ms() - start);
  return used >= limit ? 0 : limit - used;
}

static floppy_deadline_t deadline_begin(uint32_t limit_ms) {
  return (floppy_deadline_t){.start_ms = now_ms(), .limit_ms = limit_ms};
}

static uint32_t deadline_remaining(const floppy_deadline_t *deadline) {
  return remaining(deadline->start_ms, deadline->limit_ms);
}

static bool deadline_elapsed(const floppy_deadline_t *deadline) {
  return deadline_remaining(deadline) == 0;
}

static uint32_t next_generation(uint32_t generation) {
  generation++;
  return generation == 0 ? 1 : generation;
}

static bool floppy_ready(const floppy_t *f) {
  return f && f == active_floppy && f->lifecycle == FLOPPY_LIFECYCLE;
}

static uint8_t floppy_step_mul(const floppy_t *f) {
  return f->fmt && f->fmt->double_step ? 2u : 1u;
}

static uint16_t floppy_phys_cylinders(const floppy_t *f) {
  return f->fmt ? (uint16_t)(f->fmt->cylinders * floppy_step_mul(f))
                : DISK_MAX_CYLINDERS;
}

static uint floppy_dma_channel(const floppy_t *f) {
  return (uint)f->dma_ch;
}

static uint floppy_dma_tx_channel(const floppy_t *f) {
  return (uint)f->dma_tx_ch;
}

// Decoder and sector buffer shared by every core-1 flux consumer (reads,
// position check, sector writes): they never run concurrently, and the 1.3
// KB would not fit the core-1 stack comfortably.
static struct {
  mfm_t decoder;
  mfm_sector_t sector;
} scratch;

_Static_assert(FLOPPY_READ_DEADLINE_MS <= FLOPPY_OPERATION_MAX_MS &&
                   FLOPPY_CONTROL_DEADLINE_MS <= FLOPPY_OPERATION_MAX_MS,
               "FLOPPY_OPERATION_MAX_MS must bound every operation deadline");

// Head positioning is defined with the step code further down; the idle
// park and the operation prologue need it earlier.
static disk_err_t floppy_seek_track0_internal(floppy_t *f,
                                              const floppy_deadline_t *deadline,
                                              uint32_t generation, bool *moved);
static disk_err_t floppy_clear_media_latch(floppy_t *f,
                                           const floppy_deadline_t *deadline,
                                           uint32_t generation);

static void floppy_beat(floppy_t *f) {
  if (f->heartbeat) f->heartbeat();
}

static disk_err_t floppy_operation_begin(floppy_t *f, floppy_operation_t operation,
                                         uint32_t limit_ms,
                                         floppy_deadline_t *deadline) {
  if (!floppy_ready(f) || !deadline) return DISK_ERR_INVALID;
  if (f->operation != FLOPPY_OPERATION_IDLE) return DISK_ERR_BUSY;
  floppy_beat(f);
  *deadline = deadline_begin(limit_ms);
  f->operation = operation;
  f->operation_start_ms = deadline->start_ms;
  f->operation_limit_ms = deadline->limit_ms;
  return DISK_OK;
}

static void floppy_operation_end(floppy_t *f, floppy_operation_t operation) {
  if (f->operation == operation) {
    f->operation = FLOPPY_OPERATION_IDLE;
    f->operation_start_ms = 0;
    f->operation_limit_ms = 0;
  }
}

#define FLOPPY_PIN_LIST(p) \
  {(p).index, (p).track0, (p).write_protect, (p).read_data, (p).disk_change, \
   (p).drive_select, (p).motor_enable, (p).direction, (p).step, \
   (p).write_data, (p).write_gate, (p).side_select, (p).density}

static bool floppy_pins_valid(const floppy_pins_t *pins) {
  const uint8_t values[] = FLOPPY_PIN_LIST(*pins);
  for (size_t i = 0; i < sizeof(values); i++) {
    if (values[i] >= NUM_BANK0_GPIOS) return false;
    for (size_t j = 0; j < i; j++) {
      if (values[i] == values[j]) return false;
    }
  }
  return pins->index < 32u && pins->read_data < 32u && pins->write_data < 32u;
}

static void floppy_pin_oc(uint pin, bool high) {
  if (high) {
    gpio_set_dir(pin, GPIO_IN);
  } else {
    gpio_put(pin, 0);
    gpio_set_dir(pin, GPIO_OUT);
  }
}

// Match /REDWC to the media data rate; with no format selected yet, park the
// line in HD position (a 1.2MB drive's native mode).
static void floppy_apply_density(floppy_t *f) {
  bool hd = !f->fmt || f->fmt->data_rate_kbps >= 500;
  floppy_pin_oc(f->pins.density, FLOPPY_DENSITY_HD_RELEASED ? hd : !hd);
}

// Pin 26 is active low: the head sits on physical track 0.
static bool floppy_at_track0(const floppy_t *f) {
  return !gpio_get(f->pins.track0);
}

static void floppy_update_media(floppy_t *f) {
  if (!f->selected) return;
  bool active = !gpio_get(f->pins.disk_change);
  bool initial = !f->media_observed;
  f->media_observed = true;
  if (active && !f->disk_change_active) {
    f->disk_change_active = true;
    f->track0_confirmed = false;
    f->motor_qualified = false;
    if (initial) return;
    f->media_generation = next_generation(f->media_generation);
    f->stats.media_changes++;
  } else if (!active) {
    f->disk_change_active = false;
  }
}

static disk_err_t floppy_flux_media_status(floppy_t *f, uint32_t generation) {
  if (gpio_get(f->pins.disk_change)) return DISK_OK;
  floppy_update_media(f);
  return f->media_generation == generation && !f->disk_change_active
             ? DISK_OK
             : DISK_ERR_MEDIA_CHANGED;
}

static void floppy_stop(floppy_t *f) {
  if (f->motor_on || f->selected) FTRACE("idle stop: motor off, deselect\n");
  floppy_pin_oc(f->pins.write_gate, true); // never left asserted, whatever happened
  floppy_pin_oc(f->pins.motor_enable, true);
  f->motor_on = false;
  f->motor_qualified = false;
  floppy_pin_oc(f->pins.drive_select, true);
  f->selected = false;
}

// Idle stop with a disk in: bring the head home first, so that when the
// disk is pulled the empty-drive probes find it at TRK00 and clear DSKCHG
// without moving it (floppy_quick_media_check, floppy_clear_media_latch).
// The next access to the same disk seeks back from a known position; a new
// disk always recalibrates (0 steps from TRK00). A failed park is only
// traced: the next quick check then sees the head away from TRK00 and a
// full probe homes it.
static void floppy_park(floppy_t *f) {
  if (!f->selected || f->disk_change_active || floppy_at_track0(f)) return;
  floppy_deadline_t deadline;
  if (floppy_operation_begin(f, FLOPPY_OPERATION_CONTROL, FLOPPY_CONTROL_DEADLINE_MS,
                             &deadline) != DISK_OK) {
    return;
  }
  FTRACE("idle: parking the head at track 0\n");
  bool moved = false;
  disk_err_t status = floppy_seek_track0_internal(f, &deadline, f->media_generation, &moved);
  if (status != DISK_OK) FTRACE("park failed: %s\n", disk_err_name(status));
  (void)status; // traced in the debug build only
  floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
}

disk_err_t floppy_poll(floppy_t *f) {
  if (!floppy_ready(f)) return DISK_ERR_INVALID;
  if (f->motor_on && f->operation == FLOPPY_OPERATION_IDLE &&
      elapsed(f->last_io_time_ms, FLOPPY_IDLE_TIMEOUT_MS)) {
    floppy_park(f);
    floppy_stop(f);
  }
  return DISK_OK;
}

static void floppy_touch(floppy_t *f) {
  f->last_io_time_ms = now_ms();
}

static disk_err_t floppy_wait_index_falling(floppy_t *f,
                                            const floppy_deadline_t *deadline,
                                            uint32_t timeout,
                                            uint32_t generation) {
  uint32_t start = now_ms();
  while (!gpio_get(f->pins.index)) {
    floppy_update_media(f);
    if (f->media_generation != generation) return DISK_ERR_MEDIA_CHANGED;
    if (elapsed(start, timeout) || deadline_elapsed(deadline)) return DISK_ERR_TIMEOUT;
    tight_loop_contents();
  }
  while (gpio_get(f->pins.index)) {
    floppy_update_media(f);
    if (f->media_generation != generation) return DISK_ERR_MEDIA_CHANGED;
    if (elapsed(start, timeout) || deadline_elapsed(deadline)) return DISK_ERR_TIMEOUT;
    tight_loop_contents();
  }
  return DISK_OK;
}

static disk_err_t floppy_wait_stable_index(floppy_t *f,
                                           const floppy_deadline_t *deadline,
                                           uint32_t generation) {
  uint32_t start = now_ms();
  uint32_t previous = 0;
  uint32_t previous_period = 0;
  unsigned stable = 0;
  unsigned edges = 0;
  for (;;) {
    uint32_t left = remaining(start, FLOPPY_MOTOR_DEADLINE_MS);
    uint32_t operation_left = deadline_remaining(deadline);
    if (left > operation_left) left = operation_left;
    if (left == 0) break;
    disk_err_t status = floppy_wait_index_falling(f, deadline, left, generation);
    if (status != DISK_OK) {
      FTRACE("index wait: %u pulse(s) in %lu ms, last period %lu ms "
             "(need 3 in %u-%u ms) -> %s\n",
             edges, (unsigned long)(now_ms() - start),
             (unsigned long)previous_period, FLOPPY_INDEX_PERIOD_MIN_MS,
             FLOPPY_INDEX_PERIOD_MAX_MS, disk_err_name(status));
      return status;
    }
    edges++;
    uint32_t now = now_ms();
    if (previous != 0 || now == 0) {
      uint32_t period = now - previous;
      bool range = period >= FLOPPY_INDEX_PERIOD_MIN_MS &&
                   period <= FLOPPY_INDEX_PERIOD_MAX_MS;
      uint32_t difference = period > previous_period ? period - previous_period
                                                     : previous_period - period;
      bool consistent = previous_period == 0 ||
                        difference <= FLOPPY_INDEX_PERIOD_SLOP_MS;
      stable = range && consistent ? stable + 1u : 0u;
      previous_period = period;
      if (stable >= 2u) {
        FTRACE("index OK: period %lu ms (%lu rpm)\n", (unsigned long)period,
               period ? (unsigned long)(60000u / period) : 0ul);
        return DISK_OK;
      }
    }
    previous = now;
  }
  FTRACE("index wait: %u pulse(s) in %u ms, last period %lu ms "
         "(need 3 in %u-%u ms) -> TIMEOUT\n",
         edges, FLOPPY_MOTOR_DEADLINE_MS, (unsigned long)previous_period,
         FLOPPY_INDEX_PERIOD_MIN_MS, FLOPPY_INDEX_PERIOD_MAX_MS);
  (void)edges;
  return DISK_ERR_TIMEOUT;
}

static disk_err_t floppy_select_internal(floppy_t *f, bool on,
                                         const floppy_deadline_t *deadline) {
  if (f->selected == on) return DISK_OK;
  floppy_pin_oc(f->pins.drive_select, !on);
  f->selected = on;
  FTRACE("select %s (pin 12 %s)\n", on ? "ON" : "off", on ? "low" : "released");
  sleep_ms(FLOPPY_SELECT_SETTLE_MS);
  floppy_touch(f);
  return deadline_elapsed(deadline) ? DISK_ERR_TIMEOUT : DISK_OK;
}

static void floppy_motor_line_on(floppy_t *f) {
  if (!f->motor_on) {
    FTRACE("motor ON (pin 16 low)\n");
    floppy_pin_oc(f->pins.motor_enable, false);
    f->motor_on = true;
    f->motor_qualified = false;
  }
  floppy_touch(f);
}

static disk_err_t floppy_motor_off_internal(floppy_t *f) {
  if (!f->motor_on) return DISK_OK;
  FTRACE("motor off\n");
  floppy_pin_oc(f->pins.motor_enable, true);
  f->motor_on = false;
  f->motor_qualified = false;
  floppy_touch(f);
  return DISK_OK;
}

static disk_err_t floppy_qualify_motor(floppy_t *f,
                                       const floppy_deadline_t *deadline,
                                       uint32_t generation) {
  if (f->motor_qualified && f->motor_generation == generation) return DISK_OK;
  disk_err_t status = floppy_wait_stable_index(f, deadline, generation);
  if (status != DISK_OK) return status;
  floppy_update_media(f);
  if (f->media_generation != generation || f->disk_change_active) {
    return DISK_ERR_MEDIA_CHANGED;
  }
  f->motor_generation = generation;
  f->motor_qualified = true;
  return DISK_OK;
}

static disk_err_t floppy_prepare(floppy_t *f, uint32_t expected_generation,
                                 const floppy_deadline_t *deadline) {
  floppy_update_media(f);
  if (f->media_generation != expected_generation) return DISK_ERR_MEDIA_CHANGED;
  floppy_touch(f);
  disk_err_t status = floppy_select_internal(f, true, deadline);
  if (status != DISK_OK) return status;
  // Latch first: a step while selected clears DSKCHG with the spindle
  // stopped, so probing an empty drive never spins the motor.
  status = floppy_clear_media_latch(f, deadline, expected_generation);
  if (status != DISK_OK) return status;
  floppy_motor_line_on(f);
  return floppy_qualify_motor(f, deadline, expected_generation);
}

static inline uint32_t flux_ring_produced(floppy_t *f) {
  return FLOPPY_FLUX_DMA_COUNT -
         dma_channel_hw_addr(floppy_dma_channel(f))->transfer_count;
}

static uint32_t pio_rx_stall_mask(const floppy_t *f) {
  return 1u << (PIO_FDEBUG_RXSTALL_LSB + f->read.sm);
}

static uint32_t pio_tx_stall_mask(const floppy_t *f) {
  return 1u << (PIO_FDEBUG_TXSTALL_LSB + f->write.sm);
}

static void pio_clear_stall(PIO pio, uint32_t mask) {
  pio->fdebug = mask;
}

static bool pio_stalled(PIO pio, uint32_t mask) {
  return (pio->fdebug & mask) != 0;
}

static inline bool flux_data_available(floppy_t *f) {
  return f->read.half_valid || flux_ring_produced(f) != f->ring_cpu;
}

static uint16_t flux_read_sample(floppy_t *f) {
  if (f->read.half_valid) {
    f->read.half_valid = false;
    return f->read.half;
  }
  uint32_t produced = flux_ring_produced(f);
  uint32_t lag = produced - f->ring_cpu;
  if (lag > FLOPPY_FLUX_RING_WORDS) {
    f->stats.overruns++;
    f->read_overrun = true;
#if FLOPPY_DEBUG
    uint32_t elapsed_us = time_us_32() - dbg_session_us;
    FTRACE("overrun: lag %lu words, op %d, cyl %u, after %lu us: produced %lu "
           "consumed %lu words, max gap %lu us\n",
           (unsigned long)lag, (int)f->operation, f->cylinder,
           (unsigned long)elapsed_us, (unsigned long)produced,
           (unsigned long)dbg_session_samples, (unsigned long)dbg_max_gap_us);
#endif
    f->ring_cpu = produced - FLOPPY_FLUX_RING_WORDS;
    lag = FLOPPY_FLUX_RING_WORDS;
  }
  if (lag > f->stats.ring_peak) f->stats.ring_peak = lag;
  f->stats.flux_words++;
#if FLOPPY_DEBUG
  uint32_t now_us = time_us_32();
  if (dbg_session_samples && now_us - dbg_last_sample_us > dbg_max_gap_us) {
    dbg_max_gap_us = now_us - dbg_last_sample_us;
  }
  dbg_last_sample_us = now_us;
  dbg_session_samples++;
#endif
  uint32_t packed = f->flux_ring[f->ring_cpu & (FLOPPY_FLUX_RING_WORDS - 1u)];
  f->ring_cpu++;
  f->read.half = (uint16_t)(packed >> 16u);
  f->read.half_valid = true;
  return (uint16_t)(packed & 0xFFFFu);
}

static disk_err_t flux_read_wait(floppy_t *f, const floppy_deadline_t *deadline,
                                 uint32_t generation, uint16_t *out) {
  if (!flux_data_available(f)) {
    uint32_t start = now_ms();
    while (!flux_data_available(f)) {
      disk_err_t status = floppy_flux_media_status(f, generation);
      if (status != DISK_OK) return status;
      if (pio_stalled(f->read.pio, pio_rx_stall_mask(f))) {
        f->stats.overruns++;
        FTRACE("overrun: RX FIFO stall while waiting, op %d, cyl %u\n",
               (int)f->operation, f->cylinder);
        return DISK_ERR_OVERRUN;
      }
      if (f->read_overrun) return DISK_ERR_OVERRUN;
      if (elapsed(start, FLOPPY_FLUX_WAIT_TIMEOUT_MS) || deadline_elapsed(deadline)) {
        return DISK_ERR_TIMEOUT;
      }
      tight_loop_contents();
    }
  }
  *out = flux_read_sample(f);
  if (pio_stalled(f->read.pio, pio_rx_stall_mask(f))) {
    f->stats.overruns++;
    FTRACE("overrun: RX FIFO stall after sample, op %d, cyl %u\n",
           (int)f->operation, f->cylinder);
    return DISK_ERR_OVERRUN;
  }
  disk_err_t status = floppy_flux_media_status(f, generation);
  if (status != DISK_OK) return status;
  return f->read_overrun ? DISK_ERR_OVERRUN : DISK_OK;
}

static void floppy_flux_read_start(floppy_t *f) {
  pio_sm_set_enabled(f->read.pio, f->read.sm, false);
  dma_channel_abort(floppy_dma_channel(f));
  pio_sm_clear_fifos(f->read.pio, f->read.sm);
  pio_sm_restart(f->read.pio, f->read.sm);
  f->read.half_valid = false;
  f->read.primed = false;
  f->read_overrun = false;
  f->ring_cpu = 0;
#if FLOPPY_DEBUG
  dbg_session_us = time_us_32();
  dbg_session_samples = 0;
  dbg_max_gap_us = 0;
#endif
  pio_sm_exec(f->read.pio, f->read.sm, pio_encode_set(pio_x, 0));
  pio_sm_exec(f->read.pio, f->read.sm, pio_encode_jmp(f->read.offset));
  pio_clear_stall(f->read.pio, pio_rx_stall_mask(f));
  dma_channel_config config = dma_channel_get_default_config(floppy_dma_channel(f));
  channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
  channel_config_set_read_increment(&config, false);
  channel_config_set_write_increment(&config, true);
  channel_config_set_ring(&config, true, FLOPPY_FLUX_RING_BITS);
  channel_config_set_dreq(&config, pio_get_dreq(f->read.pio, f->read.sm, false));
  channel_config_set_high_priority(&config, true);
  dma_channel_configure(floppy_dma_channel(f), &config, f->flux_ring,
                        &f->read.pio->rxf[f->read.sm], FLOPPY_FLUX_DMA_COUNT, true);
  pio_sm_set_enabled(f->read.pio, f->read.sm, true);
}

static void floppy_flux_read_stop(floppy_t *f) {
  pio_sm_set_enabled(f->read.pio, f->read.sm, false);
  dma_channel_abort(floppy_dma_channel(f));
  f->flux_active = false;
}

static disk_err_t floppy_flux_session_start(floppy_t *f, uint32_t expected_generation) {
  if (f->flux_active) return DISK_ERR_INVALID;
  floppy_update_media(f);
  if (f->media_generation != expected_generation || f->disk_change_active) {
    return DISK_ERR_MEDIA_CHANGED;
  }
  floppy_flux_read_start(f);
  f->flux_generation = expected_generation;
  f->flux_active = true;
  return DISK_OK;
}

disk_err_t floppy_flux_begin(floppy_t *f, uint32_t expected_generation) {
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_RAW, FLOPPY_READ_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  status = floppy_prepare(f, expected_generation, &deadline);
  if (status == DISK_OK) status = floppy_flux_session_start(f, expected_generation);
  if (status != DISK_OK) floppy_operation_end(f, FLOPPY_OPERATION_RAW);
  return status;
}

static floppy_pulse_t flux_pulse(floppy_t *f, uint16_t value) {
  uint16_t count = value >> 1;
  int difference = f->read.prev - count;
  if (difference < 0) difference += 0x8000;
  f->read.prev = count;
  return (floppy_pulse_t){.delta = (uint16_t)difference, .index = (value & 1u) != 0};
}

static disk_err_t floppy_flux_next_internal(floppy_t *f,
                                            const floppy_deadline_t *deadline,
                                            uint32_t generation,
                                            floppy_pulse_t *pulse) {
  uint16_t value;
  if (!f->read.primed) {
    disk_err_t status = flux_read_wait(f, deadline, generation, &value);
    if (status != DISK_OK) return status;
    f->read.prev = value >> 1;
    f->read.primed = true;
  }
  disk_err_t status = flux_read_wait(f, deadline, generation, &value);
  if (status != DISK_OK) return status;
  *pulse = flux_pulse(f, value);
  return DISK_OK;
}

disk_result_t floppy_flux_read(floppy_t *f, floppy_pulse_t *pulses, size_t capacity) {
  disk_result_t result = {.error = DISK_OK, .count = 0};
  if (!floppy_ready(f) || f->operation != FLOPPY_OPERATION_RAW || !f->flux_active ||
      !pulses || capacity == 0) {
    result.error = DISK_ERR_INVALID;
    return result;
  }
  floppy_deadline_t deadline = {
      .start_ms = f->operation_start_ms,
      .limit_ms = f->operation_limit_ms,
  };
  result.error = floppy_flux_next_internal(f, &deadline, f->flux_generation, pulses);
  if (result.error != DISK_OK) return result;
  result.count = 1;
  while (result.count < capacity && flux_data_available(f)) {
    uint16_t value = flux_read_sample(f);
    if (f->read_overrun) {
      result.error = DISK_ERR_OVERRUN;
      return result;
    }
    pulses[result.count++] = flux_pulse(f, value);
  }
  if (result.count == 1) return result;
  if (pio_stalled(f->read.pio, pio_rx_stall_mask(f))) {
    f->stats.overruns++;
    result.error = DISK_ERR_OVERRUN;
    return result;
  }
  result.error = floppy_flux_media_status(f, f->flux_generation);
  return result;
}

disk_err_t floppy_flux_end(floppy_t *f) {
  if (!floppy_ready(f) || f->operation != FLOPPY_OPERATION_RAW || !f->flux_active) {
    return DISK_ERR_INVALID;
  }
  floppy_flux_read_stop(f);
  floppy_operation_end(f, FLOPPY_OPERATION_RAW);
  return DISK_OK;
}

static disk_err_t floppy_side_select_internal(floppy_t *f, uint8_t head,
                                              const floppy_deadline_t *deadline) {
  if (head >= DISK_HEADS) return DISK_ERR_INVALID;
  bool changed = !f->head_confirmed || f->head != head;
  floppy_pin_oc(f->pins.side_select, head == 0);
  f->head = head;
  f->head_confirmed = true;
  if (changed) sleep_ms(FLOPPY_HEAD_SETTLE_MS);
  floppy_touch(f);
  return deadline_elapsed(deadline) ? DISK_ERR_TIMEOUT : DISK_OK;
}

// One /STEP pulse: DIR set up, /STEP low, released, and stamped for the
// reversal rest. `busy` spins through the two FLOPPY_STEP_PULSE_US waits
// instead of sleeping, for floppy_quick_media_check, whose whole select
// window is timed in microseconds. No rest after the pulse: the caller owns
// it (or, at TRK00, needs none: the head does not move).
static void floppy_step_pulse(floppy_t *f, int direction, bool busy) {
  floppy_pin_oc(f->pins.direction, direction != DIR_INWARD);
  if (busy) busy_wait_us(FLOPPY_STEP_PULSE_US);
  else sleep_us(FLOPPY_STEP_PULSE_US);
  floppy_pin_oc(f->pins.step, false);
  if (busy) busy_wait_us(FLOPPY_STEP_PULSE_US);
  else sleep_us(FLOPPY_STEP_PULSE_US);
  floppy_pin_oc(f->pins.step, true);
  f->last_step_dir = (int8_t)direction;
  f->last_step_ms = now_ms();
}

static disk_err_t floppy_step_internal(floppy_t *f, int direction,
                                       const floppy_deadline_t *deadline,
                                       uint32_t generation) {
  if (deadline_elapsed(deadline)) return DISK_ERR_TIMEOUT;
  // A direction reversal needs more rest than a same-direction step (TEAC
  // FD-55GFR: 3 ms between steps, 10 ms when the direction changes).
  if (f->last_step_dir >= 0 && f->last_step_dir != direction) {
    uint32_t since = now_ms() - f->last_step_ms;
    if (since < FLOPPY_STEP_REVERSE_MS) sleep_ms(FLOPPY_STEP_REVERSE_MS - since);
  }
  floppy_step_pulse(f, direction, false);
  sleep_ms(FLOPPY_STEP_MS);
  if (direction == DIR_INWARD && f->cylinder + 1u < floppy_phys_cylinders(f)) {
    f->cylinder++;
  } else if (direction == DIR_OUTWARD && f->cylinder > 0) {
    f->cylinder--;
  }
  floppy_update_media(f);
  if (f->media_generation != generation) return DISK_ERR_MEDIA_CHANGED;
  return deadline_elapsed(deadline) ? DISK_ERR_TIMEOUT : DISK_OK;
}

static disk_err_t floppy_clear_media_latch(floppy_t *f,
                                           const floppy_deadline_t *deadline,
                                           uint32_t generation) {
  floppy_update_media(f);
  if (!f->disk_change_active) {
    FTRACE("DSKCHG (pin 34) inactive\n");
    return DISK_OK;
  }
  // Any step pulse while selected clears the latch when a disk is in. At
  // TRK00 an outward pulse does it without moving the head: the drive
  // ignores steps out past track 0 (YD-380B on the bench 2026-11-03: no
  // movement, TRK00 held, DSKCHG cleared with a disk and stayed set when
  // empty). The head is parked there at every idle stop (floppy_poll), so
  // probing an empty drive is silent; only an unknown position (boot, or a
  // swap mid-operation) costs one seek out first.
  disk_err_t status;
  if (!floppy_at_track0(f)) {
    FTRACE("DSKCHG (pin 34) ACTIVE: head not at TRK00, seeking out first\n");
    bool moved = false;
    status = floppy_seek_track0_internal(f, deadline, generation, &moved);
    if (status != DISK_OK) return status;
  }
  FTRACE("DSKCHG (pin 34) ACTIVE: step pulse out at TRK00 (head stays put)\n");
  status = floppy_step_internal(f, DIR_OUTWARD, deadline, generation);
  if (status != DISK_OK) return status;
  sleep_ms(FLOPPY_HEAD_SETTLE_MS);
  if (deadline_elapsed(deadline)) return DISK_ERR_TIMEOUT;
  floppy_update_media(f);
  if (f->media_generation != generation || f->disk_change_active) {
    FTRACE("DSKCHG still active after the step pulse: no disk (or door open)\n");
    return DISK_ERR_MEDIA_CHANGED;
  }
  FTRACE("DSKCHG cleared: a disk is in\n");
  f->track0_confirmed = false;
  return DISK_OK;
}

static disk_err_t floppy_seek_track0_internal(floppy_t *f,
                                              const floppy_deadline_t *deadline,
                                              uint32_t generation, bool *moved) {
  f->track0_confirmed = false;
  for (unsigned step = 0; step < FLOPPY_SEEK_HOME_STEPS_MAX; step++) {
    if (floppy_at_track0(f)) {
      FTRACE("TRK00 (pin 26) seen after %u step(s) out\n", step);
      f->cylinder = 0;
      f->track0_confirmed = true;
      return DISK_OK;
    }
    disk_err_t status = floppy_step_internal(f, DIR_OUTWARD, deadline, generation);
    if (status != DISK_OK) return status;
    *moved = true;
  }
  FTRACE("TRK00 (pin 26) never seen after %u steps out\n", FLOPPY_SEEK_HOME_STEPS_MAX);
  return DISK_ERR_NO_TRACK0;
}

// target is a LOGICAL cylinder; physical position accounts for double-step.
static disk_err_t floppy_seek_internal(floppy_t *f, uint8_t target,
                                       const floppy_deadline_t *deadline,
                                       uint32_t generation) {
  if (!f->fmt || target >= f->fmt->cylinders) return DISK_ERR_WRONG_TRACK;
  uint16_t phys_target = (uint16_t)target * floppy_step_mul(f);
  bool moved = false;
  if (!f->track0_confirmed) {
    disk_err_t status = floppy_seek_track0_internal(f, deadline, generation, &moved);
    if (status != DISK_OK) return status;
  }
  while (f->cylinder < phys_target) {
    disk_err_t status = floppy_step_internal(f, DIR_INWARD, deadline, generation);
    if (status != DISK_OK) return status;
    moved = true;
  }
  while (f->cylinder > phys_target) {
    disk_err_t status = floppy_step_internal(f, DIR_OUTWARD, deadline, generation);
    if (status != DISK_OK) return status;
    moved = true;
  }
  if (moved) sleep_ms(FLOPPY_HEAD_SETTLE_MS);
  floppy_touch(f);
  return deadline_elapsed(deadline) ? DISK_ERR_TIMEOUT : DISK_OK;
}

disk_err_t floppy_set_format(floppy_t *f, const floppy_format_t *fmt) {
  if (!floppy_ready(f) || !fmt || fmt->heads > DISK_HEADS ||
      fmt->sectors_per_track > DISK_MAX_SPT ||
      (uint16_t)(fmt->cylinders * (fmt->double_step ? 2u : 1u)) >
          DISK_MAX_CYLINDERS) {
    return DISK_ERR_INVALID;
  }
  if (f->operation != FLOPPY_OPERATION_IDLE) return DISK_ERR_BUSY;
  if (f->fmt != fmt) {
    f->fmt = fmt;
    f->track0_confirmed = false;
  }
  floppy_apply_density(f);
  return DISK_OK;
}

disk_err_t floppy_select(floppy_t *f, bool on) {
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_CONTROL, FLOPPY_CONTROL_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  status = floppy_select_internal(f, on, &deadline);
  floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
  return status;
}

disk_err_t floppy_motor_on(floppy_t *f) {
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_CONTROL, FLOPPY_CONTROL_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  status = floppy_select_internal(f, true, &deadline);
  if (status == DISK_OK) {
    floppy_update_media(f);
    uint32_t generation = f->media_generation;
    floppy_motor_line_on(f);
    status = floppy_wait_stable_index(f, &deadline, generation);
    if (status == DISK_OK && !f->disk_change_active) {
      f->motor_generation = generation;
      f->motor_qualified = true;
    }
  }
  if (status != DISK_OK) floppy_motor_off_internal(f);
  floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
  return status;
}

disk_err_t floppy_motor_off(floppy_t *f) {
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_CONTROL, FLOPPY_CONTROL_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  status = floppy_motor_off_internal(f);
  floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
  return status;
}

disk_err_t floppy_disk_changed(floppy_t *f, bool *changed) {
  if (!floppy_ready(f) || !changed) return DISK_ERR_INVALID;
  floppy_update_media(f);
  *changed = f->disk_change_active;
  return DISK_OK;
}

// Brief idle select (DSKCHG sample, quick media check): /DRVSB asserted
// for microseconds, busy-waited so the window stays far below what lights
// the front LED, with none of floppy_select_internal's settle or bookkeeping.
// The end samples DSKCHG one last time while still selected.
static void floppy_brief_select_begin(floppy_t *f) {
  floppy_pin_oc(f->pins.drive_select, false);
  busy_wait_us(FLOPPY_DSKCHG_SAMPLE_US);
  f->selected = true;
}

static void floppy_brief_select_end(floppy_t *f) {
  floppy_update_media(f);
  f->selected = false;
  floppy_pin_oc(f->pins.drive_select, true);
}

// The drive only drives pin 34 while selected, so an idle (deselected)
// drive would hide a swap until the next host request. Pulse /DRVSB just
// long enough to sample DSKCHG; an active line bumps the media generation
// exactly as during an operation. The motor line is left alone.
disk_err_t floppy_sample_disk_change(floppy_t *f) {
  if (!floppy_ready(f)) return DISK_ERR_INVALID;
  if (f->operation != FLOPPY_OPERATION_IDLE) return DISK_ERR_BUSY;
  if (f->selected) {
    floppy_update_media(f);
    return DISK_OK;
  }
  floppy_brief_select_begin(f);
  floppy_brief_select_end(f);
  return DISK_OK;
}

disk_err_t floppy_quick_media_check(floppy_t *f, bool *disk_in) {
  if (!floppy_ready(f) || !disk_in) return DISK_ERR_INVALID;
  *disk_in = false;
  if (f->operation != FLOPPY_OPERATION_IDLE || f->selected || f->motor_on) {
    return DISK_ERR_BUSY;
  }
  floppy_brief_select_begin(f);
  // Record a set latch before the pulse can clear it: every swap must bump
  // the media generation, even one the probe never saw (latch clear before).
  floppy_update_media(f);
  bool at_track0 = floppy_at_track0(f);
  bool latched = f->disk_change_active;
  int32_t clear_us = -1;
  if (at_track0 && latched) {
    // Busy, no rest: the head does not move at TRK00 (floppy_clear_media_latch).
    floppy_step_pulse(f, DIR_OUTWARD, true);
    uint32_t start = time_us_32();
    while (time_us_32() - start < FLOPPY_QUICK_CLEAR_MAX_US) {
      if (gpio_get(f->pins.disk_change)) {
        clear_us = (int32_t)(time_us_32() - start);
        break;
      }
    }
  }
  floppy_brief_select_end(f);
  // Trace only once deselected: UART time must not stretch the LED blink.
  if (!at_track0) {
    FTRACE("quick check: head not at TRK00, full probe needed\n");
    return DISK_ERR_NO_TRACK0;
  }
  if (latched && clear_us >= 0) {
    FTRACE("quick check: DSKCHG cleared %ld us after the pulse: a disk is in\n",
           (long)clear_us);
  }
  *disk_in = !f->disk_change_active;
  return DISK_OK;
}

disk_err_t floppy_media_generation(floppy_t *f, uint32_t *generation) {
  if (!floppy_ready(f) || !generation) return DISK_ERR_INVALID;
  floppy_update_media(f);
  *generation = f->media_generation;
  return DISK_OK;
}

static bool floppy_write_protected_raw(const floppy_t *f) {
  return !gpio_get(f->pins.write_protect);
}

disk_err_t floppy_write_protected(const floppy_t *f, bool *write_protected) {
  if (!floppy_ready(f) || !write_protected) return DISK_ERR_INVALID;
  *write_protected = floppy_write_protected_raw(f);
  return DISK_OK;
}

disk_err_t floppy_stats(const floppy_t *f, floppy_stats_t *stats) {
  if (!floppy_ready(f) || !stats) return DISK_ERR_INVALID;
  if (f->operation != FLOPPY_OPERATION_IDLE) return DISK_ERR_BUSY;
  *stats = f->stats;
  return DISK_OK;
}

static bool floppy_status_retryable(disk_err_t status) {
  return status == DISK_ERR_TIMEOUT || status == DISK_ERR_CRC ||
         status == DISK_ERR_OVERRUN || status == DISK_ERR_UNDERRUN ||
         status == DISK_ERR_VERIFY || status == DISK_ERR_WRONG_TRACK;
}

static void floppy_stats_failure(floppy_t *f, disk_err_t status) {
  if (status == DISK_OK) return;
  f->stats.failed++;
  if (status == DISK_ERR_TIMEOUT) f->stats.timeout++;
  else if (status == DISK_ERR_CRC) f->stats.crc++;
  else if (status == DISK_ERR_WRONG_TRACK) f->stats.wrong_track++;
  else if (status == DISK_ERR_WRONG_SIDE) f->stats.wrong_side++;
}

static disk_err_t floppy_jog(floppy_t *f, uint8_t cylinder, uint8_t distance,
                             const floppy_deadline_t *deadline,
                             uint32_t generation) {
  floppy_touch(f);
  uint8_t limit = (uint8_t)(f->fmt->cylinders - 1u);
  uint8_t away = cylinder <= distance ? (uint8_t)(cylinder + distance)
                                      : (uint8_t)(cylinder - distance);
  if (away > limit) away = limit;
  disk_err_t status = floppy_seek_internal(f, away, deadline, generation);
  if (status != DISK_OK) return status;
  return floppy_seek_internal(f, cylinder, deadline, generation);
}

typedef struct {
  const floppy_format_t *fmt;
  track_t *track;
  const track_t *expected;
  uint32_t expected_mask; // verify: sectors of `expected` to compare
  uint32_t required;      // verify: sectors that must be seen
  uint32_t mismatch;      // verify: compared sectors that differed
  uint8_t cylinder;
  uint8_t head;
  bool probe; // format probing: no recovery when this data rate shows no MFM
  uint32_t seen;
  uint32_t conflict;
  bool verify_failed;
  bool correct_ch;
  bool wrong_track;
  bool wrong_side;
  bool no_sync; // a full revolution went by without a single sync mark
} read_goal_t;

static void read_goal_accept(read_goal_t *goal, const mfm_sector_t *sector) {
  if (sector->cylinder != goal->cylinder) {
    goal->wrong_track = true;
    return;
  }
  if (sector->head != goal->head) {
    goal->wrong_side = true;
    return;
  }
  goal->correct_ch = true;
  if (!disk_sector_valid(goal->fmt, sector->sector) ||
      sector->sector >= DISK_MAX_SPT) {
    return;
  }
  uint32_t bit = 1u << sector->sector;
  if (goal->expected) {
    // Verify: every copy of a compared sector must match what was written.
    if ((goal->expected_mask & bit) &&
        memcmp(goal->expected->data[sector->sector], sector->data,
               DISK_SECTOR_SIZE) != 0) {
      goal->mismatch |= bit;
      goal->verify_failed = true;
    }
    goal->seen |= bit;
    return;
  }
  if (goal->seen & bit) {
    goal->conflict |= bit;
    goal->track->valid &= ~bit;
    return;
  }
  goal->seen |= bit;
  if (goal->conflict & bit) return;
  if (track_has(goal->track, sector->sector)) {
    if (memcmp(goal->track->data[sector->sector], sector->data,
               DISK_SECTOR_SIZE) != 0) {
      goal->track->valid &= ~bit;
      goal->conflict |= bit;
    }
  } else {
    memcpy(goal->track->data[sector->sector], sector->data, DISK_SECTOR_SIZE);
    track_mark(goal->track, sector->sector);
  }
}

static bool read_goal_complete(const read_goal_t *goal) {
  uint32_t mask = track_valid_mask(goal->fmt);
  return goal->expected ? (goal->seen & goal->required) == goal->required
                        : goal->track->valid == mask;
}

static disk_err_t read_goal_verdict(const read_goal_t *goal, const mfm_t *decoder) {
  if (goal->conflict) return DISK_ERR_CORRUPT;
  if (goal->verify_failed) return DISK_ERR_VERIFY;
  if (decoder->crc_errors > 0) return DISK_ERR_CRC;
  if (decoder->format_errors > 0) return DISK_ERR_CORRUPT;
  if (goal->expected && goal->correct_ch) return DISK_ERR_VERIFY;
  if (!goal->correct_ch && goal->wrong_track) return DISK_ERR_WRONG_TRACK;
  if (!goal->correct_ch && goal->wrong_side) return DISK_ERR_WRONG_SIDE;
  return DISK_ERR_TIMEOUT;
}

// Revolution boundary in a flux stream: the first pulse after the index
// window closes. *previous_index starts false, so the first pulse of a
// session is never one.
static inline bool flux_index_boundary(bool *previous_index, bool index) {
  bool boundary = *previous_index && !index;
  *previous_index = index;
  return boundary;
}

// Decoding starts as soon as the head is on the track, with no wait for
// the index, and stops once every sector is in: about one revolution per
// read or verify instead of up to two, so less media passes under the head.
static disk_err_t floppy_read_flux(floppy_t *f, uint32_t expected_generation,
                                   read_goal_t *goal,
                                   const floppy_deadline_t *deadline) {
  floppy_update_media(f);
  if (f->media_generation != expected_generation) return DISK_ERR_MEDIA_CHANGED;
  disk_err_t status = floppy_prepare(f, expected_generation, deadline);
  if (status != DISK_OK) return status;
  if (f->media_generation != expected_generation) return DISK_ERR_MEDIA_CHANGED;
  status = floppy_seek_internal(f, goal->cylinder, deadline, expected_generation);
  if (status != DISK_OK) return status;
  status = floppy_side_select_internal(f, goal->head, deadline);
  if (status != DISK_OK) return status;
  status = floppy_flux_session_start(f, expected_generation);
  if (status != DISK_OK) return status;

  mfm_t *decoder = &scratch.decoder;
  mfm_sector_t *sector = &scratch.sector;
  mfm_init(decoder, f->fmt);
  goal->seen = 0;
  goal->mismatch = 0;
  goal->verify_failed = false;
  goal->no_sync = false;
  uint32_t transitions = 0;
  unsigned boundaries = 0;
  bool previous_index = false;
  disk_err_t result = DISK_ERR_TIMEOUT;

  // The first index closes a partial revolution; up to
  // FLOPPY_READ_REVOLUTIONS full ones follow.
  while (transitions < FLOPPY_READ_TRANSITIONS_MAX &&
         boundaries <= FLOPPY_READ_REVOLUTIONS) {
    if ((transitions & FLOPPY_DEADLINE_POLL_MASK) == 0 && deadline_elapsed(deadline)) break;
    floppy_pulse_t pulse;
    status = floppy_flux_next_internal(f, deadline, expected_generation, &pulse);
    if (status != DISK_OK) {
      result = status;
      break;
    }
    transitions++;
    if (flux_index_boundary(&previous_index, pulse.index)) {
      boundaries++;
      // A full revolution without one sync mark: no MFM at this data rate
      // (wrong density, unformatted track); more turns cannot change that.
      if (boundaries >= 2 && decoder->syncs_found == 0) {
        goal->no_sync = true;
        break;
      }
      // Reads: a sector ID seen twice within one revolution is a duplicate.
      if (!goal->expected) goal->seen = 0;
      mfm_reset(decoder);
      continue;
    }
    if (!mfm_feed(decoder, pulse.delta, sector)) continue;
    read_goal_accept(goal, sector);
    if (goal->conflict) {
      result = DISK_ERR_CORRUPT;
      break;
    }
    if (read_goal_complete(goal)) {
      // Verify keeps going after a mismatch so the caller learns which
      // sectors came back right; the verdict still says VERIFY.
      result = goal->verify_failed ? DISK_ERR_VERIFY : DISK_OK;
      break;
    }
  }

  if (result == DISK_ERR_TIMEOUT) result = read_goal_verdict(goal, decoder);
  floppy_flux_read_stop(f);
  floppy_update_media(f);
  if (result == DISK_OK && f->media_generation != expected_generation) {
    result = DISK_ERR_MEDIA_CHANGED;
  }
  return result;
}

static disk_err_t floppy_read_recover(floppy_t *f, uint32_t expected_generation,
                                      read_goal_t *goal,
                                      const floppy_deadline_t *deadline) {
  disk_err_t status = floppy_read_flux(f, expected_generation, goal, deadline);
  if (status == DISK_OK || !floppy_status_retryable(status)) return status;
  // No index pulses at all means the spindle never ran (drive unpowered,
  // no disk): stepping around cannot help and would count as a TRK00
  // fault, so give the plain timeout back.
  if (status == DISK_ERR_TIMEOUT && !f->motor_qualified) return status;
  // Probing a format whose data rate shows no MFM: the next candidate is
  // the cure, not head jogs over track 0.
  if (goal->probe && goal->no_sync) return status;
  if (status == DISK_ERR_WRONG_TRACK) {
    f->stats.retries++;
    f->track0_confirmed = false;
    status = floppy_read_flux(f, expected_generation, goal, deadline);
    if (status == DISK_OK) {
      f->stats.recovered++;
      return DISK_OK;
    }
    if (!floppy_status_retryable(status)) return status;
  }
  static const uint8_t distances[] = {10, 20};
  for (size_t i = 0; i < sizeof(distances); i++) {
    if (deadline_elapsed(deadline)) return DISK_ERR_TIMEOUT;
    f->stats.retries++;
    status = floppy_jog(f, goal->cylinder, distances[i], deadline,
                        expected_generation);
    if (status != DISK_OK) return status;
    status = floppy_read_flux(f, expected_generation, goal, deadline);
    if (status == DISK_OK) {
      f->stats.recovered++;
      return DISK_OK;
    }
    if (!floppy_status_retryable(status)) return status;
  }
  return status;
}

static disk_err_t floppy_read_track_goal(floppy_t *f, uint32_t expected_generation,
                                         track_t *track, bool probe) {
  if (!floppy_ready(f) || !track || !f->fmt ||
      !disk_ch_valid(f->fmt, track->cylinder, track->head)) {
    return DISK_ERR_INVALID;
  }
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_READ, FLOPPY_READ_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  track->valid = 0;
  f->stats.reads++;
  read_goal_t goal = {
      .fmt = f->fmt,
      .track = track,
      .cylinder = track->cylinder,
      .head = track->head,
      .probe = probe,
  };
  status = floppy_read_recover(f, expected_generation, &goal, &deadline);
  if (status != DISK_OK) floppy_stats_failure(f, status);
  floppy_operation_end(f, FLOPPY_OPERATION_READ);
  return status;
}

disk_err_t floppy_read_track(floppy_t *f, uint32_t expected_generation,
                             track_t *track) {
  return floppy_read_track_goal(f, expected_generation, track, false);
}

disk_err_t floppy_probe_track(floppy_t *f, uint32_t expected_generation,
                              track_t *track) {
  return floppy_read_track_goal(f, expected_generation, track, true);
}

// /WD released (high) whenever the write SM is not streaming.
static void floppy_write_data_idle(floppy_t *f) {
  pio_sm_set_pins_with_mask(f->write.pio, f->write.sm,
                            1u << f->pins.write_data, 1u << f->pins.write_data);
}

static void floppy_flux_write_prepare(floppy_t *f) {
  pio_sm_set_enabled(f->write.pio, f->write.sm, false);
  dma_channel_abort(floppy_dma_tx_channel(f));
  pio_sm_clear_fifos(f->write.pio, f->write.sm);
  pio_sm_restart(f->write.pio, f->write.sm);
  pio_sm_exec(f->write.pio, f->write.sm, pio_encode_jmp(f->write.offset));
  pio_clear_stall(f->write.pio, pio_tx_stall_mask(f));
  floppy_write_data_idle(f);
  floppy_pin_oc(f->pins.write_gate, true);
}

static void floppy_flux_write_open(floppy_t *f) {
  floppy_pin_oc(f->pins.write_gate, false);
  pio_sm_set_enabled(f->write.pio, f->write.sm, true);
}

static void floppy_flux_write_close(floppy_t *f) {
  floppy_pin_oc(f->pins.write_gate, true);
  sleep_us(FLOPPY_WGATE_HOLD_US);
  pio_sm_set_enabled(f->write.pio, f->write.sm, false);
  dma_channel_abort(floppy_dma_tx_channel(f));
  floppy_write_data_idle(f);
}

// End of every write operation, whichever way it ended: the writers close
// the gate on all their own exits, this is the backstop that does not rely
// on any of them.
static void floppy_write_end(floppy_t *f) {
  floppy_flux_write_close(f);
}

typedef struct {
  floppy_t *floppy;
  const floppy_deadline_t *deadline;
  disk_err_t status;
  uint32_t generation;
  uint32_t start_ms;
  uint32_t produced;
  uint32_t completed;
  uint32_t canonical;
  uint16_t fill;
  uint16_t dma_len;
  uint8_t fill_half;
  uint8_t poll_count;
  bool dma_active;
  bool started;
  bool index_armed;
  bool boundary;
  bool canonical_done;
} flux_tx_t;

static bool flux_tx_fail(flux_tx_t *tx, disk_err_t status) {
  if (tx->status == DISK_OK) tx->status = status;
  floppy_flux_write_close(tx->floppy);
  return false;
}

static bool flux_tx_underrun(flux_tx_t *tx) {
  if (tx->status == DISK_OK) {
    tx->status = DISK_ERR_UNDERRUN;
    tx->floppy->stats.underruns++;
  }
  if (tx->started) floppy_flux_write_close(tx->floppy);
  return false;
}

static uint32_t flux_tx_fed(flux_tx_t *tx) {
  uint32_t fed = tx->completed;
  if (tx->dma_active) {
    uint32_t left =
        dma_channel_hw_addr(floppy_dma_tx_channel(tx->floppy))->transfer_count;
    if (left <= tx->dma_len) fed += tx->dma_len - left;
  }
  return fed;
}

static bool flux_tx_poll(flux_tx_t *tx) {
  if (tx->status != DISK_OK || tx->boundary) return false;
  floppy_t *f = tx->floppy;
  if (floppy_flux_media_status(f, tx->generation) != DISK_OK) {
    return flux_tx_fail(tx, DISK_ERR_MEDIA_CHANGED);
  }
  if (floppy_write_protected_raw(f)) return flux_tx_fail(tx, DISK_ERR_WRITE_PROTECTED);
  if (tx->started && pio_stalled(f->write.pio, pio_tx_stall_mask(f))) {
    return flux_tx_underrun(tx);
  }
  if (deadline_elapsed(tx->deadline) ||
      (tx->started && elapsed(tx->start_ms, FLOPPY_WRITE_FLUX_TIMEOUT_MS))) {
    return flux_tx_fail(tx, DISK_ERR_TIMEOUT);
  }
  if (!tx->started) return true;
  bool index = gpio_get(f->pins.index);
  if (index) tx->index_armed = true;
  if (!tx->index_armed || index) return true;
  uint32_t fed = flux_tx_fed(tx);
  tx->boundary = true;
  if (!tx->canonical_done || fed < tx->canonical + FLOPPY_TX_FIFO_DEPTH) {
    tx->status = DISK_ERR_VERIFY;
  }
  floppy_flux_write_close(f);
  return false;
}

// TX DMA transfer count at which the write SM's FIFO is full: the stream
// can start without an immediate underrun.
static uint32_t floppy_tx_preload_target(uint32_t length) {
  return length > FLOPPY_TX_FIFO_DEPTH ? length - FLOPPY_TX_FIFO_DEPTH : 0;
}

// Start the TX DMA on `source`: `length` PIO operands into the write SM.
static void floppy_tx_dma_start(floppy_t *f, const uint8_t *source, uint16_t length) {
  dma_channel_config config =
      dma_channel_get_default_config(floppy_dma_tx_channel(f));
  channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
  channel_config_set_read_increment(&config, true);
  channel_config_set_write_increment(&config, false);
  channel_config_set_dreq(&config, pio_get_dreq(f->write.pio, f->write.sm, true));
  channel_config_set_high_priority(&config, true);
  dma_channel_configure(floppy_dma_tx_channel(f), &config,
                        &f->write.pio->txf[f->write.sm], source, length, true);
}

static void flux_tx_configure(flux_tx_t *tx, uint8_t half, uint16_t length) {
  floppy_t *f = tx->floppy;
  const uint8_t *source = (const uint8_t *)f->flux_ring + half * FLOPPY_TX_HALF_BYTES;
  floppy_tx_dma_start(f, source, length);
  tx->dma_len = length;
  tx->dma_active = true;
}

static bool flux_tx_preload(flux_tx_t *tx) {
  uint32_t target = floppy_tx_preload_target(tx->dma_len);
  uint32_t start = now_ms();
  while (dma_channel_hw_addr(floppy_dma_tx_channel(tx->floppy))->transfer_count > target) {
    if (floppy_flux_media_status(tx->floppy, tx->generation) != DISK_OK) {
      return flux_tx_fail(tx, DISK_ERR_MEDIA_CHANGED);
    }
    if (floppy_write_protected_raw(tx->floppy)) {
      return flux_tx_fail(tx, DISK_ERR_WRITE_PROTECTED);
    }
    if (elapsed(start, FLOPPY_INDEX_TIMEOUT_MS) || deadline_elapsed(tx->deadline)) {
      return flux_tx_fail(tx, DISK_ERR_TIMEOUT);
    }
    tight_loop_contents();
  }
  return true;
}

static bool flux_tx_wait_dma(flux_tx_t *tx) {
  while (dma_channel_is_busy(floppy_dma_tx_channel(tx->floppy))) {
    if (!flux_tx_poll(tx)) return false;
    tight_loop_contents();
  }
  tx->completed += tx->dma_len;
  tx->dma_active = false;
  if (pio_stalled(tx->floppy->write.pio, pio_tx_stall_mask(tx->floppy))) {
    return flux_tx_underrun(tx);
  }
  return true;
}

static bool flux_tx_submit(flux_tx_t *tx) {
  if (tx->fill == 0 || tx->status != DISK_OK || tx->boundary) return false;
  uint8_t half = tx->fill_half;
  uint16_t length = tx->fill;
  if (!tx->started) {
    floppy_flux_write_prepare(tx->floppy);
    flux_tx_configure(tx, half, length);
    if (!flux_tx_preload(tx)) return false;
    disk_err_t status = floppy_wait_index_falling(
        tx->floppy, tx->deadline, FLOPPY_INDEX_TIMEOUT_MS, tx->generation);
    if (status != DISK_OK) return flux_tx_fail(tx, status);
    if (floppy_write_protected_raw(tx->floppy)) {
      return flux_tx_fail(tx, DISK_ERR_WRITE_PROTECTED);
    }
    if (floppy_flux_media_status(tx->floppy, tx->generation) != DISK_OK) {
      return flux_tx_fail(tx, DISK_ERR_MEDIA_CHANGED);
    }
    tx->started = true;
    tx->start_ms = now_ms();
    tx->index_armed = false;
    floppy_flux_write_open(tx->floppy);
  } else {
    if (!flux_tx_wait_dma(tx)) return false;
    flux_tx_configure(tx, half, length);
  }
  tx->fill_half ^= 1u;
  tx->fill = 0;
  return true;
}

static bool flux_tx_emit(void *ctx, uint8_t pulse) {
  flux_tx_t *tx = ctx;
  if (tx->status != DISK_OK || tx->boundary) return false;
  tx->poll_count++;
  if (tx->poll_count == FLOPPY_TX_POLL_INTERVAL) {
    tx->poll_count = 0;
    if (!flux_tx_poll(tx)) return false;
  }
  if (tx->produced >= FLOPPY_WRITE_TRANSITIONS_MAX) {
    tx->status = DISK_ERR_INVALID;
    if (tx->started) floppy_flux_write_close(tx->floppy);
    return false;
  }
  uint8_t *buffer = (uint8_t *)tx->floppy->flux_ring +
                    tx->fill_half * FLOPPY_TX_HALF_BYTES;
  buffer[tx->fill++] = pulse;
  tx->produced++;
  if (tx->fill == FLOPPY_TX_HALF_BYTES && !flux_tx_submit(tx)) return false;
  return true;
}

static disk_err_t floppy_write_flux(floppy_t *f, uint32_t expected_generation,
                                    const track_t *track,
                                    const floppy_deadline_t *deadline) {
  flux_tx_t tx = {
      .floppy = f,
      .deadline = deadline,
      .status = DISK_OK,
      .generation = expected_generation,
  };
  mfm_encode_t encoder;
  mfm_encode_init_emit(&encoder, f->fmt, flux_tx_emit, &tx);
  mfm_encode_track(&encoder, track);
  disk_err_t status = tx.status;
  if (status == DISK_OK && encoder.stopped) status = DISK_ERR_INVALID;
  if (status == DISK_OK) {
    tx.canonical = tx.produced;
    tx.canonical_done = true;
    // Gap 4b until the index. Bounded: an encoder that stopped emits
    // nothing more (the boundary would never come), and more than one
    // revolution of gap means the index is gone.
    for (uint32_t bytes = 0; tx.status == DISK_OK && !tx.boundary && !encoder.stopped &&
                             bytes < FLOPPY_TRACK_BYTES_MAX;
         bytes += 32u) {
      mfm_encode_gap(&encoder, 32);
    }
    status = tx.status;
    if (status == DISK_OK && !tx.boundary) status = DISK_ERR_TIMEOUT;
  }
  // Single exit: /WGATE released and the TX side stopped on every path.
  floppy_flux_write_close(f);
  if (status == DISK_OK) f->stats.dma_writes++;
  return status;
}

// Before /WGATE opens: decode the first ID with a good CRC on the track
// under the head and check that it names the cylinder/head about to be
// written, so a stale head position (missed step, drive brown-out) can
// never overwrite another track. DISK_ERR_TIMEOUT: no readable ID within
// one full revolution (unformatted or damaged track).
static disk_err_t floppy_check_position(floppy_t *f, uint32_t generation,
                                        uint8_t cylinder, uint8_t head,
                                        const floppy_deadline_t *deadline,
                                        uint8_t *id_cylinder_out) {
  disk_err_t status = floppy_flux_session_start(f, generation);
  if (status != DISK_OK) return status;
  mfm_t *decoder = &scratch.decoder;
  mfm_init(decoder, f->fmt);
  uint32_t transitions = 0;
  unsigned boundaries = 0;
  bool previous_index = false;
  status = DISK_ERR_TIMEOUT;
  // The first index closes a partial revolution, the second a full one.
  while (transitions < FLOPPY_READ_TRANSITIONS_MAX && boundaries < 2u) {
    if ((transitions & FLOPPY_DEADLINE_POLL_MASK) == 0 && deadline_elapsed(deadline)) break;
    floppy_pulse_t pulse;
    disk_err_t read = floppy_flux_next_internal(f, deadline, generation, &pulse);
    if (read != DISK_OK) {
      status = read;
      break;
    }
    transitions++;
    if (flux_index_boundary(&previous_index, pulse.index)) {
      boundaries++;
      mfm_reset(decoder);
      continue;
    }
    mfm_feed(decoder, pulse.delta, &scratch.sector);
    uint8_t id_cylinder;
    uint8_t id_head;
    if (!mfm_pending_id(decoder, &id_cylinder, &id_head)) continue;
    if (id_cylinder_out) *id_cylinder_out = id_cylinder;
    if (id_cylinder != cylinder) {
      FTRACE("write: head on cyl %u, not %u: no write\n", id_cylinder, cylinder);
      f->stats.wrong_track++;
      status = DISK_ERR_WRONG_TRACK;
    } else if (id_head != head) {
      FTRACE("write: ID says head %u, not %u: no write\n", id_head, head);
      f->stats.wrong_side++;
      status = DISK_ERR_WRONG_SIDE;
    } else {
      status = DISK_OK;
    }
    break;
  }
  floppy_flux_read_stop(f);
  return status;
}

static disk_err_t floppy_write_track_owned(floppy_t *f,
                                           uint32_t expected_generation,
                                           const track_t *track,
                                           const floppy_deadline_t *deadline) {
  // FORMAT semantics: the position comes from the step count since the
  // recalibration that opened the format (drive.c invalidates it at track
  // 0), exactly like a controller's FORMAT TRACK, so a track with no
  // readable ID (blank media, foreign density) is written as it is. An ID
  // naming another cylinder is either a missed step or the stale format of
  // a disk of the other track pitch (a 96 tpi disk reformatted at 48 tpi
  // reads C = 2 x cylinder; a 48 tpi disk reformatted at 96 tpi reads the
  // wide neighbour, C = physical / 2). The stale case is accepted when the
  // ID is where that pitch would put it, or after a recalibration in this
  // same write confirmed the step count.
  floppy_update_media(f);
  if (f->media_generation != expected_generation) return DISK_ERR_MEDIA_CHANGED;
  if (floppy_write_protected_raw(f)) return DISK_ERR_WRITE_PROTECTED;
  disk_err_t status = floppy_prepare(f, expected_generation, deadline);
  if (status != DISK_OK) return status;
  if (f->media_generation != expected_generation) return DISK_ERR_MEDIA_CHANGED;
  disk_err_t last = DISK_ERR_VERIFY;
  bool recalibrated = false; // TRK00 seen during this write
  for (unsigned attempt = 0; attempt < FLOPPY_WRITE_ATTEMPTS; attempt++) {
    floppy_beat(f);
    if (deadline_elapsed(deadline)) return DISK_ERR_TIMEOUT;
    floppy_update_media(f);
    if (f->media_generation != expected_generation || f->disk_change_active) {
      return DISK_ERR_MEDIA_CHANGED;
    }
    if (floppy_write_protected_raw(f)) return DISK_ERR_WRITE_PROTECTED;
    bool last_try = attempt + 1u == FLOPPY_WRITE_ATTEMPTS;
    if (last_try) f->track0_confirmed = false; // recalibrate
    if (!f->track0_confirmed) recalibrated = true;
    status = floppy_seek_internal(f, track->cylinder, deadline, expected_generation);
    if (status != DISK_OK) return status;
    status = floppy_side_select_internal(f, track->head, deadline);
    if (status != DISK_OK) return status;
    // Write where the head provably is (an ID naming this track) or where
    // nothing readable says otherwise (no ID at all: blank or foreign
    // density). An ID naming another track recalibrates and tries again.
    uint8_t id_cylinder = 0xFF;
    status = floppy_check_position(f, expected_generation, track->cylinder,
                                   track->head, deadline, &id_cylinder);
    if (status == DISK_ERR_WRONG_TRACK) {
      uint8_t phys = f->cylinder;
      bool stale_pitch = id_cylinder == phys || id_cylinder == phys / 2u ||
                         id_cylinder == (uint8_t)((phys + 1u) / 2u);
      if (recalibrated || stale_pitch) {
        FTRACE("format: stale ID C%u on physical %u, formatting over it\n",
               id_cylinder, phys);
        status = DISK_OK;
      }
    }
    if (status != DISK_OK && status != DISK_ERR_TIMEOUT) {
      last = status;
      if (!floppy_status_retryable(status)) break;
      f->stats.retries++;
      f->track0_confirmed = false;
      continue;
    }
    status = floppy_write_flux(f, expected_generation, track, deadline);
    if (status != DISK_OK) {
      last = status;
      if (!floppy_status_retryable(status)) break;
      f->stats.retries++;
      continue;
    }
    read_goal_t goal = {
        .fmt = f->fmt,
        .expected = track,
        .expected_mask = track_valid_mask(f->fmt),
        .required = track_valid_mask(f->fmt),
        .cylinder = track->cylinder,
        .head = track->head,
    };
    status = floppy_read_flux(f, expected_generation, &goal, deadline);
    floppy_update_media(f);
    if (f->media_generation != expected_generation) return DISK_ERR_MEDIA_CHANGED;
    if (status == DISK_OK) return DISK_OK;
    last = status == DISK_ERR_TIMEOUT || status == DISK_ERR_CRC ||
                   status == DISK_ERR_CORRUPT
               ? DISK_ERR_VERIFY
               : status;
    // A freshly written track that reads back badly once (one pulse out
    // of shape is enough for CORRUPT) is written again, not given up on.
    if (!floppy_status_retryable(last)) break;
    f->stats.retries++;
    status = floppy_jog(f, track->cylinder, 10, deadline, expected_generation);
    if (status != DISK_OK) {
      last = status;
      break;
    }
  }
  return last;
}

disk_err_t floppy_format_track(floppy_t *f, uint32_t expected_generation,
                               const track_t *track) {
  if (!floppy_ready(f) || !track || !f->fmt ||
      !disk_ch_valid(f->fmt, track->cylinder, track->head) ||
      track->valid != track_valid_mask(f->fmt)) {
    return DISK_ERR_INVALID;
  }
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_WRITE, FLOPPY_WRITE_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  status = floppy_write_track_owned(f, expected_generation, track, &deadline);
  floppy_write_end(f);
  if (status != DISK_OK) floppy_stats_failure(f, status);
  floppy_operation_end(f, FLOPPY_OPERATION_WRITE);
  return status;
}

disk_err_t floppy_invalidate_position(floppy_t *f) {
  if (!floppy_ready(f)) return DISK_ERR_INVALID;
  if (f->operation != FLOPPY_OPERATION_IDLE) return DISK_ERR_BUSY;
  f->track0_confirmed = false;
  return DISK_OK;
}

//--------------------------------------------------------------------
// Sector writes (uPD765 WRITE DATA)
//--------------------------------------------------------------------
//
// The track is read while the sectors to write are pre-encoded; when the ID
// of such a sector arrives (good CRC, right C/H/R), /WGATE opens at the end
// of gap 2 (22 bytes of 0x4E after the ID CRC) and the data field is
// streamed from a dedicated buffer by the TX DMA: 12 x 00, 3 x A1, FB, 512
// bytes, CRC, two gap bytes, gate closed. Reading resumes at once so the
// next ID (gap 3 later) can be caught in the same revolution; the next
// sector is encoded while the DMA plays the current one.
//
// Gate timing: the read PIO timestamps every pulse, so once the ID's CRC is
// decoded the moment it passed under the head is known relative to the
// freshest pulse in the DMA ring; that pulse's arrival is stamped with the
// microsecond timer, and the gate opens on the timer. Uncertainty is one
// pulse spacing (a few microseconds) against a 350-700 us gap.

// One buffer per sector of the track: every sector to write is encoded
// BEFORE the flux session opens, so nothing competes with draining the DMA
// ring once the disk is streaming (encoding a sector takes milliseconds,
// the ring holds about ten).
static uint8_t sector_flux[DISK_MAX_SPT][MFM_SECTOR_WRITE_PULSES_MAX]
    __attribute__((aligned(4)));

typedef struct {
  uint8_t *buf;
  uint16_t cap;
  uint16_t fill;
  bool overflow;
} flux_buf_t;

static bool flux_buf_emit(void *ctx, uint8_t pulse) {
  flux_buf_t *b = ctx;
  if (b->fill >= b->cap) {
    b->overflow = true;
    return false;
  }
  b->buf[b->fill++] = pulse;
  return true;
}

static bool floppy_encode_sector(floppy_t *f, const track_t *track, uint8_t sector,
                                 uint16_t *pulses) {
  flux_buf_t b = {.buf = sector_flux[sector], .cap = MFM_SECTOR_WRITE_PULSES_MAX};
  mfm_encode_t encoder;
  mfm_encode_init_emit(&encoder, f->fmt, flux_buf_emit, &b);
  size_t n = mfm_encode_sector_write(&encoder, track->cylinder, track->data[sector]);
  *pulses = (uint16_t)n;
  return !encoder.stopped && !b.overflow && n > 0;
}

static inline uint32_t ticks_to_us(uint32_t ticks) {
  return (ticks + MFM_TICK_HZ / 2000000u) / (MFM_TICK_HZ / 1000000u);
}

// Called with the ID just decoded. Consumes pulses until a fresh DMA word
// arrives, stamps it, and returns the timer value at which the gate must
// open (already minus the gate setup time).
static disk_err_t __not_in_flash_func(floppy_sector_gate_time)(
    floppy_t *f, uint32_t generation, const floppy_deadline_t *deadline,
    uint32_t *gate_us) {
  uint32_t after_crc = 0;
  // Pulses already in the ring arrived earlier: their age is unknown, only
  // their spacing. Drain them, then wait for the next word.
  while (f->read.half_valid || flux_ring_produced(f) != f->ring_cpu) {
    floppy_pulse_t pulse;
    disk_err_t status = floppy_flux_next_internal(f, deadline, generation, &pulse);
    if (status != DISK_OK) return status;
    after_crc += pulse.delta;
  }
  uint32_t start = time_us_32();
  while (flux_ring_produced(f) == f->ring_cpu) {
    if ((uint32_t)(time_us_32() - start) > FLOPPY_SECTOR_WORD_TIMEOUT_US) {
      return DISK_ERR_TIMEOUT;
    }
    if (pio_stalled(f->read.pio, pio_rx_stall_mask(f))) return DISK_ERR_OVERRUN;
  }
  uint32_t arrival = time_us_32(); // the second pulse of this word is "now"
  for (unsigned i = 0; i < 2; i++) {
    floppy_pulse_t pulse;
    disk_err_t status = floppy_flux_next_internal(f, deadline, generation, &pulse);
    if (status != DISK_OK) return status;
    after_crc += pulse.delta;
  }
  uint32_t gap = (MFM_GAP2_BYTES - 1u) * 8u * f->fmt->cell_ticks; // one byte early
  uint32_t crc_us = arrival - ticks_to_us(after_crc);
  *gate_us = crc_us + ticks_to_us(gap) - FLOPPY_WGATE_SETUP_US;
  return DISK_OK;
}

// Open the gate on time and start the pre-encoded stream. The read PIO is
// stopped first: the drive's read channel is meaningless during a write.
static void __not_in_flash_func(floppy_sector_gate_open)(floppy_t *f, uint32_t gate_us) {
  while ((int32_t)(time_us_32() - gate_us) < 0) tight_loop_contents();
  pio_sm_set_enabled(f->read.pio, f->read.sm, false);
  floppy_pin_oc(f->pins.write_gate, false);
  busy_wait_us_32(FLOPPY_WGATE_SETUP_US);
  pio_sm_set_enabled(f->write.pio, f->write.sm, true);
}

typedef struct {
  const track_t *track;
  uint32_t remaining; // sectors still to write
  uint32_t written;   // written in this pass, to be verified
  uint32_t encoded;   // sectors whose flux sits in sector_flux[]
  uint16_t pulses[DISK_MAX_SPT];
} sector_pass_t;

#if FLOPPY_DEBUG
// Bench diagnostics for neighbour damage after a sector write: how each
// sector of the pass was written, and a map of the track as read back.
typedef struct {
  uint8_t sector;   // 0-based
  int16_t late_us;  // gate time vs target when it was due (< 0: waited)
  uint16_t gate_us; // /WGATE open to end of stream
  uint16_t pulses;
} dbg_write_t;
static dbg_write_t dbg_write_log[DISK_MAX_SPT];
static unsigned dbg_write_count;
#endif

// The data field of sector `r` on the wire. Leaves the gate to the caller
// (floppy_sector_write_one closes it on every exit); *opened tells whether
// it was opened, i.e. whether the read side was stopped.
static disk_err_t floppy_sector_stream(floppy_t *f, uint32_t generation,
                                       const floppy_deadline_t *deadline,
                                       const sector_pass_t *pass, uint8_t r,
                                       bool *opened) {
  if (floppy_write_protected_raw(f)) return DISK_ERR_WRITE_PROTECTED;
  // TX side ready before the timing-critical part: SM parked at its first
  // instruction, FIFO preloaded, /WGATE still released.
  floppy_flux_write_prepare(f);
  floppy_tx_dma_start(f, sector_flux[r], pass->pulses[r]);
  uint32_t preload = floppy_tx_preload_target(pass->pulses[r]);
  uint32_t start = time_us_32();
  while (dma_channel_hw_addr(floppy_dma_tx_channel(f))->transfer_count > preload) {
    if ((uint32_t)(time_us_32() - start) > FLOPPY_SECTOR_PRELOAD_TIMEOUT_US) {
      return DISK_ERR_TIMEOUT;
    }
  }
  uint32_t gate_us;
  disk_err_t status = floppy_sector_gate_time(f, generation, deadline, &gate_us);
  if (status != DISK_OK) return status;
  int32_t late = (int32_t)(time_us_32() - gate_us);
  uint32_t byte_us = ticks_to_us(8u * f->fmt->cell_ticks);
  if (late > (int32_t)(FLOPPY_GATE_LATE_LIMIT_BYTES * byte_us)) {
    // Too late to land in gap 2: leave the sector for the next turn.
    f->stats.late_gate++;
    return DISK_ERR_TIMEOUT;
  }
  if (floppy_flux_media_status(f, generation) != DISK_OK) return DISK_ERR_MEDIA_CHANGED;
  if (floppy_write_protected_raw(f)) return DISK_ERR_WRITE_PROTECTED;
  floppy_sector_gate_open(f, gate_us);
  *opened = true;
  uint32_t open_ms = now_ms();
#if FLOPPY_DEBUG
  uint32_t open_us = time_us_32();
#endif

  // Wait for the stream to end: DMA done, FIFO drained, SM stalled on its
  // next pull (the last delay has been played).
  for (;;) {
    bool dma_busy = dma_channel_is_busy(floppy_dma_tx_channel(f));
    bool stalled = pio_stalled(f->write.pio, pio_tx_stall_mask(f));
    if (!dma_busy && stalled) {
#if FLOPPY_DEBUG
      if (dbg_write_count < DISK_MAX_SPT) {
        dbg_write_log[dbg_write_count++] = (dbg_write_t){
            .sector = r, .late_us = (int16_t)late,
            .gate_us = (uint16_t)(time_us_32() - open_us), .pulses = pass->pulses[r]};
      }
#endif
      return DISK_OK;
    }
    if (dma_busy && stalled) {
      f->stats.underruns++;
      return DISK_ERR_UNDERRUN;
    }
    if (floppy_write_protected_raw(f)) return DISK_ERR_WRITE_PROTECTED;
    if (floppy_flux_media_status(f, generation) != DISK_OK) return DISK_ERR_MEDIA_CHANGED;
    if (elapsed(open_ms, FLOPPY_WRITE_FLUX_TIMEOUT_MS) || deadline_elapsed(deadline)) {
      return DISK_ERR_TIMEOUT;
    }
  }
}

// Write the pre-encoded sector `r`, whose ID has just been decoded.
static disk_err_t floppy_sector_write_one(floppy_t *f, uint32_t generation,
                                          const floppy_deadline_t *deadline,
                                          sector_pass_t *pass, uint8_t r) {
  bool opened = false;
  disk_err_t status = floppy_sector_stream(f, generation, deadline, pass, r, &opened);
  // Single exit: /WGATE released and the TX side stopped, whatever the
  // stream did.
  floppy_flux_write_close(f);
  if (opened) {
    // Back to reading for the next ID.
    floppy_flux_read_start(f);
    mfm_reset(&scratch.decoder);
  }
  if (status != DISK_OK) return status;
  f->stats.sector_writes++;
  pass->remaining &= ~(1u << r);
  pass->written |= 1u << r;
  return DISK_OK;
}

// One pass over the track: write every remaining sector whose ID comes by.
static disk_err_t floppy_sector_pass(floppy_t *f, uint32_t generation,
                                     const floppy_deadline_t *deadline,
                                     sector_pass_t *pass) {
  uint8_t spt = f->fmt->sectors_per_track;
  // Everything to write is encoded now, with the flux not yet streaming.
  for (uint8_t s = 0; s < spt; s++) {
    uint32_t bit = 1u << s;
    if (!(pass->remaining & bit) || (pass->encoded & bit)) continue;
    if (!floppy_encode_sector(f, pass->track, s, &pass->pulses[s])) return DISK_ERR_INVALID;
    pass->encoded |= bit;
  }
  disk_err_t status = floppy_flux_session_start(f, generation);
  if (status != DISK_OK) return status;
  mfm_t *decoder = &scratch.decoder;
  mfm_init(decoder, f->fmt);
  pass->written = 0;
  uint32_t transitions = 0;
  unsigned boundaries = 0;
  unsigned progress_boundary = 0;
  bool previous_index = false;
  bool id_pending = false;
  status = DISK_ERR_NOT_FOUND;
  while (pass->remaining && transitions < FLOPPY_READ_TRANSITIONS_MAX) {
    if ((transitions & FLOPPY_DEADLINE_POLL_MASK) == 0 && deadline_elapsed(deadline)) {
      status = DISK_ERR_TIMEOUT;
      break;
    }
    floppy_pulse_t pulse;
    disk_err_t read = floppy_flux_next_internal(f, deadline, generation, &pulse);
    if (read != DISK_OK) {
      status = read;
      break;
    }
    transitions++;
    if (flux_index_boundary(&previous_index, pulse.index)) {
      boundaries++;
      if (boundaries - progress_boundary >= FLOPPY_SECTOR_PASS_BOUNDARIES) {
        status = decoder->syncs_found == 0 ? DISK_ERR_TIMEOUT : DISK_ERR_NOT_FOUND;
        break;
      }
      mfm_reset(decoder);
      id_pending = false;
      continue;
    }
    mfm_feed(decoder, pulse.delta, &scratch.sector);
    uint8_t c, h, r;
    bool pending = mfm_pending_record(decoder, &c, &h, &r);
    if (!pending || id_pending) {
      id_pending = pending;
      continue;
    }
    id_pending = true;
    if (c != pass->track->cylinder) {
      FTRACE("write: head on cyl %u, not %u\n", c, pass->track->cylinder);
      f->stats.wrong_track++;
      status = DISK_ERR_WRONG_TRACK;
      break;
    }
    if (h != pass->track->head) {
      f->stats.wrong_side++;
      status = DISK_ERR_WRONG_SIDE;
      break;
    }
    uint32_t bit = 1u << r;
    if (r < spt && (pass->remaining & bit) && (pass->encoded & bit)) {
      disk_err_t w = floppy_sector_write_one(f, generation, deadline, pass, r);
      if (w == DISK_OK) {
        progress_boundary = boundaries;
        id_pending = false; // decoder was reset
        continue;
      }
      if (w == DISK_ERR_TIMEOUT) {
        // Missed the gap: the ID stays pending until its data field has
        // gone by; it comes round again next revolution.
        continue;
      }
      status = w;
      break;
    }
  }
  if (!pass->remaining) status = DISK_OK;
  if (status == DISK_ERR_NOT_FOUND) f->stats.not_found++;
  floppy_flux_read_stop(f);
  return status;
}

#if FLOPPY_DEBUG
enum { MAP_SYNC, MAP_ID, MAP_ID_CRC, MAP_DATA, MAP_DATA_CRC, MAP_FORMAT };
#define DBG_MAP_EVENTS 96u
typedef struct {
  uint32_t ticks; // since the index
  uint8_t kind, sector;
} dbg_map_event_t;
static dbg_map_event_t dbg_map[DBG_MAP_EVENTS];

// One revolution, index to index, of the track under the head: every sync,
// ID, data field and decoding error with its position. Printed after the
// read (UART output during it would overrun the flux ring).
static void floppy_debug_track_map(floppy_t *f, uint32_t generation,
                                   const floppy_deadline_t *deadline) {
  for (unsigned i = 0; i < dbg_write_count; i++) {
    const dbg_write_t *w = &dbg_write_log[i];
    FTRACE("map: wrote S%u, gate %+d us vs target, open %u us, %u pulses\n",
           w->sector + 1u, w->late_us, w->gate_us, w->pulses);
  }
  if (floppy_flux_session_start(f, generation) != DISK_OK) {
    FTRACE("map: flux session refused\n");
    return;
  }
  mfm_t *d = &scratch.decoder;
  mfm_sector_t *sector = &scratch.sector;
  mfm_init(d, f->fmt);
  unsigned n = 0, boundaries = 0;
  uint32_t ticks = 0, transitions = 0;
  bool previous_index = false;
  disk_err_t status = DISK_OK;
  while (boundaries < 2 && transitions++ < FLOPPY_READ_TRANSITIONS_MAX) {
    floppy_pulse_t pulse;
    status = floppy_flux_next_internal(f, deadline, generation, &pulse);
    if (status != DISK_OK) break;
    if (flux_index_boundary(&previous_index, pulse.index)) {
      boundaries++;
      ticks = 0;
      mfm_reset(d);
      continue;
    }
    ticks += pulse.delta;
    uint32_t syncs = d->syncs_found, crcs = d->crc_errors, fmts = d->format_errors;
    mfm_record_state_t before = d->record_state;
    bool done = mfm_feed(d, pulse.delta, sector);
    if (boundaries != 1 || n >= DBG_MAP_EVENTS) continue;
    dbg_map_event_t e = {.ticks = ticks, .kind = 0xFF};
    if (d->crc_errors != crcs) {
      e.kind = before == MFM_EXPECT_ID ? MAP_ID_CRC : MAP_DATA_CRC;
      e.sector = d->pending_sector;
    } else if (d->format_errors != fmts) {
      e.kind = MAP_FORMAT;
    } else if (done) {
      e.kind = MAP_DATA;
      e.sector = sector->sector;
    } else if (before != MFM_EXPECT_DATA && d->record_state == MFM_EXPECT_DATA) {
      e.kind = MAP_ID;
      e.sector = d->pending_sector;
    } else if (d->syncs_found != syncs) {
      e.kind = MAP_SYNC;
    }
    if (e.kind != 0xFF) dbg_map[n++] = e;
  }
  floppy_flux_read_stop(f);
  static const char *const kinds[] = {"sync", "ID", "ID CRC BAD", "data",
                                      "DATA CRC BAD", "format error"};
  uint32_t byte_ticks = 8u * f->fmt->cell_ticks; // cell_ticks is one data bit
  FTRACE("map: C%u H%u, %u events, read %s (positions from index, in bytes)\n",
         f->cylinder, f->head, n, disk_err_name(status));
  for (unsigned i = 0; i < n; i++) {
    const dbg_map_event_t *e = &dbg_map[i];
    bool numbered = e->kind != MAP_SYNC && e->kind != MAP_FORMAT;
    printf("    %6lu us %5lu B  %s", (unsigned long)(e->ticks / 24u),
           (unsigned long)(e->ticks / byte_ticks), kinds[e->kind]);
    if (numbered) printf(" S%u", e->sector + 1u);
    printf("\n");
  }
}
#endif

static disk_err_t floppy_write_sectors_owned(floppy_t *f, uint32_t expected_generation,
                                             const track_t *track, uint32_t mask,
                                             uint32_t *written, uint32_t *damaged,
                                             const floppy_deadline_t *deadline) {
  floppy_update_media(f);
  if (f->media_generation != expected_generation) return DISK_ERR_MEDIA_CHANGED;
  if (floppy_write_protected_raw(f)) return DISK_ERR_WRITE_PROTECTED;
  disk_err_t status = floppy_prepare(f, expected_generation, deadline);
  if (status != DISK_OK) return status;
  if (f->media_generation != expected_generation) return DISK_ERR_MEDIA_CHANGED;
  sector_pass_t pass = {.track = track, .remaining = mask & track->valid};
  uint32_t confirmed = 0;
  disk_err_t last = DISK_ERR_NOT_FOUND;
  for (unsigned attempt = 0; attempt < FLOPPY_WRITE_ATTEMPTS && pass.remaining;
       attempt++) {
    floppy_beat(f);
    if (deadline_elapsed(deadline)) {
      last = DISK_ERR_TIMEOUT;
      break;
    }
    floppy_update_media(f);
    if (f->media_generation != expected_generation || f->disk_change_active) {
      last = DISK_ERR_MEDIA_CHANGED;
      break;
    }
    if (floppy_write_protected_raw(f)) {
      last = DISK_ERR_WRITE_PROTECTED;
      break;
    }
    bool last_try = attempt + 1u == FLOPPY_WRITE_ATTEMPTS;
    if (last_try) f->track0_confirmed = false; // recalibrate
    status = floppy_seek_internal(f, track->cylinder, deadline, expected_generation);
    if (status != DISK_OK) {
      last = status;
      break;
    }
    status = floppy_side_select_internal(f, track->head, deadline);
    if (status != DISK_OK) {
      last = status;
      break;
    }
#if FLOPPY_DEBUG
    dbg_write_count = 0;
#endif
    status = floppy_sector_pass(f, expected_generation, deadline, &pass);
    last = status;
    if (status == DISK_ERR_MEDIA_CHANGED || status == DISK_ERR_WRITE_PROTECTED) break;
    if (pass.written) {
      // Read back: the written sectors must match, and every sector the
      // cache knows on this track must still be there.
      read_goal_t goal = {
          .fmt = f->fmt,
          .expected = track,
          .expected_mask = track->valid,
          .required = track->valid,
          .cylinder = track->cylinder,
          .head = track->head,
      };
      // A read that broke off (ring overrun, timeout) says nothing about
      // the sectors it did not reach: read again before any conclusion.
      disk_err_t verify = DISK_ERR_TIMEOUT;
      for (unsigned pass_n = 0; pass_n < FLOPPY_VERIFY_READS; pass_n++) {
        verify = floppy_read_flux(f, expected_generation, &goal, deadline);
        bool complete = (goal.seen & goal.required) == goal.required;
        if (verify == DISK_ERR_MEDIA_CHANGED || complete || goal.mismatch) break;
        if (!floppy_status_retryable(verify)) break;
        f->stats.retries++;
      }
      floppy_update_media(f);
      if (f->media_generation != expected_generation) {
        last = DISK_ERR_MEDIA_CHANGED;
        break;
      }
      if (verify == DISK_ERR_MEDIA_CHANGED) {
        last = verify;
        break;
      }
      uint32_t good = goal.seen & ~goal.mismatch;
      uint32_t bad = track->valid & ~good; // unseen or different
      // Only sectors that passed the LAST verify covering them count: a
      // sector confirmed earlier and damaged by a later rewrite goes back
      // to dirty, never to the host as written.
      confirmed = (confirmed | (pass.written & good)) & ~bad;
      uint32_t collateral = bad & ~pass.written & ~pass.remaining;
      if (collateral) {
        // A neighbour we did not touch no longer reads: write it again
        // from the cached copy rather than leave it damaged.
        f->stats.collateral++;
        FTRACE("write: sectors 0x%lx damaged next to the write, rewriting\n",
               (unsigned long)collateral);
#if FLOPPY_DEBUG
        floppy_debug_track_map(f, expected_generation, deadline);
#endif
      }
      pass.remaining |= bad;
      pass.written = 0;
      if (!pass.remaining) {
        last = DISK_OK;
        break;
      }
      if (verify != DISK_OK && !floppy_status_retryable(verify) &&
          verify != DISK_ERR_CORRUPT) {
        last = verify;
        break;
      }
      last = DISK_ERR_VERIFY;
    } else if (!floppy_status_retryable(status) && status != DISK_ERR_NOT_FOUND) {
      break;
    }
    f->stats.retries++;
    if (status == DISK_ERR_WRONG_TRACK) f->track0_confirmed = false;
  }
  *written = confirmed;
  // A neighbour still in `remaining` was disturbed and not restored: the
  // caller must know, or it would take the track for clean.
  *damaged = pass.remaining & ~mask;
  return pass.remaining ? last : DISK_OK;
}

disk_err_t floppy_write_sectors(floppy_t *f, uint32_t expected_generation,
                                const track_t *track, uint32_t mask,
                                uint32_t *written, uint32_t *damaged) {
  if (written) *written = 0;
  if (damaged) *damaged = 0;
  if (!floppy_ready(f) || !track || !f->fmt || !written || !damaged ||
      !disk_ch_valid(f->fmt, track->cylinder, track->head) ||
      (mask & ~track_valid_mask(f->fmt)) != 0 || (mask & ~track->valid) != 0) {
    return DISK_ERR_INVALID;
  }
  if (!mask) return DISK_OK;
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_WRITE, FLOPPY_WRITE_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  status = floppy_write_sectors_owned(f, expected_generation, track, mask, written,
                                      damaged, &deadline);
  floppy_write_end(f);
  if (status != DISK_OK) floppy_stats_failure(f, status);
  floppy_operation_end(f, FLOPPY_OPERATION_WRITE);
  return status;
}

static void floppy_release(floppy_t *f) {
  if (f->gpio_configured) floppy_stop(f); // /WGATE released first
  if (f->read_sm_claimed) pio_sm_set_enabled(f->read.pio, f->read.sm, false);
  if (f->write_sm_claimed) {
    pio_sm_set_enabled(f->write.pio, f->write.sm, false);
    floppy_write_data_idle(f);
  }
  if (f->dma_claimed) {
    dma_channel_abort(floppy_dma_channel(f));
    dma_channel_unclaim(floppy_dma_channel(f));
    f->dma_claimed = false;
    f->dma_ch = -1;
  }
  if (f->dma_tx_claimed) {
    dma_channel_abort(floppy_dma_tx_channel(f));
    dma_channel_unclaim(floppy_dma_tx_channel(f));
    f->dma_tx_claimed = false;
    f->dma_tx_ch = -1;
  }
  if (f->gpio_configured) {
    uint8_t pins[] = FLOPPY_PIN_LIST(f->pins);
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
      gpio_deinit(pins[i]);
    }
    f->gpio_configured = false;
  }
  if (f->read_sm_claimed) {
    pio_sm_unclaim(f->read.pio, f->read.sm);
    f->read_sm_claimed = false;
  }
  if (f->write_sm_claimed) {
    pio_sm_unclaim(f->write.pio, f->write.sm);
    f->write_sm_claimed = false;
  }
  if (f->read_program_loaded) {
    pio_remove_program(f->read.pio, &flux_read_program, f->read.offset);
    f->read_program_loaded = false;
  }
  if (f->write_program_loaded) {
    pio_remove_program(f->write.pio, &flux_write_program, f->write.offset);
    f->write_program_loaded = false;
  }
  f->flux_active = false;
  f->operation = FLOPPY_OPERATION_IDLE;
  f->lifecycle = 0;
  if (active_floppy == f) active_floppy = NULL;
}

disk_err_t floppy_init(floppy_t *f, floppy_pins_t pins) {
  if (!f) return DISK_ERR_INVALID;
  if ((uintptr_t)f % _Alignof(floppy_t) != 0) return DISK_ERR_INVALID;
  if (active_floppy) return DISK_ERR_BUSY;
  uint32_t system_clock = clock_get_hz(clk_sys);
  if (system_clock == 0 || system_clock % MFM_READ_PIO_HZ != 0 ||
      !floppy_pins_valid(&pins)) {
    return DISK_ERR_INVALID;
  }
  memset(f, 0, sizeof(*f));
  f->pins = pins;
  f->last_step_dir = -1;
  f->dma_ch = -1;
  f->dma_tx_ch = -1;
  f->read.pio = pio0;
  f->write.pio = pio1;
  if (pio_get_gpio_base(f->read.pio) != 0 || pio_get_gpio_base(f->write.pio) != 0) {
    return DISK_ERR_INVALID;
  }
  active_floppy = f;

  if (!pio_can_add_program(f->read.pio, &flux_read_program) ||
      !pio_can_add_program(f->write.pio, &flux_write_program)) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  int offset = pio_add_program(f->read.pio, &flux_read_program);
  if (offset < 0) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  f->read.offset = (uint)offset;
  f->read_program_loaded = true;
  int sm = pio_claim_unused_sm(f->read.pio, false);
  if (sm < 0) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  f->read.sm = (uint)sm;
  f->read_sm_claimed = true;

  offset = pio_add_program(f->write.pio, &flux_write_program);
  if (offset < 0) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  f->write.offset = (uint)offset;
  f->write_program_loaded = true;
  sm = pio_claim_unused_sm(f->write.pio, false);
  if (sm < 0) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  f->write.sm = (uint)sm;
  f->write_sm_claimed = true;

  f->dma_ch = dma_claim_unused_channel(false);
  if (f->dma_ch < 0) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  f->dma_claimed = true;
  f->dma_tx_ch = dma_claim_unused_channel(false);
  if (f->dma_tx_ch < 0) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  f->dma_tx_claimed = true;

  uint inputs[] = {f->pins.index, f->pins.track0, f->pins.write_protect,
                   f->pins.read_data, f->pins.disk_change};
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
    gpio_init(inputs[i]);
    gpio_set_dir(inputs[i], GPIO_IN);
    gpio_pull_up(inputs[i]);
  }
  uint outputs[] = {f->pins.drive_select, f->pins.motor_enable, f->pins.direction,
                    f->pins.step, f->pins.write_data, f->pins.write_gate,
                    f->pins.side_select, f->pins.density};
  for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
    gpio_init(outputs[i]);
    gpio_disable_pulls(outputs[i]);
    gpio_put(outputs[i], 0);
    gpio_set_dir(outputs[i], GPIO_IN);
  }
  f->gpio_configured = true;
  floppy_apply_density(f);

  if (flux_read_program_init(f->read.pio, f->read.sm, f->read.offset,
                             f->pins.read_data, f->pins.index) != 0) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  pio_sm_clear_fifos(f->read.pio, f->read.sm);
  pio_sm_restart(f->read.pio, f->read.sm);
  pio_sm_set_enabled(f->read.pio, f->read.sm, false);
  if (flux_write_program_init(f->write.pio, f->write.sm, f->write.offset,
                              f->pins.write_data) != 0) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  pio_sm_set_enabled(f->write.pio, f->write.sm, false);

  f->operation = FLOPPY_OPERATION_IDLE;
  f->last_io_time_ms = now_ms();
  f->lifecycle = FLOPPY_LIFECYCLE;
  return DISK_OK;
}

static disk_err_t floppy_io_read_track(void *ctx, uint32_t expected_generation,
                                       uint8_t cylinder, uint8_t head,
                                       track_t *track) {
  if (!track) return DISK_ERR_INVALID;
  track->cylinder = cylinder;
  track->head = head;
  return floppy_read_track(ctx, expected_generation, track);
}

static disk_err_t floppy_io_write_sectors(void *ctx, uint32_t expected_generation,
                                          const track_t *track, uint32_t mask,
                                          uint32_t *written, uint32_t *damaged) {
  return floppy_write_sectors(ctx, expected_generation, track, mask, written, damaged);
}

static disk_err_t floppy_io_media_generation(void *ctx, uint32_t *generation) {
  return floppy_media_generation(ctx, generation);
}

static disk_err_t floppy_io_write_protected(void *ctx, bool *write_protected) {
  return floppy_write_protected(ctx, write_protected);
}

#if FLOPPY_DEBUG
// Bench monitor only: head moves WITHOUT the index qualification every real
// operation requires, so the stepper and TRK00 can be checked on a drive
// whose index is not (yet) arriving. Never touches /WGATE.
static disk_err_t floppy_debug_begin(floppy_t *f, floppy_deadline_t *deadline) {
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_CONTROL, FLOPPY_CONTROL_DEADLINE_MS, deadline);
  if (status != DISK_OK) return status;
  status = floppy_select_internal(f, true, deadline);
  if (status == DISK_OK) floppy_update_media(f);
  return status;
}

disk_err_t floppy_debug_step(floppy_t *f, bool inward) {
  floppy_deadline_t deadline;
  disk_err_t status = floppy_debug_begin(f, &deadline);
  if (status == DISK_OK) {
    status = floppy_step_internal(f, inward ? DIR_INWARD : DIR_OUTWARD,
                                  &deadline, f->media_generation);
  }
  if (status != DISK_ERR_BUSY) floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
  return status;
}

disk_err_t floppy_debug_home(floppy_t *f) {
  floppy_deadline_t deadline;
  disk_err_t status = floppy_debug_begin(f, &deadline);
  if (status == DISK_OK) {
    bool moved = false;
    status = floppy_seek_track0_internal(f, &deadline, f->media_generation, &moved);
  }
  if (status != DISK_ERR_BUSY) floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
  return status;
}
#endif

disk_device_t floppy_device(floppy_t *f) {
  return (disk_device_t){
      .read_track = floppy_io_read_track,
      .write_sectors = floppy_io_write_sectors,
      .media_generation = floppy_io_media_generation,
      .write_protected = floppy_io_write_protected,
      .ctx = f,
  };
}
