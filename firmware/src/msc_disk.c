// TinyUSB MSC callbacks bridging the UFI-subclass device to a blockdev
// backend (RAM virtual disk or real 5.25" drive, chosen at link time).
//
// TinyUSB's msc_device (our copy, src/tinyusb/msc_device.c) handles INQUIRY,
// TEST UNIT READY, READ CAPACITY(10), MODE SENSE(6), REQUEST SENSE and
// READ/WRITE(10) through the callbacks below; everything else lands in
// tud_msc_scsi_cb, where the UFI-specific commands live: MODE SENSE(10)
// with the Flexible Disk page (how Windows learns floppy geometry), READ
// FORMAT CAPACITIES (what can be formatted, and whether the medium is
// formatted at all), FORMAT UNIT (one track at a time, as the UFI spec
// defines it), VERIFY and SYNCHRONIZE CACHE. Returning 0 from read10/write10
// tells TinyUSB to retry later: that is how slow mechanical I/O stays
// non-blocking. FORMAT UNIT and VERIFY block instead: the host is waiting
// for that very command and UFI has no immediate mode.
//
// Media states as the host sees them (UFI 1.0 sense codes):
//   empty drive            TEST UNIT READY -> NOT READY, 3A/00 medium not present
//   disk no format reads   TEST UNIT READY -> MEDIUM ERROR, 30/01 unknown format
//                          READ FORMAT CAPACITIES -> descriptor code 01b
//                          (unformatted) + every formattable capacity
//   readable disk          GOOD (after one UNIT ATTENTION 28/00 per medium)
// Until TEST UNIT READY has raised that unit attention, media-access commands
// fail with 3A/00, so no sector of a new disk meets the host's old FAT. A
// sector outside the medium fails READ/WRITE/VERIFY with ILLEGAL REQUEST
// 21/00 (LBA out of range).
// A deferred write error (sectors dropped after repeated failures, or dirty
// data lost with the medium) is reported ONCE, on the next write, SYNC
// CACHE, FORMAT UNIT or eject, as MEDIUM ERROR 03/00 write fault; eject
// then goes through.

#include "msc_disk.h"

#include "blockdev.h"
#include "pico/time.h"
#include "tusb.h"

#include <stdio.h>
#include <string.h>

// floppy_revival_debug: trace the host's media polling on UART0.
#if defined(FLOPPY_DEBUG) && FLOPPY_DEBUG
#define MSC_TRACE(...) printf("  [msc] " __VA_ARGS__)
#else
#define MSC_TRACE(...) ((void)0)
#endif

#define SCSI_CMD_FORMAT_UNIT 0x04
#define SCSI_CMD_VERIFY_10 0x2F
#define SCSI_CMD_SYNCHRONIZE_CACHE_10 0x35
#define SCSI_CMD_MODE_SENSE_10 0x5A
#define SCSI_CMD_READ_FORMAT_CAPACITIES 0x23
#define MODE_PAGE_FLEXIBLE_DISK 0x05
#define MODE_PAGE_ALL 0x3F

// UFI READ FORMAT CAPACITIES descriptor codes.
#define UFI_CAP_UNFORMATTED 0x01
#define UFI_CAP_FORMATTED 0x02
#define UFI_CAP_NO_MEDIA 0x03
#define UFI_MAX_FORMATTABLE 4u

// MODE SENSE(10) reply: mode parameter header + Flexible Disk page 05h.
#define MODE_HEADER10_LEN 8u
#define MODE_PAGE_FLEXIBLE_DISK_LEN 32u

// Delay before re-attaching after a swap under a mounted volume: long enough
// for the host to see the device leave and drop the volume.
#define MSC_RECONNECT_MS 1000

// Sense data (key, ASC, ASCQ) this glue reports, UFI 1.0 / SPC codes.
typedef struct {
    uint8_t key, asc, ascq;
} msc_sense_t;

