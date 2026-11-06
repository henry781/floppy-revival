// MFM encoder/decoder.
// Derived from jackdoe/pico-mfm-floppy src/mfm.h (MIT). The reference is
// hardwired to 500 kbps (MFM_CELL_TICKS 48); here the cell time comes from
// the active floppy_format_t so 250/300/500 kbps all work with one codec.
// Pulse deltas are always expressed in 24 MHz ticks.

#ifndef MFM_H
#define MFM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "disk.h"
#include "format.h"

#define MFM_TICK_HZ 24000000u
#define MFM_READ_PIO_HZ (3u * MFM_TICK_HZ)
#define MFM_WRITE_PIO_HZ MFM_TICK_HZ
// Fixed instruction overhead of one flux_write PIO loop, in write-PIO ticks;
// the 8-bit value fed to the SM is (pulse ticks - overhead).
#define MFM_PIO_OVERHEAD 19u
#define MFM_MIN_PREAMBLE 60u
#define MFM_ID_DATA_MAX_PULSES 512u
#define MFM_GAP_BYTE 0x4Eu
#define MFM_ADDR_MARK 0xFEu
#define MFM_DATA_MARK 0xFBu
#define MFM_DELETED_MARK 0xF8u
#define MFM_INDEX_MARK 0xFCu
#define MFM_SIZE_CODE 2u
#define MFM_CRC_INIT 0xCDB4u
// Write precompensation, in 24 MHz ticks: 125 ns, the PC controller
// (82077) default for 250/300/500 kbps.
#define MFM_PRECOMP_SHIFT 3
// IBM System/34 gaps (bytes of 0x4E). Gap 3 is per format (BIOS value).
#define MFM_GAP4A_BYTES 80u
#define MFM_GAP1_BYTES 50u
#define MFM_GAP2_BYTES 22u
// 0x4E bytes written after a sector's data CRC before /WGATE closes; the
// splice then lands inside gap 3, as with a PC controller.
#define MFM_WRITE_TAIL_BYTES 2u
// Upper bound of PIO operands for one sector write: 12 x 00 + 3 x A1 +
// mark + data + CRC + tail, at most 8 pulses per byte (one per 2 half-cells).
#define MFM_SECTOR_WRITE_PULSES_MAX \
  ((12u + 3u + 1u + DISK_SECTOR_SIZE + 2u + MFM_WRITE_TAIL_BYTES) * 8u)

// Longest bit cell of any format, in ticks: 250 kbps (the slowest rate,
// 360KB in a 300 rpm drive) is 4 us = 96 ticks. format.c cell_ticks must
// not exceed it.
#define MFM_MAX_CELL_TICKS (MFM_TICK_HZ / 250000u)
// Longest pulse is 2 cells; must fit the write PIO's 8-bit delay operand.
_Static_assert(2u * MFM_MAX_CELL_TICKS - MFM_PIO_OVERHEAD <= 255u,
               "pulse fits in 8 bits");

typedef enum {
  MFM_SHORT = 0,
  MFM_MEDIUM = 1,
  MFM_LONG = 2,
} mfm_pulse_t;

typedef bool (*mfm_emit_fn)(void *ctx, uint8_t pulse);

typedef struct {
  const floppy_format_t *fmt;
  uint8_t pulse[3]; // PIO delay values for short/medium/long at fmt's rate
  size_t pos;
  mfm_emit_fn emit;
  void *emit_ctx;
  int prev_bit;
  int pending_cells;
  bool stopped;
  int precomp_shift;
  uint8_t held;
  int held_adjust;
  bool held_valid;
} mfm_encode_t;

void mfm_encode_init_emit(mfm_encode_t *encoder, const floppy_format_t *fmt,
                          mfm_emit_fn emit, void *ctx);
