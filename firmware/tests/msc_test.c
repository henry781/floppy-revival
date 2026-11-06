// Host-side test of the UFI glue (src/msc_disk.c) against a scripted fake
// blockdev: media state machine as the host sees it (no medium, UNIT
// ATTENTION once per medium, unformatted medium, ready), READ FORMAT
// CAPACITIES contents, FORMAT UNIT parsing and dispatch, the deferred write
// error reported exactly once, eject while dirty, the USB reconnect when a
// mounted medium changes, bus-reset bookkeeping, out-of-range LBAs, SYNC
// CACHE after a swap and VERIFY on a backend that stopped answering.
//
// Built with tests/shims/ standing in for TinyUSB and pico/time.h; the
// class driver itself (what reaches the host) is tests/msc_device_test.c.

#include "../src/blockdev.h"
#include "../src/msc_disk.h"
#include "tusb.h"
#include "pico/time.h"

#include <stdio.h>
#include <string.h>

//--------------------------------------------------------------------
// Shims
//--------------------------------------------------------------------

uint64_t shim_now_us;
shim_sense_t shim_sense;
int shim_disconnects, shim_connects;
bool shim_mounted = true;

bool tud_msc_set_sense(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq) {
    (void)lun;
    shim_sense.key = key;
    shim_sense.asc = asc;
    shim_sense.ascq = ascq;
    return true;
}
void tud_disconnect(void) { shim_disconnects++; }
void tud_connect(void) { shim_connects++; }
bool tud_mounted(void) { return shim_mounted; }

//--------------------------------------------------------------------
// Fake blockdev
//--------------------------------------------------------------------

static bdev_media_state_t fake_state = BDEV_MEDIA_NONE;
static uint32_t fake_seq;
static bool fake_wp;
static bool fake_dirty;
static bool fake_flush_stuck; // flush requests do not clear dirty
static int fake_write_errors; // pending, reported once each
static uint32_t fake_blocks = 2400;
static int fake_again; // bdev_read/write answer AGAIN this many times first
static bool fake_healthy = true;
static struct {
    int calls;
    uint32_t blocks;
    uint8_t cylinder, head;
    int32_t result;
} fake_format;
static uint8_t fake_disk[2400][BDEV_SECTOR_SIZE];
static uint32_t fake_bad_lba = UINT32_MAX;

void bdev_init(void) {}
void bdev_task(void) {}
bool bdev_healthy(void) { return fake_healthy; }
bdev_media_state_t bdev_media_state(void) { return fake_state; }
bool bdev_media_ready(void) { return fake_state == BDEV_MEDIA_READY; }
uint32_t bdev_media_seq(void) { return fake_seq; }
bool bdev_writable(void) { return bdev_media_ready() && !fake_wp; }
bool bdev_write_protected(void) { return fake_state != BDEV_MEDIA_NONE && fake_wp; }
bool bdev_dirty(void) { return bdev_media_ready() && fake_dirty; }
bool bdev_take_write_error(void) {
    if (!fake_write_errors) return false;
    fake_write_errors--;
    return true;
}
uint32_t bdev_block_count(void) { return bdev_media_ready() ? fake_blocks : 0; }
void bdev_geometry(uint16_t *c, uint8_t *h, uint8_t *s, uint16_t *rpm, uint16_t *kbps) {
    *c = 80; *h = 2; *s = 15; *rpm = 360; *kbps = 500;
}
unsigned bdev_formattable(uint32_t blocks[], unsigned max) {
    static const uint32_t list[] = {2400, 1440, 720};
    unsigned n = 0;
    for (; n < 3 && n < max; n++) blocks[n] = list[n];
    return n;
}
uint32_t bdev_max_blocks(void) { return 2400; }
int32_t bdev_read(uint32_t lba, uint8_t *buf) {
    if (!bdev_media_ready()) return BDEV_ENOMEDIUM;
    if (lba >= fake_blocks) return BDEV_EINVAL;
    if (fake_again) { fake_again--; return BDEV_AGAIN; }
    if (lba == fake_bad_lba) return BDEV_EIO;
    memcpy(buf, fake_disk[lba], BDEV_SECTOR_SIZE);
    return BDEV_OK;
}
int32_t bdev_write(uint32_t lba, const uint8_t *buf) {
    if (!bdev_media_ready()) return BDEV_ENOMEDIUM;
    if (fake_wp) return BDEV_EROFS;
    if (lba >= fake_blocks) return BDEV_EINVAL;
    if (fake_again) { fake_again--; return BDEV_AGAIN; }
    memcpy(fake_disk[lba], buf, BDEV_SECTOR_SIZE);
    fake_dirty = true;
    return BDEV_OK;
}
void bdev_flush_async(void) { if (!fake_flush_stuck) fake_dirty = false; }
bool bdev_flush_wait(void) {
    bdev_flush_async();
    return !fake_dirty; // stuck: the drive side stopped answering
}
int32_t bdev_format_track(uint32_t blocks, uint8_t cylinder, uint8_t head) {
    fake_format.calls++;
    fake_format.blocks = blocks;
    fake_format.cylinder = cylinder;
    fake_format.head = head;
    if (fake_format.result == BDEV_OK) fake_state = BDEV_MEDIA_READY;
    return fake_format.result;
}
static int fake_host_resets;
void bdev_host_reset(void) { fake_host_resets++; }