static const msc_sense_t SENSE_MEDIUM_NOT_PRESENT = {SCSI_SENSE_NOT_READY, 0x3A, 0x00};
static const msc_sense_t SENSE_UNKNOWN_FORMAT = {SCSI_SENSE_MEDIUM_ERROR, 0x30, 0x01};
static const msc_sense_t SENSE_UNRECOVERED_READ = {SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00};
static const msc_sense_t SENSE_WRITE_FAULT = {SCSI_SENSE_MEDIUM_ERROR, 0x03, 0x00};
static const msc_sense_t SENSE_FORMAT_FAILED = {SCSI_SENSE_MEDIUM_ERROR, 0x31, 0x01};
static const msc_sense_t SENSE_INVALID_COMMAND = {SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00};
static const msc_sense_t SENSE_LBA_OUT_OF_RANGE = {SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00};
static const msc_sense_t SENSE_INVALID_CDB_FIELD = {SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00};
static const msc_sense_t SENSE_INVALID_PARAM_FIELD = {SCSI_SENSE_ILLEGAL_REQUEST, 0x26, 0x00};
static const msc_sense_t SENSE_PARAM_LIST_LENGTH = {SCSI_SENSE_ILLEGAL_REQUEST, 0x1A, 0x00};
static const msc_sense_t SENSE_MEDIA_CHANGED = {SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00};
static const msc_sense_t SENSE_WRITE_PROTECTED = {SCSI_SENSE_DATA_PROTECT, 0x27, 0x00};

static void set_sense(uint8_t lun, msc_sense_t s) {
    tud_msc_set_sense(lun, s.key, s.asc, s.ascq);
}

static struct {
    uint32_t lba;
    uint32_t media_seq;
    bool valid;
    uint8_t data[BDEV_SECTOR_SIZE];
} rd_cache;

static uint8_t wr_buf[BDEV_SECTOR_SIZE];

static uint32_t acked_media_seq; // media the host was told about (UA)
// The host holds a volume on acked_media_seq. macOS stops polling TEST UNIT
// READY once a UFI volume is mounted, so a swap can never be reported in
// band: msc_task() drops off the bus instead and the host remounts.
static bool host_has_media;
// Media the host ejected (START STOP UNIT): reported absent until the next
// swap, otherwise the host would remount the same disk straight away.
static uint32_t ejected_media_seq;
static absolute_time_t reconnect_at;
static bool reconnect_pending;

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id, "FLOPREV ", 8);
    memcpy(product_id, "5.25 Floppy     ", 16);
    memcpy(product_rev, "1.00", 4);
}

// Sense for a medium that cannot take a media-access command right now.
static void set_media_sense(uint8_t lun, bdev_media_state_t state) {
    set_sense(lun, state == BDEV_MEDIA_UNFORMATTED ? SENSE_UNKNOWN_FORMAT
                                                   : SENSE_MEDIUM_NOT_PRESENT);
}

// Sense for a failed blockdev call (any status but BDEV_OK / BDEV_AGAIN).
// `failure` is what a medium error means for this command (unrecovered read,
// write fault, format failed); `invalid` what a rejected argument means
// (LBA out of range for sector I/O, a bad format parameter for FORMAT UNIT).
static void set_bdev_sense(uint8_t lun, int32_t status, msc_sense_t failure,
                           msc_sense_t invalid) {
    switch (status) {
    case BDEV_ENOMEDIUM:
        set_sense(lun, SENSE_MEDIUM_NOT_PRESENT);
        break;
    case BDEV_EROFS:
        set_sense(lun, SENSE_WRITE_PROTECTED);
        break;
    case BDEV_EINVAL:
        set_sense(lun, invalid);
        break;
    default: // BDEV_EIO
        set_sense(lun, failure);
        break;
    }
}

// Media the host has not acknowledged through TEST UNIT READY yet: its
// cached FAT belongs to the previous disk, so no sector may cross.
static bool media_unacked(uint8_t lun) {
    if (bdev_media_seq() == acked_media_seq) return false;
    rd_cache.valid = false;
    set_sense(lun, SENSE_MEDIUM_NOT_PRESENT);
    return true;
}