void mfm_encode_bytes(mfm_encode_t *encoder, const uint8_t *data, size_t len);
void mfm_encode_sync(mfm_encode_t *encoder);
void mfm_encode_gap(mfm_encode_t *encoder, size_t count);
// Precompensation on/off for the cylinder about to be written (flushes any
// held pulse first).
void mfm_encode_set_cylinder(mfm_encode_t *encoder, uint8_t cylinder);
// Emit the pulse held back by precompensation; call at the end of a stream.
void mfm_encode_flush(mfm_encode_t *encoder);
// ID field: sync, FE, C H R N, CRC, gap 2.
void mfm_encode_id(mfm_encode_t *encoder, uint8_t cylinder, uint8_t head,
                   uint8_t sector);
// Data field: sync, FB, 512 bytes, CRC.
void mfm_encode_data_field(mfm_encode_t *encoder, const uint8_t data[DISK_SECTOR_SIZE]);
// Everything /WGATE covers when one sector is rewritten in place (uPD765
// WRITE DATA): data field + MFM_WRITE_TAIL_BYTES of gap, flushed. Returns
// the number of pulses emitted.
size_t mfm_encode_sector_write(mfm_encode_t *encoder, uint8_t cylinder,
                               const uint8_t data[DISK_SECTOR_SIZE]);
size_t mfm_encode_track(mfm_encode_t *encoder, const track_t *track);

typedef enum {
  MFM_HUNT = 0,
  MFM_SYNCING,
  MFM_DATA,
  MFM_CLOCK,
} mfm_state_t;

typedef enum {
  MFM_EXPECT_ID = 0,
  MFM_EXPECT_DATA,
  MFM_READING_DATA,
} mfm_record_state_t;

typedef struct {
  uint8_t cylinder;
  uint8_t head;
  uint8_t sector;
  uint8_t data[DISK_SECTOR_SIZE];
} mfm_sector_t;

typedef struct {
  const floppy_format_t *fmt;
  mfm_state_t state;
  mfm_record_state_t record_state;
  uint16_t pulse_floor;   // reject deltas below (4/5 format cell)
  uint16_t pulse_ceiling; // reject deltas at/above (5/2 format cell)
  uint16_t T2_max;        // short/medium boundary (5/4 tracked cell)
  uint16_t T3_max;        // medium/long boundary (7/4 tracked cell)
  uint8_t byte_acc;
  uint8_t bit_count;
  uint16_t buf_pos;
  uint16_t bytes_expected;
  uint16_t crc;
  uint8_t sync_stage;
  uint16_t t_cell;        // tracked bit cell (preamble mean + PLL)
  uint16_t short_count;
  uint32_t preamble_sum;
  uint8_t pending_cylinder;
  uint8_t pending_head;
  uint8_t pending_sector;
  uint16_t pending_pulses;
  uint32_t syncs_found;
  uint32_t crc_errors;
  uint32_t format_errors;
  // First few format errors (bench diagnostics): offending delta, cell
  // estimate and decoder state/record state/bytes decoded at that moment.
  uint8_t err_log_count;
  struct {
    uint16_t delta, cell, pos, line;
    uint8_t state, record;
  } err_log[8];
  uint8_t buf[DISK_SECTOR_SIZE + 3u];
} mfm_t;

void mfm_init(mfm_t *decoder, const floppy_format_t *fmt);
void mfm_reset(mfm_t *decoder);
bool mfm_feed(mfm_t *decoder, uint16_t delta, mfm_sector_t *out);

// True from the moment an ID record with a good CRC has been decoded until
// its data field starts: *cylinder / *head are the values that ID carries.
// Lets the write path confirm the head position without a full sector.
static inline bool mfm_pending_id(const mfm_t *decoder, uint8_t *cylinder,
                                  uint8_t *head) {
  if (decoder->record_state != MFM_EXPECT_DATA) return false;
  *cylinder = decoder->pending_cylinder;
  *head = decoder->pending_head;
  return true;
}

// Same, with the sector number (0-based) the ID carries.
static inline bool mfm_pending_record(const mfm_t *decoder, uint8_t *cylinder,
                                      uint8_t *head, uint8_t *sector) {
  if (!mfm_pending_id(decoder, cylinder, head)) return false;
  *sector = decoder->pending_sector;
  return true;
}

#endif
