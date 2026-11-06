// Derived from jackdoe/pico-mfm-floppy src/mfm.c (MIT); parameterized for
// runtime cell timing and geometry (see mfm.h).

#include "mfm.h"
#include "crc.h"
#include <string.h>

// Pulse classes of the three A1 bytes with the missing clock (0x4489 x3),
// as they follow a run of 0x00 bytes.
static const uint8_t mfm_sync_pattern[] = {
    MFM_MEDIUM, MFM_LONG, MFM_MEDIUM, MFM_LONG, MFM_MEDIUM,
    MFM_SHORT, MFM_LONG, MFM_MEDIUM, MFM_LONG, MFM_MEDIUM,
    MFM_SHORT, MFM_LONG, MFM_MEDIUM, MFM_LONG, MFM_MEDIUM,
};

// Three C2 bytes with the missing clock (0x5224 x3): the index address mark
// a FORMAT writes after gap 4a. Read back, they fail the A1 sync and are
// ignored, exactly as a PC controller ignores the IAM.
static const uint8_t mfm_iam_pattern[] = {
    MFM_MEDIUM, MFM_SHORT, MFM_MEDIUM, MFM_LONG, MFM_MEDIUM,
    MFM_LONG, MFM_SHORT, MFM_MEDIUM, MFM_LONG, MFM_MEDIUM,
    MFM_LONG, MFM_SHORT, MFM_MEDIUM, MFM_LONG, MFM_MEDIUM,
};

static void mfm_encode_out(mfm_encode_t *e, uint8_t timing) {
  if (e->stopped) return;
  if (!e->emit(e->emit_ctx, timing)) {
    e->stopped = true;
    return;
  }
  e->pos++;
}

// Write precompensation. Peak shift on readback pushes a flux transition
// away from its nearer neighbour, so the transition between a shorter
// interval A and a longer interval B is written early: A shrinks and B
// grows by the shift (and the mirror case late). Both intervals change, so
// the rest of the stream keeps its position: one interval is held back
// until its successor is known.
static void mfm_encode_pulse(mfm_encode_t *e, uint8_t timing) {
  if (e->stopped) return;
  if (!e->precomp_shift) {
    mfm_encode_out(e, timing);
    return;
  }
  if (!e->held_valid) {
    e->held = timing;
    e->held_adjust = 0;
    e->held_valid = true;
    return;
  }
  int shift = 0;
  if (e->held < timing) shift = -e->precomp_shift;
  else if (e->held > timing) shift = e->precomp_shift;
  mfm_encode_out(e, (uint8_t)((int)e->held + e->held_adjust + shift));
  e->held = timing;
  e->held_adjust = -shift;
}

void mfm_encode_flush(mfm_encode_t *e) {
  if (!e || !e->held_valid) return;
  if (!e->stopped) mfm_encode_out(e, (uint8_t)((int)e->held + e->held_adjust));
  e->held_valid = false;
  e->held_adjust = 0;
}

static void mfm_encode_cells(mfm_encode_t *e) {
  mfm_pulse_t pulse = e->pending_cells <= 1 ? MFM_SHORT
                      : e->pending_cells == 2 ? MFM_MEDIUM
                                              : MFM_LONG;
  mfm_encode_pulse(e, e->pulse[pulse]);
  e->pending_cells = 0;
}

static void mfm_encode_setup(mfm_encode_t *e, const floppy_format_t *fmt) {
  e->fmt = fmt;
  if (!fmt) {
    e->stopped = true;
    return;
  }
  uint16_t cell = fmt->cell_ticks;
  e->pulse[MFM_SHORT] = (uint8_t)(cell - MFM_PIO_OVERHEAD);
  e->pulse[MFM_MEDIUM] = (uint8_t)(cell * 3u / 2u - MFM_PIO_OVERHEAD);
  e->pulse[MFM_LONG] = (uint8_t)(cell * 2u - MFM_PIO_OVERHEAD);
}