// A write error waiting to be told: fails the command once.
static bool report_write_error(uint8_t lun) {
    if (!bdev_take_write_error()) return false;
    MSC_TRACE("deferred write error reported\n");
    set_sense(lun, SENSE_WRITE_FAULT);
    return true;
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    bdev_media_state_t state = bdev_media_state();
    uint32_t seq = bdev_media_seq();
    if (state == BDEV_MEDIA_NONE || seq == ejected_media_seq) {
        rd_cache.valid = false;
        MSC_TRACE("TUR -> medium not present\n");
        set_sense(lun, SENSE_MEDIUM_NOT_PRESENT);
        return false;
    }
    // Keyed on the media sequence number, not a not-ready edge: a swap whose
    // not-ready window fell between two TUR polls still raises the unit
    // attention, so the host re-reads capacity and filesystem metadata.
    if (seq != acked_media_seq) {
        MSC_TRACE("TUR -> unit attention, media changed\n");
        acked_media_seq = seq;
        rd_cache.valid = false;
        // A write error still pending belongs to the medium the host no
        // longer has (it got the unit attention, or a reconnect, for that):
        // never charge it to the new disk's first write.
        while (bdev_take_write_error()) {
        }
        set_sense(lun, SENSE_MEDIA_CHANGED);
        return false;
    }
    if (state == BDEV_MEDIA_UNFORMATTED) {
        MSC_TRACE("TUR -> unformatted medium\n");
        set_media_sense(lun, state);
        return false;
    }
    host_has_media = true;
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    bdev_media_state_t state = bdev_media_state();
    *block_size = BDEV_SECTOR_SIZE;
    *block_count = state == BDEV_MEDIA_READY ? bdev_block_count() : 0;
    // msc_device fails READ CAPACITY on a zero count with our sense.
    if (*block_count == 0) set_media_sense(lun, state);
}

// Feeds the WP bit of MODE SENSE(6) and gates WRITE(10) in msc_device:
// "not protected" for an unformatted disk, whose writes fail on the medium
// state instead (hosts read this before offering to format).
bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return !bdev_write_protected();
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)power_condition;
    MSC_TRACE("START STOP UNIT power %u start %d loej %d\n", power_condition, start, load_eject);
    if (load_eject && !start) {
        if (report_write_error(lun)) return false;
        // Hold the eject until the write-back cache is on the medium: macOS
        // sends it once and does not retry a "becoming ready" answer.
        if (!bdev_flush_wait()) {
            set_sense(lun, SENSE_WRITE_FAULT);
            return false;
        }
        if (report_write_error(lun)) return false; // the flush gave up
        MSC_TRACE("eject\n");
        ejected_media_seq = bdev_media_seq();
        host_has_media = false;
    }
    return true;
}

void msc_task(void) {
    if (reconnect_pending) {
        if (absolute_time_diff_us(get_absolute_time(), reconnect_at) <= 0) {
            reconnect_pending = false;
            tud_connect();
        }
        return;
    }
    if (!host_has_media || !tud_mounted()) return;
    if (bdev_media_ready() && bdev_media_seq() == acked_media_seq) return;
    // The disk under a mounted volume was pulled or swapped: re-enumerate so
    // the host drops its cached FAT and mounts whatever is in the drive.
    printf("media changed under a mounted volume: USB reconnect\n");
    host_has_media = false;
    acked_media_seq = 0;
    rd_cache.valid = false;
    tud_disconnect();
    reconnect_at = make_timeout_time_ms(MSC_RECONNECT_MS);
    reconnect_pending = true;
}

// USB bus reset or unplug: the host context that issued any request in
// flight is gone, and it will enumerate us afresh.
void msc_bus_reset(void) {
    MSC_TRACE("bus reset\n");
    bdev_host_reset();
    acked_media_seq = 0;
    host_has_media = false;
    rd_cache.valid = false;
}

