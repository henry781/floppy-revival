// Decoder robustness under timing jitter, at every supported data rate.
//
// Real flux has peak-shift and spindle wow the emulated round-trip lacks.
// This feeds the decoder pulses with deterministic pseudo-random jitter:
//  - at ±12.5% of a bit cell every sector must still decode (classification
//    margin to the short/medium boundary is 25%)
//  - at ±35% decode may fail, but no corrupted sector may ever be ACCEPTED
//    (the CRC must catch what classification lets through)

#include "../src/floppy/mfm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PULSES 120000

static uint8_t pulses[MAX_PULSES];
static size_t pulse_count;
static track_t track;

static uint32_t rng_state;

static uint32_t rng(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

// uniform in [-amplitude, +amplitude]
static int jitter(int amplitude) {
    return (int)(rng() % (2u * amplitude + 1u)) - amplitude;
}

static bool capture_pulse(void *ctx, uint8_t pulse) {
    (void)ctx;
    if (pulse_count >= MAX_PULSES) return false;
    pulses[pulse_count++] = pulse;
    return true;
}

static void build_track(const floppy_format_t *fmt, uint8_t cylinder) {
    memset(&track, 0, sizeof(track));
    track.cylinder = cylinder;
    track.head = 0;
    track.valid = track_valid_mask(fmt);
    for (uint8_t s = 0; s < fmt->sectors_per_track; s++) {
        for (size_t i = 0; i < DISK_SECTOR_SIZE; i++) {
            track.data[s][i] = (uint8_t)(rng() >> 13);
        }
    }
    pulse_count = 0;
    mfm_encode_t encoder;
    mfm_encode_init_emit(&encoder, fmt, capture_pulse, NULL);
    mfm_encode_track(&encoder, &track);
}

// Returns sectors seen; sets *corrupt if an accepted sector had wrong data.
static uint32_t decode_with_jitter(const floppy_format_t *fmt, int amplitude,
                                   bool *corrupt) {
    mfm_t decoder;
    mfm_sector_t sector;
    mfm_init(&decoder, fmt);
    uint32_t seen = 0;
    *corrupt = false;
    for (size_t i = 0; i < pulse_count; i++) {
        int delta = pulses[i] + MFM_PIO_OVERHEAD + jitter(amplitude);
        if (delta < 1) delta = 1;
        if (mfm_feed(&decoder, (uint16_t)delta, &sector)) {
            if (sector.cylinder != track.cylinder || sector.head != track.head ||
                !disk_sector_valid(fmt, sector.sector) ||
                memcmp(sector.data, track.data[sector.sector],
                       DISK_SECTOR_SIZE) != 0) {
                *corrupt = true;
            } else {
                seen |= 1u << sector.sector;
            }
        }
    }
    return seen;
}

int main(void) {
    const floppy_format_t *formats[] = {
        &FORMAT_525_360K, &FORMAT_525_360K_HD_DRIVE, &FORMAT_525_720K_HD_DRIVE, &FORMAT_525_12M,
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        const floppy_format_t *fmt = formats[i];
        rng_state = 0x12345678u + (uint32_t)i;
        build_track(fmt, 5);
        bool corrupt;

        int moderate = fmt->cell_ticks / 8; // 12.5% of a cell
        uint32_t seen = decode_with_jitter(fmt, moderate, &corrupt);
        if (seen != track_valid_mask(fmt) || corrupt) {
            printf("FAIL %s: +/-%d ticks jitter lost sectors (0x%x)%s\n", fmt->name,
                   moderate, seen, corrupt ? " CORRUPT" : "");
            failures++;
        } else {
            printf("PASS %s: full decode at +/-%d ticks jitter\n", fmt->name, moderate);
        }

        int extreme = fmt->cell_ticks * 35 / 100;
        for (int round = 0; round < 20; round++) {
            decode_with_jitter(fmt, extreme, &corrupt);
            if (corrupt) break;
        }
        if (corrupt) {
            printf("FAIL %s: corrupted sector accepted at +/-%d ticks\n", fmt->name,
                   extreme);
            failures++;
        } else {
            printf("PASS %s: no false accepts at +/-%d ticks jitter\n", fmt->name,
                   extreme);
        }
    }
    if (failures) {
        printf("%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