void mfm_encode_init_emit(mfm_encode_t *e, const floppy_format_t *fmt,
                          mfm_emit_fn emit, void *ctx) {
  if (!e) return;
  memset(e, 0, sizeof(*e));
  e->emit = emit;
  e->emit_ctx = ctx;
  mfm_encode_setup(e, fmt);
  if (!emit) e->stopped = true;
}

void mfm_encode_set_cylinder(mfm_encode_t *e, uint8_t cylinder) {
  if (!e || !e->fmt) return;
  mfm_encode_flush(e);
  bool precomp = e->fmt->precomp_start_cylinder != 0 &&
                 cylinder >= e->fmt->precomp_start_cylinder;
  e->precomp_shift = precomp ? MFM_PRECOMP_SHIFT : 0;
}

void mfm_encode_bytes(mfm_encode_t *e, const uint8_t *data, size_t len) {
  if (!e || (!data && len != 0)) {
    if (e) e->stopped = true;
    return;
  }
  for (size_t i = 0; i < len && !e->stopped; i++) {
    uint8_t byte = data[i];
    for (int bit = 7; bit >= 0 && !e->stopped; bit--) {
      int data_bit = (byte >> bit) & 1;
      int clock_bit = e->prev_bit == 0 && data_bit == 0;
      if (clock_bit) mfm_encode_cells(e);
      else e->pending_cells++;
      if (data_bit) mfm_encode_cells(e);
      else e->pending_cells++;
      e->prev_bit = data_bit;
    }
  }
}

// 12 x 00 then three missing-clock bytes. After the pattern the encoder
// state is that of the last mark byte: A1 ends in a 1 with its pulse in
// the last cell, C2 ends in 0 with two empty cells after its last pulse.
static void mfm_encode_mark(mfm_encode_t *e, const uint8_t *pattern, size_t len,
                            int prev_bit, int pending_cells) {
  static const uint8_t preamble[12] = {0};
  if (!e || e->stopped) return;
  mfm_encode_bytes(e, preamble, sizeof(preamble));
  for (size_t i = 0; i < len && !e->stopped; i++) {
    mfm_encode_pulse(e, e->pulse[pattern[i]]);
  }
  e->prev_bit = prev_bit;
  e->pending_cells = pending_cells;
}

void mfm_encode_sync(mfm_encode_t *e) {
  mfm_encode_mark(e, mfm_sync_pattern, sizeof(mfm_sync_pattern), 1, 0);
}

static void mfm_encode_iam(mfm_encode_t *e) {
  static const uint8_t iam = MFM_INDEX_MARK;
  mfm_encode_mark(e, mfm_iam_pattern, sizeof(mfm_iam_pattern), 0, 2);
  mfm_encode_bytes(e, &iam, 1);
}

void mfm_encode_gap(mfm_encode_t *e, size_t count) {
  if (!e || e->stopped) return;
  for (size_t i = 0; i < count && !e->stopped; i++) {
    uint8_t gap = MFM_GAP_BYTE;
    mfm_encode_bytes(e, &gap, 1);
  }
}

void mfm_encode_id(mfm_encode_t *e, uint8_t cylinder, uint8_t head,
                   uint8_t sector) {
  if (!e || e->stopped) return;
  if (!e->fmt || !disk_ch_valid(e->fmt, cylinder, head) ||
      !disk_sector_valid(e->fmt, sector)) {
    e->stopped = true;
    return;
  }
  uint8_t address[] = {MFM_ADDR_MARK, cylinder, head, (uint8_t)(sector + 1u), MFM_SIZE_CODE};
  uint16_t address_crc = crc16(address, sizeof(address), MFM_CRC_INIT);
  uint8_t address_crc_bytes[] = {
      (uint8_t)(address_crc >> 8u), (uint8_t)(address_crc & 0xFFu)};
  mfm_encode_sync(e);
  mfm_encode_bytes(e, address, sizeof(address));
  mfm_encode_bytes(e, address_crc_bytes, sizeof(address_crc_bytes));
  mfm_encode_gap(e, MFM_GAP2_BYTES);
}