void tud_umount_cb(void) {
    msc_bus_reset();
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer,
                          uint32_t bufsize) {
    if (media_unacked(lun)) return -1;
    bdev_media_state_t state = bdev_media_state();
    if (state != BDEV_MEDIA_READY) {
        set_media_sense(lun, state);
        return -1;
    }
    if (!rd_cache.valid || rd_cache.lba != lba ||
        rd_cache.media_seq != bdev_media_seq()) {
        int32_t status = bdev_read(lba, rd_cache.data);
        if (status == BDEV_AGAIN) return 0;
        if (status != BDEV_OK) {
            rd_cache.valid = false;
            set_bdev_sense(lun, status, SENSE_UNRECOVERED_READ, SENSE_LBA_OUT_OF_RANGE);
            return -1;
        }
        rd_cache.lba = lba;
        rd_cache.media_seq = bdev_media_seq();
        rd_cache.valid = true;
    }
    // msc_device derives the offset from the block size the host's CBW
    // implies (transfer length / block count): a CBW claiming blocks larger
    // than a sector puts it past the end. Refused, never read out of bounds.
    if (offset >= BDEV_SECTOR_SIZE) {
        set_sense(lun, SENSE_INVALID_CDB_FIELD);
        return -1;
    }
    uint32_t n = BDEV_SECTOR_SIZE - offset;
    if (n > bufsize) n = bufsize;
    memcpy(buffer, &rd_cache.data[offset], n);
    return (int32_t)n;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer,
                           uint32_t bufsize) {
    if (media_unacked(lun)) return -1;
    bdev_media_state_t state = bdev_media_state();
    if (state != BDEV_MEDIA_READY) {
        set_media_sense(lun, state);
        return -1;
    }
    if (offset == 0 && report_write_error(lun)) return -1;
    if (!bdev_writable()) {
        // Normally unreachable: msc_device gates WRITE(10) on is_writable_cb
        // and reports DATA PROTECT itself. This catches a WP tab flipped
        // mid-command.
        set_sense(lun, SENSE_WRITE_PROTECTED);
        return -1;
    }
    if (offset >= BDEV_SECTOR_SIZE) { // as in read10
        set_sense(lun, SENSE_INVALID_CDB_FIELD);
        return -1;
    }
    uint32_t n = BDEV_SECTOR_SIZE - offset;
    if (n > bufsize) n = bufsize;
    memcpy(&wr_buf[offset], buffer, n);
    if (offset + n >= BDEV_SECTOR_SIZE) {
        int32_t status = bdev_write(lba, wr_buf);
        if (status == BDEV_AGAIN) return 0; // TinyUSB retries with same args
        if (status != BDEV_OK) {
            set_bdev_sense(lun, status, SENSE_WRITE_FAULT, SENSE_LBA_OUT_OF_RANGE);
            return -1;
        }
        if (rd_cache.valid && rd_cache.lba == lba) rd_cache.valid = false;
    }
    return (int32_t)n;
}

//--------------------------------------------------------------------
// MODE SENSE(10) Flexible Disk page from the active media geometry
//--------------------------------------------------------------------

// Mode parameter header (MODE SENSE(10) format) + page 05h. Returns the
// length built, MODE_HEADER10_LEN + MODE_PAGE_FLEXIBLE_DISK_LEN.
static uint16_t build_mode_sense10_flexible(uint8_t *buf) {
    const uint16_t len = MODE_HEADER10_LEN + MODE_PAGE_FLEXIBLE_DISK_LEN;
    memset(buf, 0, len);
    // Header: mode data length excludes its own 2-byte length field
    buf[0] = (uint8_t)((len - 2) >> 8);
    buf[1] = (uint8_t)((len - 2) & 0xFF);
    buf[2] = 0x00; // medium type
    buf[3] = bdev_write_protected() ? 0x80 : 0x00; // WP bit

    uint16_t cylinders, rpm, kbps;
    uint8_t heads, spt;
    bdev_geometry(&cylinders, &heads, &spt, &rpm, &kbps);
    uint8_t *p = &buf[MODE_HEADER10_LEN];
    p[0] = MODE_PAGE_FLEXIBLE_DISK;
    p[1] = MODE_PAGE_FLEXIBLE_DISK_LEN - 2; // page length excludes code + length bytes
    p[2] = (uint8_t)(kbps >> 8);
    p[3] = (uint8_t)(kbps & 0xFF); // transfer rate in kbit/s
    p[4] = heads;
    p[5] = spt;
    p[6] = (uint8_t)(BDEV_SECTOR_SIZE >> 8);
    p[7] = (uint8_t)(BDEV_SECTOR_SIZE & 0xFF); // bytes per sector
    p[8] = (uint8_t)(cylinders >> 8);
    p[9] = (uint8_t)(cylinders & 0xFF);
    p[28] = (uint8_t)(rpm >> 8);
    p[29] = (uint8_t)(rpm & 0xFF);
    return len;
}

