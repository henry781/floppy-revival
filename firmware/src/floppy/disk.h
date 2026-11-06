// Track/sector types and error codes.
// Derived from jackdoe/pico-mfm-floppy src/disk.h (MIT), reworked so
// geometry (cylinders / sectors-per-track) is a runtime floppy_format_t
// instead of 1.44MB compile-time constants. Buffers are sized for the
// largest supported 5.25" format (1.2MB: 15 sectors/track, 80 cylinders).

#ifndef DISK_H
#define DISK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "format.h"

#define DISK_SECTOR_SIZE 512u
#define DISK_HEADS 2u
#define DISK_MAX_CYLINDERS 80u
#define DISK_MAX_SPT 15u
#define DISK_MAX_SECTOR_COUNT (DISK_MAX_CYLINDERS * DISK_HEADS * DISK_MAX_SPT)

_Static_assert(DISK_MAX_SPT < 32u, "track validity must fit uint32_t");
_Static_assert(DISK_MAX_SECTOR_COUNT <= UINT16_MAX, "LBA must fit uint16_t");

typedef enum {
  DISK_OK = 0,
  DISK_END, // reserved, unused: keeps the numbering older logs quote
  DISK_ERR_INVALID,
  DISK_ERR_BUSY,
  DISK_ERR_TIMEOUT,
  DISK_ERR_CRC,
  DISK_ERR_WRONG_TRACK,
  DISK_ERR_WRONG_SIDE,
  DISK_ERR_NO_TRACK0,
  DISK_ERR_MEDIA_CHANGED,
  DISK_ERR_WRITE_PROTECTED,
  DISK_ERR_UNDERRUN,
  DISK_ERR_OVERRUN,
  DISK_ERR_VERIFY,
  DISK_ERR_CORRUPT,
  DISK_ERR_NOT_FOUND, // sector ID never came round (uPD765 "no data")
  DISK_ERR_IO,
} disk_err_t;

// Log-friendly name; the numeric value is what older logs and docs quote.
static inline const char *disk_err_name(disk_err_t err) {
  static const char *const names[] = {
      "OK",           "END",          "INVALID",     "BUSY",
      "TIMEOUT",      "CRC",          "WRONG_TRACK", "WRONG_SIDE",
      "NO_TRACK0",    "MEDIA_CHANGED", "WRITE_PROTECTED", "UNDERRUN",
      "OVERRUN",      "VERIFY",       "CORRUPT",     "NOT_FOUND",
      "IO",
  };
  return (unsigned)err < sizeof names / sizeof names[0] ? names[err] : "?";
}

typedef struct {
  disk_err_t error;
  size_t count;
} disk_result_t;

typedef struct {
  uint8_t cylinder;
  uint8_t head;
  uint32_t valid;
  uint8_t data[DISK_MAX_SPT][DISK_SECTOR_SIZE];
} track_t;

typedef struct {
  disk_err_t (*read_track)(void *ctx, uint32_t expected_generation,
                           uint8_t cylinder, uint8_t head, track_t *out);
  // Rewrite the sectors in `mask` in place (each after its own ID), then
  // read the track back and compare every sector in track->valid.
  // *written receives the sectors of `mask` confirmed on the medium, also
  // when the call fails for the others. *damaged receives the sectors of
  // track->valid outside `mask` that no longer read back right (neighbours
  // disturbed by the write and not restored): the caller holds their good
  // copy in `track` and must write them again.
  disk_err_t (*write_sectors)(void *ctx, uint32_t expected_generation,
                              const track_t *track, uint32_t mask,
                              uint32_t *written, uint32_t *damaged);
  disk_err_t (*media_generation)(void *ctx, uint32_t *generation);
  disk_err_t (*write_protected)(void *ctx, bool *write_protected);
  void *ctx;
} disk_device_t;

static inline uint32_t track_valid_mask(const floppy_format_t *fmt) {
  return (1u << fmt->sectors_per_track) - 1u;
}

static inline bool disk_ch_valid(const floppy_format_t *fmt, uint8_t cylinder,
                                 uint8_t head) {
  return cylinder < fmt->cylinders && head < fmt->heads;
}

static inline bool disk_sector_valid(const floppy_format_t *fmt, uint8_t sector) {
  return sector < fmt->sectors_per_track;
}

static inline bool disk_lba_to_chs(const floppy_format_t *fmt, uint16_t lba,
                                   uint8_t *cylinder, uint8_t *head,
                                   uint8_t *sector) {
  if (lba >= format_total_sectors(fmt) || !cylinder || !head || !sector) {
    return false;
  }
  uint16_t track = lba / fmt->sectors_per_track;
  *cylinder = (uint8_t)(track / fmt->heads);
  *head = (uint8_t)(track % fmt->heads);
  *sector = (uint8_t)(lba % fmt->sectors_per_track);
  return true;
}

static inline bool track_has(const track_t *track, uint8_t sector) {
  return track && sector < DISK_MAX_SPT && (track->valid & (1u << sector)) != 0;
}

static inline bool track_mark(track_t *track, uint8_t sector) {
  if (!track || sector >= DISK_MAX_SPT) return false;
  track->valid |= 1u << sector;
  return true;
}

#endif