void mfm_encode_data_field(mfm_encode_t *e, const uint8_t data[DISK_SECTOR_SIZE]) {
  if (!e || e->stopped) return;
  if (!e->fmt || !data) {
    e->stopped = true;
    return;
  }
  uint8_t data_mark = MFM_DATA_MARK;
  uint16_t data_crc = crc16(data, DISK_SECTOR_SIZE, crc16_update(MFM_CRC_INIT, data_mark));
  uint8_t data_crc_bytes[] = {
      (uint8_t)(data_crc >> 8u), (uint8_t)(data_crc & 0xFFu)};
  mfm_encode_sync(e);
  mfm_encode_bytes(e, &data_mark, 1);
  mfm_encode_bytes(e, data, DISK_SECTOR_SIZE);
  mfm_encode_bytes(e, data_crc_bytes, sizeof(data_crc_bytes));
}

size_t mfm_encode_sector_write(mfm_encode_t *e, uint8_t cylinder,
                               const uint8_t data[DISK_SECTOR_SIZE]) {
  if (!e) return 0;
  mfm_encode_set_cylinder(e, cylinder);
  // The stream starts cold: no previous bit, no pending cells.
  e->prev_bit = 0;
  e->pending_cells = 0;
  mfm_encode_data_field(e, data);
  mfm_encode_gap(e, MFM_WRITE_TAIL_BYTES);
  mfm_encode_flush(e);
  return e->pos;
}

static void mfm_encode_sector(mfm_encode_t *e, uint8_t cylinder, uint8_t head,
                              uint8_t sector, const uint8_t data[DISK_SECTOR_SIZE]) {
  mfm_encode_id(e, cylinder, head, sector);
  mfm_encode_data_field(e, data);
}

// IBM System/34 track: gap 4a, IAM, then per sector ID + gap 2 + data +
// gap 3 (the BIOS value for the geometry). Gap 4b runs to the index.
size_t mfm_encode_track(mfm_encode_t *e, const track_t *track) {
  if (!e) return 0;
  if (!track || !e->fmt || !disk_ch_valid(e->fmt, track->cylinder, track->head) ||
      track->valid != track_valid_mask(e->fmt)) {
    e->stopped = true;
    return e->pos;
  }
  mfm_encode_set_cylinder(e, track->cylinder);
  mfm_encode_gap(e, MFM_GAP4A_BYTES);
  mfm_encode_iam(e);
  mfm_encode_gap(e, MFM_GAP1_BYTES);
  for (uint8_t sector = 0; sector < e->fmt->sectors_per_track && !e->stopped;
       sector++) {
    mfm_encode_sector(e, track->cylinder, track->head, sector, track->data[sector]);
    mfm_encode_gap(e, e->fmt->format_gap3);
  }
  mfm_encode_flush(e);
  return e->pos;
}

// Decoder timing windows, as fractions of the bit cell T (cell_ticks: one
// data bit, 4 us at 250 kbps). MFM flux intervals are nominally 1 T
// ("10"), 3/2 T ("100") and 2 T ("1000"); each class boundary sits halfway
// between two nominal values:
//
//   delta < 4/5 T_fmt           noise (too short)
//   4/5 T_fmt <= delta <= 5/4 T  short   (1 T)
//   5/4 T  <  delta <= 7/4 T     medium  (3/2 T)
//   7/4 T  <  delta <  5/2 T_fmt long    (2 T)
//   delta >= 5/2 T_fmt          noise (too long, e.g. a write splice)
//
// T_fmt is the format's nominal cell: the reject limits never move. T is
// the tracked cell: it starts at the preamble's mean short interval and,
// inside a record, a software PLL nudges it by 1/16 of the error of every
// short interval up to 9/8 T (rounded to nearest, ties up). Medium/long
// intervals, and short ones beyond 9/8 T, never retune it.
#define MFM_FLOOR(cell) ((uint32_t)(cell) * 4u / 5u)
#define MFM_CEILING(cell) ((uint32_t)(cell) * 5u / 2u)
#define MFM_SHORT_MAX(cell) ((uint32_t)(cell) * 5u / 4u)
#define MFM_MEDIUM_MAX(cell) ((uint32_t)(cell) * 7u / 4u)
#define MFM_PLL_MAX(cell) ((cell) + ((cell) >> 3)) // 9/8 T
#define MFM_PLL_GAIN 16 // correction = error / MFM_PLL_GAIN

