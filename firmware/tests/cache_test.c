// Host-side test of the parameterized write-back track cache against a fake
// track device: read/refresh/merge, sector-granular flush, write-protect
// behavior, media-change invalidation, dirty-LRU eviction, a sector the
// medium never reads, the bounded write-failure policy (sectors the medium
// refuses, neighbours a write damaged, a write protection that appeared with
// data cached), binding to the probed medium only, seeding and read-ahead.

#include "../src/floppy/cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const floppy_format_t *g_fmt = &FORMAT_525_12M;

static track_t fake_disk[80][2];
static uint32_t fake_generation = 1;
static bool fake_wp;
static int fake_track_reads;
static int fake_track_writes;   // write_sectors calls
static uint32_t fake_last_mask; // mask of the last write_sectors call
static uint32_t fake_bad_read[80][2];  // sectors that never read back
static uint32_t fake_bad_write[80][2]; // sectors that never take a write
// Sectors a write next to them disturbs (data lost on the medium, reported
// in *damaged as floppy_write_sectors does once its own retries are spent).
static uint32_t fake_fragile[80][2];

static disk_err_t fake_read_track(void *ctx, uint32_t expected_generation,
                                  uint8_t cylinder, uint8_t head, track_t *out) {
    (void)ctx;
    if (expected_generation != fake_generation) return DISK_ERR_MEDIA_CHANGED;
    fake_track_reads++;
    memcpy(out, &fake_disk[cylinder][head], sizeof(track_t));
    out->valid = track_valid_mask(g_fmt) & ~fake_bad_read[cylinder][head];
    return out->valid == track_valid_mask(g_fmt) ? DISK_OK : DISK_ERR_CRC;
}

static disk_err_t fake_write_sectors(void *ctx, uint32_t expected_generation,
                                     const track_t *track, uint32_t mask,
                                     uint32_t *written, uint32_t *damaged) {
    (void)ctx;
    *written = 0;
    *damaged = 0;
    if (expected_generation != fake_generation) return DISK_ERR_MEDIA_CHANGED;
    if (fake_wp) return DISK_ERR_WRITE_PROTECTED;
    if ((mask & ~track->valid) != 0) return DISK_ERR_INVALID;
    fake_track_writes++;
    fake_last_mask = mask;
    track_t *disk = &fake_disk[track->cylinder][track->head];
    uint32_t bad = fake_bad_write[track->cylinder][track->head];
    for (uint8_t s = 0; s < g_fmt->sectors_per_track; s++) {
        uint32_t bit = 1u << s;
        if (!(mask & bit) || (bad & bit)) continue;
        memcpy(disk->data[s], track->data[s], DISK_SECTOR_SIZE);
        *written |= bit;
    }
    // A fragile neighbour the call was not asked to write is ruined; one it
    // was asked to write gets its data like any other.
    uint32_t hit = fake_fragile[track->cylinder][track->head] & track->valid & ~mask;
    for (uint8_t s = 0; s < g_fmt->sectors_per_track; s++) {
        if (hit & (1u << s)) memset(disk->data[s], 0xDD, DISK_SECTOR_SIZE);
    }
    *damaged = hit;
    return (mask & bad) || hit ? DISK_ERR_VERIFY : DISK_OK;
}

static disk_err_t fake_media_generation(void *ctx, uint32_t *generation) {
    (void)ctx;
    *generation = fake_generation;
    return DISK_OK;
}

static disk_err_t fake_write_protected(void *ctx, bool *write_protected) {
    (void)ctx;
    *write_protected = fake_wp;
    return DISK_OK;
}

static int failures;

#define CHECK(cond, ...)                       \
    do {                                       \
        if (!(cond)) {                         \
            printf("FAIL: " __VA_ARGS__);      \
            printf(" (line %d)\n", __LINE__);  \
            failures++;                        \
        }                                      \
    } while (0)

static void fill_disk(void) {
    for (int c = 0; c < 80; c++) {
        for (int h = 0; h < 2; h++) {
            fake_disk[c][h].cylinder = (uint8_t)c;
            fake_disk[c][h].head = (uint8_t)h;
            fake_disk[c][h].valid = track_valid_mask(g_fmt);
            for (int s = 0; s < g_fmt->sectors_per_track; s++) {
                memset(fake_disk[c][h].data[s], c ^ (h << 4) ^ s, DISK_SECTOR_SIZE);
            }
        }
    }
}