//--------------------------------------------------------------------
// Test harness
//--------------------------------------------------------------------

static int failures;
#define CHECK(cond, ...)                      \
    do {                                      \
        if (!(cond)) {                        \
            printf("FAIL: " __VA_ARGS__);     \
            printf(" (line %d)\n", __LINE__); \
            failures++;                       \
        }                                     \
    } while (0)

#define SENSE(k, a, q) (shim_sense.key == (k) && shim_sense.asc == (a) && shim_sense.ascq == (q))

static void insert(bdev_media_state_t state) {
    fake_state = state;
    fake_seq++;
}

static int32_t scsi(uint8_t op, uint8_t cdb_rest[15], void *buf, uint16_t len) {
    uint8_t cdb[16] = {op};
    if (cdb_rest) memcpy(&cdb[1], cdb_rest, 15);
    shim_sense = (shim_sense_t){0, 0, 0};
    return tud_msc_scsi_cb(0, cdb, buf, len);
}

static bool tur(void) {
    shim_sense = (shim_sense_t){0, 0, 0};
    return tud_msc_test_unit_ready_cb(0);
}

int main(void) {
    uint8_t buf[512];
    uint8_t rest[15];

    // Empty drive.
    CHECK(!tur() && SENSE(0x02, 0x3A, 0x00), "no medium -> not present");
    uint32_t blocks; uint16_t bsz;
    shim_sense = (shim_sense_t){0, 0, 0};
    tud_msc_capacity_cb(0, &blocks, &bsz);
    CHECK(blocks == 0 && SENSE(0x02, 0x3A, 0x00), "capacity with no medium fails 3A");
    memset(rest, 0, sizeof rest); rest[7] = 0x40;
    int32_t n = scsi(0x23, rest, buf, sizeof buf);
    CHECK(n == 12 && buf[3] == 8 && buf[8] == 0x03 &&
              buf[4] == 0 && buf[5] == 0 && buf[6] == 0x09 && buf[7] == 0x60,
          "RFC no medium: header + maximum capacity 2400, code 3 (n=%d)", (int)n);

    // Unformatted disk inserted: one UA, then 30/01; capacities list all.
    insert(BDEV_MEDIA_UNFORMATTED);
    CHECK(!tur() && SENSE(0x06, 0x28, 0x00), "new medium -> unit attention");
    CHECK(!tur() && SENSE(0x03, 0x30, 0x01), "unformatted -> cannot read medium");
    CHECK(!tur() && SENSE(0x03, 0x30, 0x01), "stays unformatted");
    n = scsi(0x23, rest, buf, sizeof buf);
    CHECK(n == 12 + 24 && buf[3] == 32 && buf[8] == 0x01, "RFC unformatted: code 1 + 3 FCDs (n=%d)", (int)n);
    CHECK(buf[12 + 3] == 0x60 && buf[20 + 3] == 0xA0 && buf[28 + 2] == 0x02 && buf[28 + 3] == 0xD0,
          "FCDs 2400 / 1440 / 720");
    CHECK(buf[12 + 7] == 0x00 && buf[12 + 6] == 0x02, "FCD block length 512");
    // Truncation to the allocation length keeps the list length.
    rest[6] = 0; rest[7] = 12;
    n = scsi(0x23, rest, buf, sizeof buf);
    CHECK(n == 12 && buf[3] == 32, "RFC truncated to 12, list length untouched");
    tud_msc_capacity_cb(0, &blocks, &bsz);
    CHECK(blocks == 0 && SENSE(0x03, 0x30, 0x01), "READ CAPACITY on unformatted -> 30/01");
    shim_sense = (shim_sense_t){0, 0, 0};
    CHECK(tud_msc_read10_cb(0, 0, 0, buf, 512) == -1 && SENSE(0x03, 0x30, 0x01),
          "READ on unformatted -> 30/01");
    shim_sense = (shim_sense_t){0, 0, 0};
    CHECK(tud_msc_write10_cb(0, 0, 0, buf, 512) == -1 && SENSE(0x03, 0x30, 0x01),
          "WRITE on unformatted -> 30/01");
    CHECK(tud_msc_is_writable_cb(0), "unformatted disk is not reported write-protected");
    memset(rest, 0, sizeof rest); rest[1] = 0x05; rest[7] = 40; // MODE SENSE(10) page 5
    n = scsi(0x5A, rest, buf, sizeof buf);
    CHECK(n == 40 && buf[3] == 0x00, "mode page WP bit clear on an unprotected unformatted disk");
    memset(rest, 0, sizeof rest); rest[6] = 0; rest[7] = 0x40;

    // FORMAT UNIT parsing.
    uint8_t list[12] = {0x00, 0xB0, 0x00, 0x08, 0x00, 0x00, 0x09, 0x60, 0x00, 0x00, 0x02, 0x00};
    memset(rest, 0, sizeof rest);
    rest[0] = 0x17; rest[1] = 3; rest[7] = 12; // track 3, 12-byte list
    fake_format.result = BDEV_OK;
    CHECK(scsi(0x04, rest, list, 12) == 0 && fake_format.calls == 1 &&
              fake_format.blocks == 2400 && fake_format.cylinder == 3 && fake_format.head == 0,
          "FORMAT UNIT track 3 side 0 at 2400 blocks");
    CHECK(fake_state == BDEV_MEDIA_READY, "format turned the medium ready");
    CHECK(tur(), "TUR GOOD after format, no new unit attention");
    list[1] = 0xB1; rest[1] = 79;
    CHECK(scsi(0x04, rest, list, 12) == 0 && fake_format.head == 1 && fake_format.cylinder == 79,
          "side bit selects head 1");
    list[1] = 0xB0;
    rest[0] = 0x07; // FmtData clear
    CHECK(scsi(0x04, rest, list, 12) == -1 && SENSE(0x05, 0x24, 0x00), "FmtData=0 rejected");
    rest[0] = 0x17; rest[7] = 0;
    CHECK(scsi(0x04, rest, list, 0) == -1 && SENSE(0x05, 0x1A, 0x00), "no parameter list rejected");
    rest[7] = 12;
    list[1] = 0xB2; // immediate
    CHECK(scsi(0x04, rest, list, 12) == -1 && SENSE(0x05, 0x26, 0x00), "immediate bit rejected");
    list[1] = 0xA0; // single track clear
    CHECK(scsi(0x04, rest, list, 12) == -1 && SENSE(0x05, 0x26, 0x00), "whole-disk format rejected");
    list[1] = 0xB0;
    list[7] = 0x61; // 2401 blocks
    fake_format.result = BDEV_EINVAL;
    CHECK(scsi(0x04, rest, list, 12) == -1 && SENSE(0x05, 0x26, 0x00), "unknown capacity -> 26/00");
    list[7] = 0x60;
    fake_format.result = BDEV_EIO;
    CHECK(scsi(0x04, rest, list, 12) == -1 && SENSE(0x03, 0x31, 0x01), "track failure -> format command failed");
    fake_format.result = BDEV_EROFS;
    CHECK(scsi(0x04, rest, list, 12) == -1 && SENSE(0x07, 0x27, 0x00), "write-protected -> 27/00");
    fake_format.result = BDEV_OK;

    // Ready medium: reads, retry contract, deferred write error once.
    memset(fake_disk[5], 0x5A, 512);
    fake_again = 2;
    CHECK(tud_msc_read10_cb(0, 5, 0, buf, 512) == 0, "read answers 0 while the drive works");
    CHECK(tud_msc_read10_cb(0, 5, 0, buf, 512) == 0, "still working");
    CHECK(tud_msc_read10_cb(0, 5, 0, buf, 512) == 512 && buf[0] == 0x5A, "read completes");
    CHECK(tud_msc_read10_cb(0, 5, 0, buf, 512) == 512, "second read of the same LBA cached");
    fake_bad_lba = 7;
    shim_sense = (shim_sense_t){0, 0, 0};
    CHECK(tud_msc_read10_cb(0, 7, 0, buf, 512) == -1 && SENSE(0x03, 0x11, 0x00),
          "unreadable sector -> unrecovered read error");
    fake_bad_lba = UINT32_MAX;
    shim_sense = (shim_sense_t){0, 0, 0};
    CHECK(tud_msc_read10_cb(0, 2400, 0, buf, 512) == -1 && SENSE(0x05, 0x21, 0x00),
          "read past the end -> LBA out of range, not a medium error");
    shim_sense = (shim_sense_t){0, 0, 0};
    CHECK(tud_msc_write10_cb(0, 2400, 0, buf, 512) == -1 && SENSE(0x05, 0x21, 0x00),
          "write past the end -> LBA out of range, not a write fault");

    memset(buf, 0xC3, 512);
    CHECK(tud_msc_write10_cb(0, 9, 0, buf, 512) == 512 && fake_disk[9][0] == 0xC3, "write lands");
    CHECK(tud_msc_read10_cb(0, 9, 0, buf, 512) == 512 && buf[0] == 0xC3, "read cache invalidated by write");
    fake_write_errors = 1;
    shim_sense = (shim_sense_t){0, 0, 0};
    CHECK(tud_msc_write10_cb(0, 10, 0, buf, 512) == -1 && SENSE(0x03, 0x03, 0x00),
          "deferred write error reported on the next write");
    shim_sense = (shim_sense_t){0, 0, 0};
    CHECK(tud_msc_write10_cb(0, 10, 0, buf, 512) == 512, "and only once");
    memset(rest, 0, sizeof rest);
    fake_write_errors = 1;
    CHECK(scsi(0x35, rest, buf, 0) == -1 && SENSE(0x03, 0x03, 0x00), "SYNC CACHE reports the error once");
    fake_dirty = true;
    fake_write_errors = 0;
    // The fake flushes synchronously in bdev_flush_async, so SYNC CACHE is GOOD.
    CHECK(scsi(0x35, rest, buf, 0) == 0, "SYNC CACHE flushes and succeeds");

    // VERIFY.
    memset(rest, 0, sizeof rest);
    rest[4] = 5; rest[7] = 2; // lba 5, 2 blocks
    CHECK(scsi(0x2F, rest, buf, 0) == 0, "VERIFY of readable sectors");
    fake_bad_lba = 6;
    CHECK(scsi(0x2F, rest, buf, 0) == -1 && SENSE(0x03, 0x11, 0x00), "VERIFY reports a bad sector");
    fake_bad_lba = UINT32_MAX;
    rest[3] = 0x09; rest[4] = 0x5F; rest[7] = 2; // lba 2399, 2 blocks
    CHECK(scsi(0x2F, rest, buf, 0) == -1 && SENSE(0x05, 0x21, 0x00), "VERIFY past the end");
    rest[1] = rest[2] = rest[3] = rest[4] = 0xFF; rest[7] = 2; // lba 0xFFFFFFFF: lba + count wraps
    CHECK(scsi(0x2F, rest, buf, 0) == -1 && SENSE(0x05, 0x21, 0x00), "VERIFY with a wrapping range");
    memset(rest, 0, sizeof rest);
    rest[4] = 5; rest[7] = 2;
    fake_again = 3;
    CHECK(scsi(0x2F, rest, buf, 0) == 0 && fake_again == 0, "VERIFY waits out a busy drive");
    // A backend that stops answering (core 1 dead) must fail the command,
    // not spin: fake_again never runs out within the test.
    fake_again = 1000000;
    fake_healthy = false;
    CHECK(scsi(0x2F, rest, buf, 0) == -1 && SENSE(0x03, 0x11, 0x00),
          "VERIFY on an unhealthy backend terminates with a medium error");
    CHECK(fake_again == 1000000 - 1, "and gives up at the first busy answer");
    fake_again = 0;
    fake_healthy = true;

    // SYNC CACHE after a swap: the new medium is announced first (no flush,
    // and the old disk's lost write is not charged to it), then TUR raises
    // the unit attention and SYNC CACHE works on the new disk.
    fake_write_errors = 1;
    insert(BDEV_MEDIA_READY);
    memset(rest, 0, sizeof rest);
    CHECK(scsi(0x35, rest, buf, 0) == -1 && SENSE(0x02, 0x3A, 0x00) && fake_write_errors == 1,
          "SYNC CACHE after a swap refused before the host acknowledged the medium");
    CHECK(!tur() && SENSE(0x06, 0x28, 0x00), "TUR then raises the unit attention");
    CHECK(tur(), "then ready");
    CHECK(scsi(0x35, rest, buf, 0) == 0, "SYNC CACHE on the new medium, old write error dropped");

    // Eject and SYNC CACHE wait for the flush (macOS never retries an
    // eject); a drive side that stopped answering fails them as a write
    // fault. The ejected medium then reads as absent until a new one.
    fake_dirty = true;
    fake_flush_stuck = true;
    shim_sense = (shim_sense_t){0, 0, 0};
    CHECK(!tud_msc_start_stop_cb(0, 0, false, true) && SENSE(0x03, 0x03, 0x00),
          "eject fails when the flush cannot complete");
    CHECK(scsi(0x35, rest, buf, 0) == -1 && SENSE(0x03, 0x03, 0x00),
          "SYNC CACHE fails when the flush cannot complete");
    fake_flush_stuck = false;
    CHECK(scsi(0x35, rest, buf, 0) == 0 && !fake_dirty, "SYNC CACHE waits for the flush");
    fake_dirty = true;
    fake_write_errors = 1;
    CHECK(!tud_msc_start_stop_cb(0, 0, false, true) && SENSE(0x03, 0x03, 0x00),
          "eject reports a lost write once");
    shim_sense = (shim_sense_t){0, 0, 0};
    CHECK(tud_msc_start_stop_cb(0, 0, false, true), "eject then goes through");
    CHECK(!tur() && SENSE(0x02, 0x3A, 0x00), "ejected medium reads as not present");
    fake_write_errors = 1; // lost with the ejected medium
    insert(BDEV_MEDIA_READY);
    CHECK(!tur() && SENSE(0x06, 0x28, 0x00), "re-inserted -> unit attention");
    CHECK(tur(), "then ready");
    shim_sense = (shim_sense_t){0, 0, 0};
    CHECK(tud_msc_write10_cb(0, 11, 0, buf, 512) == 512,
          "an error pending from the previous medium is not charged to the new disk");

    // Swap under a mounted volume: msc_task disconnects, reconnects 1 s later.
    insert(BDEV_MEDIA_READY);
    msc_task();
    CHECK(shim_disconnects == 1, "disconnect on a swap under a mounted volume");
    msc_task();
    CHECK(shim_connects == 0, "reconnect waits");
    shim_now_us += 1100000;
    msc_task();
    CHECK(shim_connects == 1, "reconnect after 1 s");
    CHECK(!tur() && SENSE(0x06, 0x28, 0x00), "new medium announced after the reconnect");
    CHECK(tur(), "ready again");

    // Pulling the disk with no replacement also drops off the bus.
    fake_state = BDEV_MEDIA_NONE;
    msc_task();
    CHECK(shim_disconnects == 2, "disconnect when the disk is pulled");
    shim_now_us += 1100000;
    msc_task();
    CHECK(shim_connects == 2, "reconnect");
    CHECK(!tur() && SENSE(0x02, 0x3A, 0x00), "no medium after the pull");
    // A disk that was never mounted (unformatted) does not trigger reconnects.
    insert(BDEV_MEDIA_UNFORMATTED);
    tur(); tur();
    fake_state = BDEV_MEDIA_NONE;
    msc_task();
    CHECK(shim_disconnects == 2, "no reconnect for an unmounted medium");

    // Bus reset: the host must get a fresh unit attention, and the backend
    // is told to forget its in-flight request.
    insert(BDEV_MEDIA_READY);
    tur(); tur();
    CHECK(tur(), "ready before reset");
    tud_umount_cb();
    CHECK(fake_host_resets == 1, "backend told about the bus reset");
    CHECK(!tur() && SENSE(0x06, 0x28, 0x00), "unit attention after a bus reset");

    // Write-protected medium.
    fake_wp = true;
    CHECK(!tud_msc_is_writable_cb(0), "WP reported");
    shim_sense = (shim_sense_t){0, 0, 0};
    CHECK(tud_msc_write10_cb(0, 1, 0, buf, 512) == -1 && SENSE(0x07, 0x27, 0x00),
          "write on WP medium -> data protect");
    fake_wp = false;

    // INQUIRY strings.
    uint8_t vid[8], pid[16], rev[4];
    tud_msc_inquiry_cb(0, vid, pid, rev);
    CHECK(memcmp(rev, "1.00", 4) == 0, "product revision 1.00");

    if (failures) {
        printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("all msc tests passed\n");
    return 0;
}