// Record lengths in decoded bytes, mark included: FE C H R N CRC CRC, and
// FB/F8 + sector + CRC.
#define MFM_ID_RECORD_BYTES 7u
#define MFM_DATA_RECORD_BYTES (1u + DISK_SECTOR_SIZE + 2u)

static void mfm_log_error(mfm_t *m, uint16_t delta, int line) {
  m->format_errors++;
  if (m->err_log_count >= sizeof(m->err_log) / sizeof(m->err_log[0])) return;
  m->err_log[m->err_log_count].delta = delta;
  m->err_log[m->err_log_count].cell = m->t_cell;
  m->err_log[m->err_log_count].pos = m->buf_pos;
  m->err_log[m->err_log_count].line = (uint16_t)line;
  m->err_log[m->err_log_count].state = (uint8_t)m->state;
  m->err_log[m->err_log_count].record = (uint8_t)m->record_state;
  m->err_log_count++;
}

static void mfm_set_cell(mfm_t *m, uint16_t t_cell) {
  m->t_cell = t_cell;
  m->T2_max = (uint16_t)MFM_SHORT_MAX(t_cell);
  m->T3_max = (uint16_t)MFM_MEDIUM_MAX(t_cell);
}

static void mfm_hunt(mfm_t *m) {
  m->state = MFM_HUNT;
  m->short_count = 0;
  m->preamble_sum = 0;
  m->sync_stage = 0;
  m->buf_pos = 0;
  m->bytes_expected = 0;
  m->bit_count = 0;
  m->byte_acc = 0;
}

static void mfm_abort(mfm_t *m) {
  mfm_hunt(m);
  m->record_state = MFM_EXPECT_ID;
  m->pending_pulses = 0;
}

// Format error: log it (with the caller's line, for the bench dump) and
// drop the record. Always returns false, for `return mfm_fail(...)`.
static bool mfm_fail(mfm_t *m, uint16_t delta, int line) {
  mfm_log_error(m, delta, line);
  mfm_abort(m);
  return false;
}

// PLL correction: error / MFM_PLL_GAIN, rounded to nearest, ties up.
static int mfm_pll_correction(int error) {
  return error >= 0 ? (error + MFM_PLL_GAIN / 2) / MFM_PLL_GAIN
                    : -((-error + MFM_PLL_GAIN / 2 - 1) / MFM_PLL_GAIN);
}

// Pulse class of one flux interval (see the windows above), or -1 for noise.
static int mfm_classify(mfm_t *m, uint16_t delta) {
  if (delta < m->pulse_floor) return -1;
  if (delta <= m->T2_max) {
    if ((m->state == MFM_DATA || m->state == MFM_CLOCK) && m->t_cell > 0 &&
        delta <= MFM_PLL_MAX(m->t_cell)) {
      int adjustment = mfm_pll_correction((int)delta - (int)m->t_cell);
      mfm_set_cell(m, (uint16_t)((int)m->t_cell + adjustment));
    }
    return MFM_SHORT;
  }
  if (delta <= m->T3_max) return MFM_MEDIUM;
  if (delta < m->pulse_ceiling) return MFM_LONG;
  return -1;
}

static bool mfm_push_bit(mfm_t *m, int bit) {
  m->byte_acc = (uint8_t)(((uint32_t)m->byte_acc << 1u) |
                          (bit != 0 ? 1u : 0u));
  m->bit_count++;
  if (m->bit_count < 8) return true;
  if (m->buf_pos >= sizeof(m->buf)) return false;
  m->buf[m->buf_pos++] = m->byte_acc;
  m->crc = crc16_update(m->crc, m->byte_acc);
  m->bit_count = 0;
  m->byte_acc = 0;
  return true;
}

