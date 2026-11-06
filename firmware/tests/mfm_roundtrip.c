// Host-side round-trip test for the parameterized MFM codec: encode a full
// track at each supported data rate, replay the pulse stream into the
// decoder, and require every sector to come back intact.
//
// The encoder emits PIO delay operands (ticks - MFM_PIO_OVERHEAD); the read
// side would measure the full pulse spacing, so the overhead is added back
// when feeding the decoder. The splice variant injects out-of-range noise in
// the gap between every ID and its data field, as left by the write gate
// turning on when a sector was rewritten. Build/run: tests/run_host_tests.sh

#include "../src/floppy/mfm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PULSES 120000

static uint8_t pulses[MAX_PULSES];
static size_t pulse_count;
static track_t track;

static bool capture_pulse(void *ctx, uint8_t pulse) {
    (void)ctx;
    if (pulse_count >= MAX_PULSES) return false;
    pulses[pulse_count++] = pulse;
    return true;
}

static int test_format(const floppy_format_t *fmt, uint8_t cylinder, bool splice) {
    memset(&track, 0, sizeof(track));
    track.cylinder = cylinder;
    track.head = 1;
    track.valid = track_valid_mask(fmt);
    for (uint8_t s = 0; s < fmt->sectors_per_track; s++) {
        for (size_t i = 0; i < DISK_SECTOR_SIZE; i++) {
            track.data[s][i] = (uint8_t)(s * 31u + i * 7u + cylinder);
        }
    }

    pulse_count = 0;
    mfm_encode_t encoder;
    mfm_encode_init_emit(&encoder, fmt, capture_pulse, NULL);
    mfm_encode_track(&encoder, &track);
    if (encoder.stopped) {
        // internal validation failure, or the capture buffer filled up
        printf("FAIL %s cyl %u: encoder stopped\n", fmt->name, cylinder);
        return 1;
    }

    // Track must fit one revolution, in actual flux time.
    double ticks_per_rev = (double)MFM_TICK_HZ * 60.0 / fmt->rpm;
    double total_ticks = 0;
    for (size_t i = 0; i < pulse_count; i++) {
        total_ticks += pulses[i] + MFM_PIO_OVERHEAD;
    }
    if (total_ticks > ticks_per_rev) {
        printf("FAIL %s cyl %u: track too long (%.2f revolutions)\n", fmt->name,
               cylinder, total_ticks / ticks_per_rev);
        return 1;
    }

    mfm_t decoder;
    mfm_sector_t sector;
    mfm_init(&decoder, fmt);
    uint32_t seen = 0;
    unsigned ids = 0; // ID records reported by mfm_pending_id (write position check)
    bool id_was_pending = false;
    for (size_t i = 0; i < pulse_count; i++) {
        uint16_t delta = (uint16_t)(pulses[i] + MFM_PIO_OVERHEAD);
        if (splice && decoder.record_state == MFM_EXPECT_DATA &&
            decoder.pending_pulses == 40) {
            // inside the 0x4E run of gap 2: one too-short, one too-long
            mfm_feed(&decoder, (uint16_t)(fmt->cell_ticks / 2u), &sector);
            mfm_feed(&decoder, (uint16_t)(fmt->cell_ticks * 3u), &sector);
        }
        if (mfm_feed(&decoder, delta, &sector)) {
            if (sector.cylinder != track.cylinder || sector.head != track.head ||
                memcmp(sector.data, track.data[sector.sector], DISK_SECTOR_SIZE) != 0) {
                printf("FAIL %s cyl %u: sector %u corrupt\n", fmt->name, cylinder,
                       sector.sector);
                return 1;
            }
            seen |= 1u << sector.sector;
        }
        uint8_t id_cylinder, id_head;
        bool pending = mfm_pending_id(&decoder, &id_cylinder, &id_head);
        if (pending && !id_was_pending) {
            if (id_cylinder != track.cylinder || id_head != track.head) {
                printf("FAIL %s cyl %u: pending ID says C%u H%u\n", fmt->name,
                       cylinder, id_cylinder, id_head);
                return 1;
            }
            ids++;
        }
        id_was_pending = pending;
    }
    if (ids != fmt->sectors_per_track) {
        printf("FAIL %s cyl %u: %u pending IDs, expected %u\n", fmt->name, cylinder,
               ids, fmt->sectors_per_track);
        return 1;
    }
    if (seen != track_valid_mask(fmt)) {
        printf("FAIL %s cyl %u%s: sectors seen 0x%x expected 0x%x (crc=%u fmt=%u)\n",
               fmt->name, cylinder, splice ? " splice" : "", seen, track_valid_mask(fmt),
               decoder.crc_errors, decoder.format_errors);
        return 1;
    }
    printf("PASS %s cyl %u%s (%zu pulses, %u syncs)\n", fmt->name, cylinder,
           splice ? " splice" : "", pulse_count, decoder.syncs_found);
    return 0;
}