int main(void) {
    static cache_t cache;
    disk_device_t device = {
        .read_track = fake_read_track,
        .write_sectors = fake_write_sectors,
        .media_generation = fake_media_generation,
        .write_protected = fake_write_protected,
    };
    fill_disk();
    CHECK(cache_init(&cache, device) == DISK_OK, "cache_init");
    CHECK(cache_bind(&cache, g_fmt, fake_generation - 1u) == DISK_ERR_MEDIA_CHANGED,
          "bind refused for a medium other than the one probed");
    CHECK(cache.fmt == NULL, "nothing bound after the refusal");
    CHECK(cache_bind(&cache, g_fmt, fake_generation) == DISK_OK, "cache_bind");

    uint8_t buf[DISK_SECTOR_SIZE];
    uint8_t new_data[DISK_SECTOR_SIZE];
    memset(new_data, 0xA5, sizeof(new_data));

    // Read pulls in a whole track; a second read of the same track is free.
    CHECK(cache_read(&cache, 0, buf) == DISK_OK, "read lba 0");
    CHECK(buf[0] == 0, "lba 0 contents");
    int reads_before = fake_track_reads;
    CHECK(cache_read(&cache, 1, buf) == DISK_OK, "read lba 1");
    CHECK(fake_track_reads == reads_before, "second read hits cache");

    // Write is deferred; the medium only changes on flush, as a full track.
    uint16_t lba = (uint16_t)(3 * g_fmt->heads * g_fmt->sectors_per_track + 7);
    CHECK(cache_write(&cache, lba, new_data) == DISK_OK, "write lba");
    CHECK(cache_dirty(&cache), "cache dirty after write");
    CHECK(fake_track_writes == 0, "no device write before flush");
    CHECK(cache_read(&cache, lba, buf) == DISK_OK, "read back dirty sector");
    CHECK(memcmp(buf, new_data, sizeof(buf)) == 0, "dirty sector contents");
    // Reading a neighbor sector triggers refresh; dirty data must survive.
    CHECK(cache_read(&cache, (uint16_t)(lba + 1), buf) == DISK_OK, "read neighbor");
    CHECK(buf[0] == (3 ^ 8), "neighbor from medium");
    CHECK(cache_flush(&cache) == DISK_OK, "flush");
    CHECK(!cache_dirty(&cache), "clean after flush");
    CHECK(fake_track_writes == 1, "one write_sectors call");
    CHECK(fake_last_mask == (1u << 7), "only the dirty sector was written (mask 0x%x)",
          fake_last_mask);
    CHECK(memcmp(fake_disk[3][0].data[7], new_data, DISK_SECTOR_SIZE) == 0,
          "medium updated");
    CHECK(memcmp(fake_disk[3][0].data[8], buf, DISK_SECTOR_SIZE) == 0,
          "unwritten sector preserved");

    // A sector the medium never reads must not block writes to its track:
    // the flush needs no full-track refresh.
    fake_bad_read[5][1] = 1u << 2;
    uint16_t bad_track_lba = (uint16_t)((5 * g_fmt->heads + 1) * g_fmt->sectors_per_track);
    CHECK(cache_read(&cache, (uint16_t)(bad_track_lba + 2), buf) != DISK_OK,
          "bad sector reads as an error");
    CHECK(cache_read(&cache, (uint16_t)(bad_track_lba + 3), buf) == DISK_OK,
          "neighbour of the bad sector still reads");
    memset(new_data, 0x3C, sizeof(new_data));
    CHECK(cache_write(&cache, (uint16_t)(bad_track_lba + 4), new_data) == DISK_OK,
          "write next to a bad sector");
    int w0 = fake_track_writes;
    CHECK(cache_flush(&cache) == DISK_OK, "flush a track with an unreadable sector");
    CHECK(fake_track_writes == w0 + 1 && fake_last_mask == (1u << 4),
          "only sector 4 written on the damaged track");
    CHECK(!cache_dirty(&cache), "clean after that flush");
    CHECK(!cache.write_fault, "no write fault for a readable write");
    fake_bad_read[5][1] = 0;

    // Bounded failure policy: a sector that never takes a write is retried
    // CACHE_FLUSH_ATTEMPTS times, then dropped and reported once; other
    // dirty sectors of the same track land on the first flush.
    fake_bad_write[6][0] = 1u << 1;
    uint16_t t6 = (uint16_t)(6 * g_fmt->heads * g_fmt->sectors_per_track);
    memset(new_data, 0x77, sizeof(new_data));
    CHECK(cache_write(&cache, (uint16_t)(t6 + 1), new_data) == DISK_OK, "write doomed sector");
    CHECK(cache_write(&cache, (uint16_t)(t6 + 2), new_data) == DISK_OK, "write its neighbour");
    int attempts = 0;
    for (; attempts < 10 && cache_dirty(&cache); attempts++) {
        disk_err_t st = cache_flush(&cache);
        CHECK(st != DISK_OK, "flush with a failing sector reports an error (attempt %d)", attempts);
    }
    CHECK(attempts == (int)CACHE_FLUSH_ATTEMPTS, "dropped after %u attempts (took %d)",
          CACHE_FLUSH_ATTEMPTS, attempts);
    CHECK(memcmp(fake_disk[6][0].data[2], new_data, DISK_SECTOR_SIZE) == 0,
          "neighbour landed on the first flush");
    CHECK(cache.dropped_sectors == 1, "one sector dropped (%lu)", (unsigned long)cache.dropped_sectors);
    CHECK(cache_take_write_fault(&cache), "write fault reported once");
    CHECK(!cache_take_write_fault(&cache), "write fault reported only once");
    CHECK(!cache_dirty(&cache), "nothing left dirty after the drop");
    CHECK(cache_read(&cache, (uint16_t)(t6 + 1), buf) == DISK_OK &&
              memcmp(buf, new_data, DISK_SECTOR_SIZE) != 0,
          "dropped sector re-read from the medium, not from the lost data");
    fake_bad_write[6][0] = 0;

    // A neighbour damaged by a sector write and not restored by the
    // device's own retries: the track is not clean. The neighbour is written
    // again from the cached copy on the next flush and lands.
    uint16_t t7 = (uint16_t)(7 * g_fmt->heads * g_fmt->sectors_per_track);
    CHECK(cache_read(&cache, (uint16_t)(t7 + 4), buf) == DISK_OK, "cache track 7");
    uint8_t neighbour[DISK_SECTOR_SIZE];
    memcpy(neighbour, buf, sizeof(neighbour));
    fake_fragile[7][0] = 1u << 4;
    memset(new_data, 0x61, sizeof(new_data));
    CHECK(cache_write(&cache, (uint16_t)(t7 + 3), new_data) == DISK_OK, "write next to a fragile sector");
    CHECK(cache_flush(&cache) == DISK_ERR_VERIFY, "flush reports the damage");
    CHECK(cache_dirty(&cache), "damaged neighbour left dirty, not forgotten");
    CHECK(!cache.write_fault, "no write fault yet");
    fake_fragile[7][0] = 0;
    w0 = fake_track_writes;
    CHECK(cache_flush(&cache) == DISK_OK, "second flush restores the neighbour");
    CHECK(fake_track_writes == w0 + 1 && fake_last_mask == (1u << 4),
          "only the neighbour rewritten (mask 0x%x)", fake_last_mask);
    CHECK(memcmp(fake_disk[7][0].data[4], neighbour, DISK_SECTOR_SIZE) == 0,
          "neighbour back on the medium from the cached copy");
    CHECK(!cache_dirty(&cache) && !cache_take_write_fault(&cache), "clean, nothing to report");

    // The same neighbour damaged for good (it no longer takes a write
    // either): it goes through the failure policy like host data, three
    // flushes then dropped and reported once. Before the fix the first flush
    // returned an error with nothing dirty and the next one said GOOD.
    fake_fragile[8][0] = 1u << 4;
    fake_bad_write[8][0] = 1u << 4;
    uint16_t t8 = (uint16_t)(8 * g_fmt->heads * g_fmt->sectors_per_track);
    CHECK(cache_read(&cache, (uint16_t)(t8 + 4), buf) == DISK_OK, "cache track 8");
    CHECK(cache_write(&cache, (uint16_t)(t8 + 3), new_data) == DISK_OK, "write next to a dying sector");
    attempts = 0;
    for (; attempts < 10 && cache_dirty(&cache); attempts++) {
        CHECK(cache_flush(&cache) != DISK_OK, "collateral flush %d reports an error", attempts);
    }
    CHECK(attempts == (int)CACHE_FLUSH_ATTEMPTS, "collateral dropped after %u flushes (took %d)",
          CACHE_FLUSH_ATTEMPTS, attempts);
    CHECK(cache_take_write_fault(&cache), "collateral loss reported as a write fault");
    CHECK(cache_flush(&cache) == DISK_OK && !cache_take_write_fault(&cache),
          "reported once, then the cache is clean");
    CHECK(memcmp(fake_disk[8][0].data[3], new_data, DISK_SECTOR_SIZE) == 0,
          "the host's own sector landed");
    fake_fragile[8][0] = 0;
    fake_bad_write[8][0] = 0;

    // Write-protected: flush fails, data stays dirty, then succeeds after WP
    // off (before the attempts run out).
    fake_wp = true;
    CHECK(cache_write(&cache, 0, new_data) == DISK_OK, "write while WP (cached)");
    CHECK(cache_flush(&cache) == DISK_ERR_WRITE_PROTECTED, "flush WP");
    CHECK(cache_dirty(&cache), "still dirty after WP flush");
    fake_wp = false;
    CHECK(cache_flush(&cache) == DISK_OK, "flush after WP cleared");
    CHECK(memcmp(fake_disk[0][0].data[0], new_data, DISK_SECTOR_SIZE) == 0,
          "WP-deferred write landed");

    // Protection that stays (notch covered or J3 closed mid-copy): nothing
    // is written, and the flush ends: dropped after the attempts, reported.
    memset(new_data, 0x42, sizeof(new_data));
    CHECK(cache_write(&cache, 1, new_data) == DISK_OK, "write, then protect");
    fake_wp = true;
    w0 = fake_track_writes;
    attempts = 0;
    for (; attempts < 10 && cache_dirty(&cache); attempts++) {
        CHECK(cache_flush(&cache) == DISK_ERR_WRITE_PROTECTED, "protected flush %d", attempts);
    }
    CHECK(attempts == (int)CACHE_FLUSH_ATTEMPTS, "protected data dropped after %u flushes (took %d)",
          CACHE_FLUSH_ATTEMPTS, attempts);
    CHECK(fake_track_writes == w0, "nothing written through the protection");
    CHECK(cache_take_write_fault(&cache), "protected loss reported as a write fault");
    CHECK(memcmp(fake_disk[0][0].data[1], new_data, DISK_SECTOR_SIZE) != 0, "medium untouched");
    fake_wp = false;

    // Media change invalidates everything; dirty data lost that way is a
    // write fault for the host, whichever path noticed the change.
    CHECK(cache_write(&cache, 20, new_data) == DISK_OK, "dirty before the swap");
    fake_generation++;
    CHECK(cache_check(&cache) == DISK_ERR_MEDIA_CHANGED, "media change detected");
    CHECK(!cache_dirty(&cache), "cache cleared by the change");
    CHECK(cache_take_write_fault(&cache), "lost dirty data recorded as a write fault");
    CHECK(!cache_take_write_fault(&cache), "recorded once");
    CHECK(cache_read(&cache, 0, buf) == DISK_ERR_MEDIA_CHANGED,
          "read fails until rebind");
    CHECK(cache_bind(&cache, g_fmt, fake_generation) == DISK_OK, "rebind");
    CHECK(cache_read(&cache, 0, buf) == DISK_OK, "read after rebind");

    // Dirty LRU eviction: touch more tracks than slots; evicted dirty slot
    // must be flushed to the medium automatically.
    int writes_before = fake_track_writes;
    for (int t = 0; t < (int)CACHE_TRACKS + 1; t++) {
        uint16_t tlba = (uint16_t)(t * g_fmt->heads * g_fmt->sectors_per_track);
        memset(new_data, 0x50 + t, sizeof(new_data));
        CHECK(cache_write(&cache, tlba, new_data) == DISK_OK, "write track %d", t);
    }
    CHECK(fake_track_writes > writes_before, "eviction flushed a dirty slot");
    memset(new_data, 0x50, sizeof(new_data));
    CHECK(memcmp(fake_disk[0][0].data[0], new_data, DISK_SECTOR_SIZE) == 0,
          "evicted track 0 on medium");
    CHECK(cache_flush(&cache) == DISK_OK, "flush the rest");

    // Seeding: a track handed over by the probe/format path serves reads
    // without touching the medium; a dirty slot is never overwritten.
    track_t seed = fake_disk[20][1];
    seed.valid = track_valid_mask(g_fmt);
    memset(seed.data[0], 0xEE, DISK_SECTOR_SIZE);
    uint16_t t20 = (uint16_t)((20 * g_fmt->heads + 1) * g_fmt->sectors_per_track);
    reads_before = fake_track_reads;
    CHECK(cache_seed(&cache, &seed) == DISK_OK, "seed");
    CHECK(cache_read(&cache, t20, buf) == DISK_OK && buf[0] == 0xEE,
          "seeded data served");
    CHECK(fake_track_reads == reads_before, "seed cost no medium read");
    CHECK(cache_write(&cache, t20, new_data) == DISK_OK, "dirty the seeded track");
    CHECK(cache_seed(&cache, &seed) == DISK_ERR_BUSY, "seed refused on a dirty slot");
    CHECK(cache_flush(&cache) == DISK_OK, "flush seeded track");

    // Read-ahead: prefetch reads the track once; a later host read is free.
    reads_before = fake_track_reads;
    CHECK(!cache_has_track(&cache, 30, 0), "track 30 not cached yet");
    CHECK(cache_prefetch(&cache, 30, 0) == DISK_OK, "prefetch");
    CHECK(fake_track_reads == reads_before + 1, "prefetch read the medium once");
    CHECK(cache_prefetch(&cache, 30, 0) == DISK_OK && fake_track_reads == reads_before + 1,
          "second prefetch is a no-op");
    uint16_t t30 = (uint16_t)(30 * g_fmt->heads * g_fmt->sectors_per_track);
    CHECK(cache_read(&cache, t30, buf) == DISK_OK && fake_track_reads == reads_before + 1,
          "read after prefetch hits the cache");

    if (failures) {
        printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("all cache tests passed\n");
    return 0;
}