void mfm_init(mfm_t *m, const floppy_format_t *fmt) {
  if (!m || !fmt) return;
  memset(m, 0, sizeof(*m));
  m->fmt = fmt;
  m->pulse_floor = (uint16_t)MFM_FLOOR(fmt->cell_ticks);
  m->pulse_ceiling = (uint16_t)MFM_CEILING(fmt->cell_ticks);
  mfm_set_cell(m, fmt->cell_ticks);
  mfm_abort(m);
}

void mfm_reset(mfm_t *m) {
  if (!m) return;
  mfm_abort(m);
}

// Noise interval. While hunting for a preamble (e.g. the write splice in
// the gap between an ID and its data field) it only restarts the preamble:
// the pending ID survives, bounded by MFM_ID_DATA_MAX_PULSES. Inside a
// sync or a record it is a format error.
static void mfm_feed_noise(mfm_t *m, uint16_t delta) {
  if (m->state == MFM_HUNT) {
    m->short_count = 0;
    m->preamble_sum = 0;
  } else {
    mfm_fail(m, delta, __LINE__);
  }
}

// HUNT: count short intervals (the 0x00 preamble, averaged into the cell
// estimate); after at least MFM_MIN_PREAMBLE of them, the first non-short
// interval must open the A1 sync pattern.
static void mfm_feed_hunt(mfm_t *m, int pulse, uint16_t delta) {
  if (pulse == MFM_SHORT) {
    if (m->short_count == UINT16_MAX ||
        m->preamble_sum > UINT32_MAX - delta) {
      m->short_count = 1;
      m->preamble_sum = delta;
    } else {
      m->short_count++;
      m->preamble_sum += delta;
    }
    return;
  }
  if (m->short_count >= MFM_MIN_PREAMBLE) {
    mfm_set_cell(m, (uint16_t)(m->preamble_sum / m->short_count));
    if (pulse == mfm_sync_pattern[0]) {
      m->state = MFM_SYNCING;
      m->sync_stage = 1;
    } else {
      if (m->record_state == MFM_EXPECT_DATA) mfm_log_error(m, delta, __LINE__);
      mfm_abort(m);
    }
  }
  m->short_count = 0;
  m->preamble_sum = 0;
}

// SYNCING: match the rest of the three A1 bytes; on the last one, start
// decoding bytes (the CRC covers the A1s, hence MFM_CRC_INIT). A mismatch
// drops the sync; a short mismatch already counts as preamble.
static void mfm_feed_sync(mfm_t *m, int pulse, uint16_t delta) {
  if (pulse != mfm_sync_pattern[m->sync_stage]) {
    if (m->record_state == MFM_EXPECT_DATA) mfm_log_error(m, delta, __LINE__);
    mfm_abort(m);
    if (pulse == MFM_SHORT) {
      m->short_count = 1;
      m->preamble_sum = delta;
    }
    return;
  }
  m->sync_stage++;
  if (m->sync_stage < sizeof(mfm_sync_pattern)) return;
  m->syncs_found++;
  m->state = MFM_DATA;
  m->byte_acc = 0;
  m->bit_count = 0;
  m->buf_pos = 0;
  m->bytes_expected = 0;
  m->crc = MFM_CRC_INIT;
}

// DATA/CLOCK: turn one interval into data bits. DATA: the last pulse was
// a data pulse (a 1); CLOCK: a clock pulse (between two 0s, the second one
// already pushed). The interval then gives the bits up to the next pulse;
// a long interval after a clock pulse is not valid MFM. False on a format
// error or a full buffer.
static bool mfm_feed_bits(mfm_t *m, int pulse) {
  switch (m->state) {
    case MFM_DATA:
      if (pulse == MFM_SHORT) return mfm_push_bit(m, 1);
      if (pulse == MFM_MEDIUM) {
        if (!mfm_push_bit(m, 0) || !mfm_push_bit(m, 0)) return false;
        m->state = MFM_CLOCK;
        return true;
      }
      return mfm_push_bit(m, 0) && mfm_push_bit(m, 1);
    case MFM_CLOCK:
      if (pulse == MFM_SHORT) return mfm_push_bit(m, 0);
      if (pulse == MFM_MEDIUM) {
        if (!mfm_push_bit(m, 1)) return false;
        m->state = MFM_DATA;
        return true;
      }
      return false;
    default:
      return true;
  }
}

