// Supported 5.25" PC formats. Unlike the 3.5" 1.44MB-only reference code
// (jackdoe/pico-mfm-floppy) this port carries the geometry and MFM cell
// timing at runtime so one firmware handles 250/300/500 kbps media.

#ifndef FLOPPY_FORMAT_H_
#define FLOPPY_FORMAT_H_

#include <stdint.h>

typedef struct {
    const char *name;
    uint16_t cylinders; // logical cylinders as seen in sector IDs
    uint8_t heads;
    uint8_t sectors_per_track;
    uint16_t rpm;
    uint16_t data_rate_kbps;
    // MFM short-pulse spacing (= one bit time) in 24 MHz ticks:
    // 250 kbps -> 96, 300 kbps -> 80, 500 kbps -> 48.
    uint16_t cell_ticks;
    uint8_t double_step;      // 1: two physical steps per logical cylinder
    uint8_t precomp_start_cylinder;
    // Gap 3 (0x4E bytes after each sector) written by FORMAT, the IBM PC
    // BIOS diskette-parameter-table value for this geometry.
    uint8_t format_gap3;
} floppy_format_t;

extern const floppy_format_t FORMAT_525_360K;          // DD disk in 48tpi drive
extern const floppy_format_t FORMAT_525_360K_HD_DRIVE; // DD disk in 96tpi 360rpm drive
extern const floppy_format_t FORMAT_525_720K_HD_DRIVE; // 96tpi DD (2S/4D) disk, 96tpi drive
extern const floppy_format_t FORMAT_525_12M;           // HD disk in 96tpi drive

// Formats to try (in order) when probing inserted media, chosen at compile
// time by the drive type (FLOPPY_DRIVE_DD selects the 48tpi list).
extern const floppy_format_t *const floppy_probe_candidates[];
extern const unsigned floppy_probe_candidate_count;

static inline uint32_t format_total_sectors(const floppy_format_t *f) {
    return (uint32_t)f->cylinders * f->heads * f->sectors_per_track;
}

// The probe candidate whose capacity is exactly `blocks` 512-byte sectors
// (UFI FORMAT UNIT selects the format that way), or NULL.
const floppy_format_t *format_by_block_count(uint32_t blocks);
// Largest capacity among the candidates, for READ FORMAT CAPACITIES.
uint32_t format_max_blocks(void);

#endif // FLOPPY_FORMAT_H_
