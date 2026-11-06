#include "format.h"
#include <stddef.h>

const floppy_format_t FORMAT_525_360K = {
    .name = "5.25\" 360KB (DD drive)",
    .cylinders = 40,
    .heads = 2,
    .sectors_per_track = 9,
    .rpm = 300,
    .data_rate_kbps = 250,
    .cell_ticks = 96,
    .double_step = 0,
    .precomp_start_cylinder = 20,
    .format_gap3 = 0x50,
};

// A 360KB disk spun at 360 RPM needs 300 kbps to keep the same bits-per-track.
const floppy_format_t FORMAT_525_360K_HD_DRIVE = {
    .name = "5.25\" 360KB (HD drive)",
    .cylinders = 40,
    .heads = 2,
    .sectors_per_track = 9,
    .rpm = 360,
    .data_rate_kbps = 300,
    .cell_ticks = 80,
    .double_step = 1,
    .precomp_start_cylinder = 20,
    .format_gap3 = 0x50,
};

// 96tpi double-density media ("2S/4D", "QD"): 80 tracks at DD bit density.
// Same 300 kbps cell as 360KB in this drive, but single-step: physical
// track 0 is identical, so probing tells the two apart from the boot
// sector or from physical track 1 (see drive.c).
const floppy_format_t FORMAT_525_720K_HD_DRIVE = {
    .name = "5.25\" 720KB 96tpi DD (HD drive)",
    .cylinders = 80,
    .heads = 2,
    .sectors_per_track = 9,
    .rpm = 360,
    .data_rate_kbps = 300,
    .cell_ticks = 80,
    .double_step = 0,
    .precomp_start_cylinder = 40,
    .format_gap3 = 0x50,
};

const floppy_format_t FORMAT_525_12M = {
    .name = "5.25\" 1.2MB",
    .cylinders = 80,
    .heads = 2,
    .sectors_per_track = 15,
    .rpm = 360,
    .data_rate_kbps = 500,
    .cell_ticks = 48,
    .double_step = 0,
    .precomp_start_cylinder = 40,
    .format_gap3 = 0x54,
};

#ifdef FLOPPY_DRIVE_DD
const floppy_format_t *const floppy_probe_candidates[] = {
    &FORMAT_525_360K,
};
#else
const floppy_format_t *const floppy_probe_candidates[] = {
    &FORMAT_525_12M,
    &FORMAT_525_720K_HD_DRIVE, // before 360KB: drive.c rejects it on 360KB media
    &FORMAT_525_360K_HD_DRIVE,
};
#endif

const unsigned floppy_probe_candidate_count =
    sizeof(floppy_probe_candidates) / sizeof(floppy_probe_candidates[0]);

const floppy_format_t *format_by_block_count(uint32_t blocks) {
    for (unsigned i = 0; i < floppy_probe_candidate_count; i++) {
        if (format_total_sectors(floppy_probe_candidates[i]) == blocks) {
            return floppy_probe_candidates[i];
        }
    }
    return NULL;
}

uint32_t format_max_blocks(void) {
    uint32_t max = 0;
    for (unsigned i = 0; i < floppy_probe_candidate_count; i++) {
        uint32_t n = format_total_sectors(floppy_probe_candidates[i]);
        if (n > max) max = n;
    }
    return max;
}