//--------------------------------------------------------------------
// READ FORMAT CAPACITIES (UFI 4.10)
//--------------------------------------------------------------------

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// Capacity List Header + Current/Maximum Capacity Descriptor + Formattable
// Capacity Descriptors. A 5.25" drive cannot tell HD from DD media (no
// sensor hole), so every capacity it can lay down is listed whenever a disk
// is in, formatted or not; the host picks. Returns the length built.
static uint16_t build_format_capacities(uint8_t *buf, uint16_t cap) {
    bdev_media_state_t state = bdev_media_state();
    uint32_t formattable[UFI_MAX_FORMATTABLE];
    unsigned n = state == BDEV_MEDIA_NONE
                     ? 0
                     : bdev_formattable(formattable, UFI_MAX_FORMATTABLE);
    uint16_t len = (uint16_t)(4 + 8 + 8 * n);
    if (len > cap) return 0;
    memset(buf, 0, len);
    buf[3] = (uint8_t)(8 + 8 * n); // capacity list length
    uint8_t *mcd = &buf[4];
    uint8_t code;
    uint32_t blocks;
    if (state == BDEV_MEDIA_READY) {
        code = UFI_CAP_FORMATTED;
        blocks = bdev_block_count();
    } else {
        code = state == BDEV_MEDIA_UNFORMATTED ? UFI_CAP_UNFORMATTED : UFI_CAP_NO_MEDIA;
        blocks = bdev_max_blocks();
    }
    put_be32(mcd, blocks);
    mcd[4] = code;
    mcd[6] = (uint8_t)(BDEV_SECTOR_SIZE >> 8);
    mcd[7] = (uint8_t)(BDEV_SECTOR_SIZE & 0xFF);
    for (unsigned i = 0; i < n; i++) {
        uint8_t *fcd = &buf[12 + 8 * i];
        put_be32(fcd, formattable[i]);
        fcd[6] = (uint8_t)(BDEV_SECTOR_SIZE >> 8);
        fcd[7] = (uint8_t)(BDEV_SECTOR_SIZE & 0xFF);
    }
    return len;
}

//--------------------------------------------------------------------
// FORMAT UNIT (UFI 4.1): one track per command
//--------------------------------------------------------------------

static int32_t scsi_format_unit(uint8_t lun, uint8_t const cdb[16], const uint8_t *list,
                                uint16_t list_len) {
    // FmtData=1, CmpList=0, Defect List Format=7, nothing else accepted.
    if ((cdb[1] & 0x1F) != 0x17) {
        set_sense(lun, SENSE_INVALID_CDB_FIELD);
        return -1;
    }
    uint16_t declared = (uint16_t)(cdb[7] << 8 | cdb[8]);
    if (declared != 12 || list_len < 12) {
        set_sense(lun, SENSE_PARAM_LIST_LENGTH);
        return -1;
    }
    uint8_t flags = list[1];
    uint16_t defect_len = (uint16_t)(list[2] << 8 | list[3]);
    bool immediate = (flags & 0x02) != 0;
    bool single_track = (flags & 0x10) != 0;
    uint8_t side = flags & 0x01;
    uint32_t blocks = get_be32(&list[4]);
    uint32_t block_len = (uint32_t)list[9] << 16 | (uint32_t)list[10] << 8 | list[11];
    if (immediate || !single_track || defect_len != 8 || block_len != BDEV_SECTOR_SIZE) {
        set_sense(lun, SENSE_INVALID_PARAM_FIELD);
        return -1;
    }
    if (media_unacked(lun)) return -1;
    if (report_write_error(lun)) return -1;
    if (bdev_media_state() == BDEV_MEDIA_NONE) {
        set_sense(lun, SENSE_MEDIUM_NOT_PRESENT);
        return -1;
    }
    MSC_TRACE("FORMAT UNIT track %u side %u, %lu blocks\n", cdb[2], side,
              (unsigned long)blocks);
    rd_cache.valid = false;
    int32_t r = bdev_format_track(blocks, cdb[2], side);
    if (r == BDEV_OK) return 0;
    // BDEV_EINVAL here is a capacity no format matches, or a track outside
    // it: a bad value in the parameter list, not an LBA.
    set_bdev_sense(lun, r, SENSE_FORMAT_FAILED, SENSE_INVALID_PARAM_FIELD);
    return -1;
}

//--------------------------------------------------------------------
// VERIFY(10): read the range, report the first sector that does not read
//--------------------------------------------------------------------