// A sector rewritten in place: the ID stream a PC format left on the disk,
// a splice (the gate opening inside gap 2), then the data field this
// firmware writes. The decoder must deliver the sector, and precompensation
// must not change the stream's length (each shift moves one pulse, so the
// adjustments cancel in pairs).
static int test_sector_write(const floppy_format_t *fmt, uint8_t cylinder) {
    uint8_t data[DISK_SECTOR_SIZE];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 13u + cylinder);

    pulse_count = 0;
    mfm_encode_t id_enc;
    mfm_encode_init_emit(&id_enc, fmt, capture_pulse, NULL);
    mfm_encode_id(&id_enc, cylinder, 0, 4);
    mfm_encode_flush(&id_enc);
    size_t id_pulses = pulse_count;

    mfm_encode_t enc;
    mfm_encode_init_emit(&enc, fmt, capture_pulse, NULL);
    size_t n = mfm_encode_sector_write(&enc, cylinder, data);
    if (enc.stopped || n == 0 || n > MFM_SECTOR_WRITE_PULSES_MAX) {
        printf("FAIL %s cyl %u: sector write encoder stopped or too long (%zu)\n",
               fmt->name, cylinder, n);
        return 1;
    }
    double total = 0;
    for (size_t i = id_pulses; i < pulse_count; i++) total += pulses[i] + MFM_PIO_OVERHEAD;
    // Reference length without precompensation: cylinder 0.
    uint8_t ref_buf[MFM_SECTOR_WRITE_PULSES_MAX];
    size_t saved = pulse_count;
    pulse_count = 0;
    mfm_encode_t ref;
    mfm_encode_init_emit(&ref, fmt, capture_pulse, NULL);
    mfm_encode_sector_write(&ref, 0, data);
    double ref_total = 0;
    for (size_t i = 0; i < pulse_count; i++) ref_total += pulses[i] + MFM_PIO_OVERHEAD;
    memcpy(ref_buf, pulses, pulse_count);
    (void)ref_buf;
    if (pulse_count != saved - id_pulses || ref_total != total) {
        printf("FAIL %s cyl %u: precompensation changed the stream (%zu/%zu pulses, %.0f/%.0f ticks)\n",
               fmt->name, cylinder, saved - id_pulses, pulse_count, total, ref_total);
        return 1;
    }
    // Rebuild the full stream and decode it with a splice before the data field.
    pulse_count = 0;
    mfm_encode_init_emit(&id_enc, fmt, capture_pulse, NULL);
    mfm_encode_id(&id_enc, cylinder, 0, 4);
    mfm_encode_flush(&id_enc);
    mfm_encode_init_emit(&enc, fmt, capture_pulse, NULL);
    mfm_encode_sector_write(&enc, cylinder, data);

    mfm_t decoder;
    mfm_sector_t sector;
    mfm_init(&decoder, fmt);
    // Lead-in: some gap bytes so the decoder has a preamble to hunt from.
    for (size_t i = 0; i < id_pulses; i++) {
        mfm_feed(&decoder, (uint16_t)(pulses[i] + MFM_PIO_OVERHEAD), &sector);
    }
    uint8_t c, h;
    if (!mfm_pending_id(&decoder, &c, &h) || c != cylinder || h != 0) {
        printf("FAIL %s cyl %u: ID not pending after the ID field\n", fmt->name, cylinder);
        return 1;
    }
    // Splice: the last old gap byte is cut short, then the new stream.
    mfm_feed(&decoder, (uint16_t)(fmt->cell_ticks * 5u / 4u + 7u), &sector);
    bool got = false;
    for (size_t i = id_pulses; i < pulse_count; i++) {
        if (mfm_feed(&decoder, (uint16_t)(pulses[i] + MFM_PIO_OVERHEAD), &sector)) {
            got = sector.cylinder == cylinder && sector.head == 0 && sector.sector == 4 &&
                  memcmp(sector.data, data, DISK_SECTOR_SIZE) == 0;
        }
    }
    if (!got) {
        printf("FAIL %s cyl %u: rewritten sector did not decode (crc=%u fmt=%u)\n",
               fmt->name, cylinder, decoder.crc_errors, decoder.format_errors);
        return 1;
    }
    printf("PASS %s cyl %u sector write (%zu pulses, %.1f us)\n", fmt->name, cylinder, n,
           total / 24.0);
    return 0;
}

int main(void) {
    const floppy_format_t *formats[] = {
        &FORMAT_525_360K, &FORMAT_525_360K_HD_DRIVE, &FORMAT_525_720K_HD_DRIVE, &FORMAT_525_12M,
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        // mfm.h sizes the write PIO's delay operand for this longest cell.
        if (formats[i]->cell_ticks > MFM_MAX_CELL_TICKS) {
            printf("FAIL %s: cell %u ticks > MFM_MAX_CELL_TICKS\n", formats[i]->name,
                   formats[i]->cell_ticks);
            failures++;
        }
        failures += test_format(formats[i], 0, false);
        // also an inner cylinder, where write precompensation kicks in
        failures += test_format(formats[i], (uint8_t)(formats[i]->cylinders - 1u), false);
        failures += test_format(formats[i], 0, true);
        failures += test_sector_write(formats[i], 0);
        failures += test_sector_write(formats[i], (uint8_t)(formats[i]->cylinders - 1u));
    }
    if (failures) {
        printf("%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