// First byte after the sync: the address mark decides the record length.
// An ID mark while a data field is awaited, a data mark with no ID before
// it, or any other byte is a format error.
static bool mfm_start_record(mfm_t *m, uint16_t delta) {
  uint8_t mark = m->buf[0];
  if (mark == MFM_ADDR_MARK) {
    if (m->record_state == MFM_EXPECT_DATA) return mfm_fail(m, delta, __LINE__);
    m->record_state = MFM_EXPECT_ID;
    m->pending_pulses = 0;
    m->bytes_expected = MFM_ID_RECORD_BYTES;
    return true;
  }
  if ((mark == MFM_DATA_MARK || mark == MFM_DELETED_MARK) &&
      m->record_state == MFM_EXPECT_DATA) {
    m->record_state = MFM_READING_DATA;
    m->bytes_expected = MFM_DATA_RECORD_BYTES;
    return true;
  }
  return mfm_fail(m, delta, __LINE__);
}

// Complete ID record: with a good CRC and a C/H/R/N the format allows, it
// becomes the pending ID its data field is matched to; the decoder then
// hunts for that field's preamble.
static void mfm_finish_id(mfm_t *m, uint16_t delta) {
  bool crc_ok = m->crc == 0;
  bool geometry_ok = disk_ch_valid(m->fmt, m->buf[1], m->buf[2]) &&
                     m->buf[3] >= 1 &&
                     m->buf[3] <= m->fmt->sectors_per_track &&
                     m->buf[4] == MFM_SIZE_CODE;
  if (!crc_ok) m->crc_errors++;
  if (!geometry_ok) mfm_log_error(m, delta, __LINE__);
  if (!crc_ok || !geometry_ok) {
    mfm_abort(m);
    return;
  }
  m->pending_cylinder = m->buf[1];
  m->pending_head = m->buf[2];
  m->pending_sector = (uint8_t)(m->buf[3] - 1u);
  m->record_state = MFM_EXPECT_DATA;
  m->pending_pulses = 0;
  mfm_hunt(m);
}

// Complete data record: returned in *out under the pending ID if its CRC
// checks. Either way the decoder starts over with the next ID.
static bool mfm_finish_data(mfm_t *m, mfm_sector_t *out) {
  if (m->crc != 0) {
    m->crc_errors++;
    mfm_abort(m);
    return false;
  }
  out->cylinder = m->pending_cylinder;
  out->head = m->pending_head;
  out->sector = m->pending_sector;
  memcpy(out->data, &m->buf[1], DISK_SECTOR_SIZE);
  mfm_abort(m);
  return true;
}

bool mfm_feed(mfm_t *m, uint16_t delta, mfm_sector_t *out) {
  if (!m || !out || !m->fmt) return false;
  if (m->record_state == MFM_EXPECT_DATA && ++m->pending_pulses > MFM_ID_DATA_MAX_PULSES) {
    mfm_fail(m, delta, __LINE__);
  }

  int pulse = mfm_classify(m, delta);
  if (pulse < 0) {
    mfm_feed_noise(m, delta);
    return false;
  }
  if (m->state == MFM_HUNT) {
    mfm_feed_hunt(m, pulse, delta);
    return false;
  }
  if (m->state == MFM_SYNCING) {
    mfm_feed_sync(m, pulse, delta);
    return false;
  }

  if (!mfm_feed_bits(m, pulse)) return mfm_fail(m, delta, __LINE__);
  if (m->buf_pos == 1 && m->bytes_expected == 0 && !mfm_start_record(m, delta)) {
    return false;
  }
  if (m->bytes_expected == 0 || m->buf_pos < m->bytes_expected) return false;

  if (m->buf[0] == MFM_ADDR_MARK) {
    mfm_finish_id(m, delta);
    return false;
  }
  return mfm_finish_data(m, out);
}