static uint8_t verify_buf[BDEV_SECTOR_SIZE];

static int32_t scsi_verify(uint8_t lun, uint8_t const cdb[16]) {
    if (media_unacked(lun)) return -1;
    bdev_media_state_t state = bdev_media_state();
    if (state != BDEV_MEDIA_READY) {
        set_media_sense(lun, state);
        return -1;
    }
    uint32_t lba = get_be32(&cdb[2]);
    uint32_t count = (uint32_t)(cdb[7] << 8 | cdb[8]);
    uint32_t blocks = bdev_block_count();
    if (lba > blocks || count > blocks - lba) { // no lba + count wrap-around
        set_sense(lun, SENSE_LBA_OUT_OF_RANGE);
        return -1;
    }
    for (uint32_t i = 0; i < count; i++) {
        int32_t status;
        // Same wait as bdev_format_track: the drive backend feeds the
        // watchdog from bdev_read while core 1 is alive; a dead backend
        // fails the command instead of spinning here forever (and the
        // main loop then lets the watchdog reset the chip).
        for (;;) {
            status = bdev_read(lba + i, verify_buf);
            if (status != BDEV_AGAIN) break;
            if (!bdev_healthy()) {
                status = BDEV_EIO;
                break;
            }
            best_effort_wfe_or_timeout(make_timeout_time_ms(2));
        }
        if (status == BDEV_OK) continue;
        set_bdev_sense(lun, status, SENSE_UNRECOVERED_READ, SENSE_LBA_OUT_OF_RANGE);
        return -1;
    }
    return 0;
}

// Returns a built reply truncated to the CDB allocation length (bytes 7-8 for
// both MODE SENSE(10) and READ FORMAT CAPACITIES) and to the class driver's
// buffer; the length fields inside the reply stay untouched (UFI 4.10).
static int32_t send_reply(uint8_t const cdb[16], void *buffer, uint16_t bufsize,
                          const uint8_t *reply, uint16_t len) {
    uint16_t alloc = (uint16_t)((cdb[7] << 8) | cdb[8]);
    if (len > alloc) len = alloc;
    if (len > bufsize) len = bufsize;
    memcpy(buffer, reply, len);
    return len;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    switch (scsi_cmd[0]) {
    case SCSI_CMD_MODE_SENSE_10: {
        uint8_t page = scsi_cmd[2] & 0x3F;
        if (page == MODE_PAGE_FLEXIBLE_DISK || page == MODE_PAGE_ALL) {
            uint8_t tmp[MODE_HEADER10_LEN + MODE_PAGE_FLEXIBLE_DISK_LEN];
            uint16_t len = build_mode_sense10_flexible(tmp);
            return send_reply(scsi_cmd, buffer, bufsize, tmp, len);
        }
        set_sense(lun, SENSE_INVALID_CDB_FIELD);
        return -1;
    }

    case SCSI_CMD_READ_FORMAT_CAPACITIES: {
        uint8_t tmp[4 + 8 + 8 * UFI_MAX_FORMATTABLE];
        uint16_t len = build_format_capacities(tmp, sizeof(tmp));
        return send_reply(scsi_cmd, buffer, bufsize, tmp, len);
    }

    case SCSI_CMD_FORMAT_UNIT:
        return scsi_format_unit(lun, scsi_cmd, buffer, bufsize);

    case SCSI_CMD_VERIFY_10:
        return scsi_verify(lun, scsi_cmd);

    case SCSI_CMD_SYNCHRONIZE_CACHE_10: {
        // After a swap, a write error still pending belongs to the old disk:
        // the host must hear about the new medium first (TEST UNIT READY
        // then raises the unit attention and drops that error).
        if (media_unacked(lun)) return -1;
        if (report_write_error(lun)) return -1;
        bdev_media_state_t state = bdev_media_state();
        if (state != BDEV_MEDIA_READY) {
            set_media_sense(lun, state);
            return -1;
        }
        // A write barrier: answer once the cached writes are on the medium.
        if (!bdev_flush_wait()) {
            set_sense(lun, SENSE_WRITE_FAULT);
            return -1;
        }
        return report_write_error(lun) ? -1 : 0;
    }

    default:
        set_sense(lun, SENSE_INVALID_COMMAND);
        return -1;
    }
}
