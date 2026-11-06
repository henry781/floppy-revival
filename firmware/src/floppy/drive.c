// blockdev backend over the real 5.25" drive.
//
// All mechanical I/O runs on core 1 so the USB device stack on core 0 stays
// responsive: core 0 posts single-sector requests through a one-deep mailbox
// and returns BDEV_AGAIN until core 1 reports completion. Core 1 also owns
// media detection (probing formats on inserted disks), the write-back cache
// flush on idle, read-ahead, FORMAT UNIT tracks and the motor idle timeout.
//
// Mailbox protocol notes:
//  - requests carry a sequence number; responses are matched on it, so a
//    completion can never be attributed to a later command with the same
//    (op, lba)
//  - if media readiness is lost (or the USB bus resets) while a request is
//    in flight, the request is poisoned and its eventual response discarded
//  - host-requested flushes are latched in flush_pending and retried from
//    bdev_task until the mailbox accepts them — they cannot be dropped
//
// Media states (see blockdev.h): NONE (empty drive), UNFORMATTED (a disk
// spins but none of the formats reads: the host is told so, and may format
// it) and READY. A disk is never mounted at a guessed density: a foreign or
// blank disk stays unformatted until the host formats it explicitly.

#include "blockdev.h"
#include "cache.h"
#include "disk.h"
#include "drive_internal.h"
#include "floppy.h"
#include "format.h"
#include "pins.h"

#include "hardware/adc.h"
#include "hardware/watchdog.h"
#if FLOPPY_DEBUG
#include "pico/bootrom.h"
#endif
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include <stdio.h>
#include <string.h>

#define DRIVE_PROBE_RETRY_MS 5000u
#define DRIVE_PROBE_BACKOFF_MS 30000u
#define DRIVE_PROBE_FAST_ATTEMPTS 3u
// After a disk is pulled, keep probing at the fast rate for this long so
// the replacement is picked up within seconds, not after the 30 s backoff.
#define DRIVE_PROBE_FAST_AFTER_LOSS_MS 60000u
#define DRIVE_DSKCHG_POLL_MS 500u
#define DRIVE_FLUSH_IDLE_MS 1500u
#define DRIVE_WP_POLL_MS 500u
// Read-ahead: after the host read the last sector of a track, fetch the
// next track while the spindle is still turning (-DFLOPPY_READ_AHEAD=0 off).
#ifndef FLOPPY_READ_AHEAD
#define FLOPPY_READ_AHEAD 1
#endif
// FORMAT fill byte (what DOS FORMAT leaves in every sector).
#define DRIVE_FORMAT_FILL 0xF6u

// Status LEDs (driven by core 0 only, from bdev_task and bdev_again).
// ACT: lit while a readable disk is mounted; flickers (dark first) during
// host requests and drive work, each burst held at least one phase so a
// cache hit is visible. STATUS (red): steady while unflushed writes are
// cached, 1 Hz on an unformatted/unreadable disk, 2 Hz after a write error
// on this medium, 4 Hz on a drive fault.
#define LED_ACT_PHASE_MS 60u
#define LED_SLOW_BLINK_MS 500u  // 1 Hz
#define LED_ERROR_BLINK_MS 250u // 2 Hz
#define LED_FAST_BLINK_MS 125u  // 4 Hz

enum {
    OP_READ = 1,
    OP_WRITE,
    OP_FLUSH,
    OP_FORMAT, // lba field: cylinder | head << 8 | blocks << 16
};

typedef struct {
    uint8_t op;
    uint8_t seq;
    uint32_t lba;
} drive_req_t;

// A response names its request by sequence number only: that is the one
// key bdev_drain matches on (an (op, lba) pair can recur, a seq in flight
// cannot), so echoing op and lba back would only invite a second, weaker
// match.
typedef struct {
    uint8_t seq;
    int8_t err; // disk_err_t
} drive_resp_t;

floppy_t drive;
static cache_t cache;
static track_t probe_track; // probe reads, FORMAT track scratch
static queue_t req_q;
static queue_t resp_q;
static uint8_t io_buf[BDEV_SECTOR_SIZE]; // handed off with the mailbox

// Cross-core state. Each variable has exactly one writing core, so plain
// volatile loads and stores are enough (no read-modify-write is ever
// shared). Written by core 1 only (core1_* functions, and drive_debug.c,
// which also runs on core 1), read by core 0 (bdev_* queries, LEDs):
//  - media_state, media_fmt: core1_set_media, core1_format_track; media_fmt
//    is stored before media_state behind a barrier, so a core-0 reader that
//    sees READY also sees its format
//  - media_seq: core1_set_media
//  - media_wp: core1_set_media (probe, unformatted report, media loss),
//    core1_idle (WPT poll while selected), core1_handle (after a request)
//  - media_dirty, write_error_sticky, write_errors, drive_busy, drive_fault
// Written by core 0, read by core 1: adapter_last_mv (debug status line).
// core1_beat_ms: see its own comment below.
volatile uint8_t media_state;
static volatile uint32_t media_seq;
static volatile bool media_wp;
static volatile bool media_dirty;
const floppy_format_t *volatile media_fmt;
static volatile bool drive_busy; // in a request or idle work (LED only)
// Write errors: core 1 counts them, core 0 reports each once (counter
// compare, so neither side ever overwrites the other's update).
static volatile uint32_t write_errors;
static volatile bool write_error_sticky; // on this medium; LED

enum {
    FAULT_NONE = 0,
    FAULT_UNREADABLE, // disk spins but does not read (MEDIA_UNFORMATTED)
    FAULT_DRIVE,      // TRK00 fault latched
};
static volatile uint8_t drive_fault;

// Core-0 view of the in-flight request.
static struct {
    bool busy;
    bool done;
    bool discard; // media flapped or bus reset while in flight; drop the response
    uint8_t op;
    uint8_t seq;
    uint32_t lba;
    int8_t err;
} cur;

static uint8_t req_seq;
static bool flush_pending;
static bool drive_ok;
static uint32_t write_errors_reported;

// Core-1 liveness for the watchdog. Bumped once per core1_entry iteration,
// and by floppy.c (drive.heartbeat) at the start of every drive operation
// and of every write attempt, so a flush of several tracks in one request
// beats once per track. The longest stretch without a beat is therefore one
// operation, which its own deadline bounds (FLOPPY_OPERATION_MAX_MS, 20 s,
// a write with its retries; every wait inside is checked against it, the
// overshoot is milliseconds): 2 s of margin on top. A hung core 1 is reset
// at most 22 s + the 8 s watchdog after its last beat. While a host command
// waits on core 1, tud_task does not return to the main loop (TinyUSB
// re-queues the command until read10/write10 stop returning 0), so
// bdev_again (every BDEV_AGAIN answer) feeds the watchdog itself;
// otherwise its 8 s timeout would cut such operations. Written through core1_beat only: by core 1, and once by
// core 0 in bdev_init before core 1 is launched; read by core 0
// (bdev_healthy).
static volatile uint32_t core1_beat_ms;
#define DRIVE_HEARTBEAT_STALE_MS (FLOPPY_OPERATION_MAX_MS + 2000u)

void core1_beat(void) {
    core1_beat_ms = to_ms_since_boot(get_absolute_time());
}

// After this many consecutive probe failures on TRK00, stop auto-probing:
// each failed recalibrate rams the head against the outward stop 90 times.
// Latched until power-cycle — check the TRK00 wiring (pin 26/GP2).
#define DRIVE_TRK00_FAULT_LIMIT 3u
static unsigned trk00_faults;
// Outcome of the last probe attempt (core 1 only). DISK_ERR_MEDIA_CHANGED
// doubles as "no disk" for the presence test after a failed probe: it is
// what a probe read returns while DSKCHG stays active (empty drive, or a
// disk swapped mid-probe), and the empty-drive quick check and a mount lost
// to a swap store it on purpose, so that core1_after_failed_probe never
// reports such a drive as holding an unreadable disk.
disk_err_t last_probe_err;
static unsigned failed_probes;
static uint32_t media_lost_ms; // 0: no media loss since boot
static uint32_t last_probe_ms; // last automatic probe attempt (core1_probe_due)

// A disk that spins but does not read (unformatted, damaged, foreign
// format) is left alone after DRIVE_PROBE_FAST_ATTEMPTS rounds: re-reading
// its track 0 every backoff period would only wear it. Probing resumes when
// DSKCHG reports another insertion. Meanwhile the host sees it as an
// unformatted medium and may FORMAT it.
static unsigned unreadable_probes;
static bool probe_parked;
static uint32_t parked_generation;
// Media generation the last probe read ran on, and the one of the medium
// reported to the host (READY or UNFORMATTED): what the cache is bound to
// and what a FORMAT UNIT may write on. A disk swapped in since then is
// another medium, never mounted or formatted on the strength of a read of
// its predecessor.
static uint32_t probe_generation;
static uint32_t reported_generation;

#if FLOPPY_READ_AHEAD
static bool prefetch_armed;
static uint8_t prefetch_cylinder;
static uint8_t prefetch_head;
#endif

#if FLOPPY_READONLY_PIN >= 0
static bool readonly_pin_ok; // J3 input set up (not colliding with another net)
#endif

// True when writes are administratively blocked: read-only jumper closed
// to GND, or a forced read-only build. Sampled live so the jumper can be
// changed between disks (with everything powered down). Called from both
// cores (a GPIO input read): core 0 refuses host writes and formats, core 1
// refuses to flush what was cached before the jumper closed (the cache sees
// it as write protection, see drive_device_write_protected). A J3 pin that
// collides with another net reads as closed: refusing writes is the safe
// failure.
static bool drive_forced_ro(void) {
#if FLOPPY_FORCE_READ_ONLY
    return true;
#elif FLOPPY_READONLY_PIN >= 0
    return !readonly_pin_ok || !gpio_get(FLOPPY_READONLY_PIN);
#else
    return false;
#endif
}

//--------------------------------------------------------------------
// Core 1: drive agent
//--------------------------------------------------------------------

static uint32_t core1_last_io_ms; // last request completion, for idle flush

// A medium appeared (readable or not): new sequence number for the host.
static void core1_set_media(uint8_t state, const floppy_format_t *fmt, bool wp) {
    media_fmt = fmt;
    media_wp = wp;
    __sync_synchronize();
    if (state != MEDIA_NONE && media_state == MEDIA_NONE) media_seq = media_seq + 1;
    if (state == MEDIA_READY && media_state == MEDIA_UNFORMATTED) media_seq = media_seq + 1;
    media_state = state;
}

static void core1_note_write_error(const char *why) {
    write_error_sticky = true;
    write_errors = write_errors + 1;
    printf("write error: %s\n", why);
}

// FAT boot sector total-sector count from a probed track 0, or 0 when the
// sector is missing or carries no plausible BPB (non-DOS, DOS 1.x).
static unsigned core1_bpb_total_sectors(const track_t *t) {
    if (!(t->valid & 1u)) return 0;
    const uint8_t *b = t->data[0];
    unsigned bytes_per_sector = b[11] | (unsigned)b[12] << 8;
    unsigned total = b[19] | (unsigned)b[20] << 8;
    if (bytes_per_sector != BDEV_SECTOR_SIZE || b[21] < 0xF0) return 0;
    return total;
}

// 720KB (96tpi DD) and 360KB media read identically on physical track 0.
// Called with track 0 read as 720KB: the BPB decides when there is one
// (and names the 360KB format directly, no second read of the same track);
// otherwise only a 96tpi disk has data on physical track 1.
static const floppy_format_t *core1_resolve_dd(const floppy_format_t *fmt,
                                               uint32_t generation) {
    unsigned total = core1_bpb_total_sectors(&probe_track);
    if (total == format_total_sectors(fmt)) return fmt;
    if (total != 0) {
        const floppy_format_t *other = format_by_block_count(total);
        if (other && other->cell_ticks == fmt->cell_ticks &&
            other->sectors_per_track == fmt->sectors_per_track &&
            floppy_set_format(&drive, other) == DISK_OK) {
            printf("probe %s: boot sector says %u sectors: %s\n", fmt->name, total,
                   other->name);
            return other;
        }
        printf("probe %s: boot sector says %u sectors, not this format\n",
               fmt->name, total);
        return NULL;
    }
    probe_track.cylinder = 1;
    probe_track.head = 0;
    disk_err_t status = floppy_probe_track(&drive, generation, &probe_track);
    printf("probe %s: no BPB, physical track 1: %d (%s)\n", fmt->name,
           (int)status, disk_err_name(status));
    return status == DISK_OK ? fmt : NULL;
}

static const floppy_format_t *core1_probe_formats(void) {
    for (unsigned i = 0; i < floppy_probe_candidate_count; i++) {
        const floppy_format_t *fmt = floppy_probe_candidates[i];
        if (floppy_set_format(&drive, fmt) != DISK_OK) return NULL;
        uint32_t generation;
        if (floppy_media_generation(&drive, &generation) != DISK_OK) return NULL;
        probe_generation = generation;
        memset(&probe_track, 0, sizeof(probe_track));
        probe_track.cylinder = 0;
        probe_track.head = 0;
        disk_err_t status = floppy_probe_track(&drive, generation, &probe_track);
        printf("probe %s: %d (%s)\n", fmt->name, (int)status, disk_err_name(status));
        last_probe_err = status;
        if (status == DISK_OK && fmt == &FORMAT_525_720K_HD_DRIVE) {
            const floppy_format_t *resolved = core1_resolve_dd(fmt, generation);
            if (!resolved) continue;
            return resolved;
        }
        if (status == DISK_OK) return fmt;
        if (status == DISK_ERR_MEDIA_CHANGED) return NULL; // retry next round
    }
    return NULL;
}

bool core1_try_probe(void) {
    const floppy_format_t *fmt = core1_probe_formats();
    if (!fmt) return false;
    bool wp = false;
    floppy_write_protected(&drive, &wp);
    disk_err_t bound = cache_bind(&cache, fmt, probe_generation);
    if (bound != DISK_OK) {
        // Swapped after the probe read: probe the new disk (next round),
        // and never call it unformatted on the old disk's read.
        printf("probe %s: medium changed before mount\n", fmt->name);
        last_probe_err = DISK_ERR_MEDIA_CHANGED;
        return false;
    }
    reported_generation = probe_generation;
    media_dirty = false;
    unreadable_probes = 0;
    probe_parked = false;
    write_error_sticky = false;
    // The probe read a whole track: hand it to the cache so the host's
    // first reads (boot sector, FAT) cost no revolution.
    if (probe_track.valid == track_valid_mask(fmt)) cache_seed(&cache, &probe_track);
    core1_set_media(MEDIA_READY, fmt, wp);
    printf("media: %s%s\n", fmt->name, wp ? " (write-protected)" : "");
    return true;
}

// Anything the cache dropped since the last look (sectors a track refused,
// dirty data lost with the medium or a format) becomes one host-visible
// write error.
static void core1_collect_write_faults(const char *why) {
    if (cache_take_write_fault(&cache)) core1_note_write_error(why);
}

static void core1_media_lost(void) {
    // Dirty data is usually already gone by now: cache_check cleared the
    // slots when it saw the new generation, and recorded the loss. A host
    // that still sends SYNCHRONIZE CACHE / eject for this medium gets a
    // medium error for the first of those, never GOOD. A mounted volume is
    // dropped by the USB reconnect instead (msc_task), and the pending error
    // is discarded at the next disk's UNIT ATTENTION: the host learns of the
    // loss from the volume vanishing unejected, not from a sense code.
    cache_clear(&cache);
    core1_collect_write_faults("media lost with unflushed writes, discarded");
    media_dirty = false;
#if FLOPPY_READ_AHEAD
    prefetch_armed = false;
#endif
    if (media_state != MEDIA_NONE) {
        printf("media removed or changed\n");
        failed_probes = 0;
        media_lost_ms = to_ms_since_boot(get_absolute_time());
        if (media_lost_ms == 0) media_lost_ms = 1;
    }
    probe_parked = false;
    unreadable_probes = 0;
    core1_set_media(MEDIA_NONE, NULL, false);
}

static void core1_check_media(void) {
    bool changed;
    if (floppy_disk_changed(&drive, &changed) == DISK_OK && changed) {
        core1_media_lost();
        return;
    }
    uint32_t generation;
    if (floppy_media_generation(&drive, &generation) == DISK_OK &&
        generation != cache.generation) {
        core1_media_lost();
    }
}

// DSKCHG sampling while the drive idles deselected, so a swap is seen
// without waiting for the next host request. One timestamp for both callers
// (a parked unreadable disk, a mounted one): they never run in the same
// idle pass, and either way the line is sampled every DRIVE_DSKCHG_POLL_MS.
// True when a sample was taken.
static bool core1_sample_dskchg_due(uint32_t now) {
    static uint32_t last_dskchg_ms;
    if ((uint32_t)(now - last_dskchg_ms) < DRIVE_DSKCHG_POLL_MS) return false;
    last_dskchg_ms = now;
    floppy_sample_disk_change(&drive);
    return true;
}

// A parked (unreadable) disk is only watched for a swap. True when another
// disk went in: the parked state is dropped and the probe runs right away.
static bool core1_parked_disk_changed(uint32_t now) {
    if (!core1_sample_dskchg_due(now)) return false;
    uint32_t generation;
    if (floppy_media_generation(&drive, &generation) != DISK_OK ||
        generation == parked_generation) {
        return false;
    }
    printf("disk changed: probing again\n");
    core1_media_lost();
    last_probe_ms = now - DRIVE_PROBE_RETRY_MS; // probe right away
    return true;
}

// Probe pacing: every DRIVE_PROBE_RETRY_MS for the first
// DRIVE_PROBE_FAST_ATTEMPTS misses and during the window after a media
// loss, every DRIVE_PROBE_BACKOFF_MS otherwise. True (and the attempt
// timestamped) when one is due.
static bool core1_probe_due(uint32_t now) {
    bool just_lost = media_lost_ms != 0 &&
                     (uint32_t)(now - media_lost_ms) < DRIVE_PROBE_FAST_AFTER_LOSS_MS;
    uint32_t interval = failed_probes >= DRIVE_PROBE_FAST_ATTEMPTS && !just_lost
                            ? DRIVE_PROBE_BACKOFF_MS
                            : DRIVE_PROBE_RETRY_MS;
    if ((uint32_t)(now - last_probe_ms) < interval) return false;
    last_probe_ms = now;
    return true;
}

// Empty drive: a short select (<= 2 ms) with one outward step pulse while
// the head sits at TRK00 (it stays there) tells whether a disk is in,
// without motor or seek. True when it proved the drive empty (a miss,
// counted like a failed probe). The full probe runs only once DSKCHG
// clears; any other result (head not home: NO_TRACK0, or BUSY) returns
// false on purpose, and the full probe then homes the head. A miss also
// proves TRK00 reads asserted, hence the trk00_faults reset.
static bool core1_quick_check_empty(void) {
    bool disk_in = false;
    if (floppy_quick_media_check(&drive, &disk_in) != DISK_OK || disk_in) return false;
    failed_probes++;
    unreadable_probes = 0;
    trk00_faults = 0;
    last_probe_err = DISK_ERR_MEDIA_CHANGED; // "no disk", see last_probe_err
    return true;
}

// A full probe mounted nothing: report a spinning disk as unformatted (and
// park it after a few rounds), drop an unformatted one that went away, and
// count TRK00 failures towards the latched fault.
static void core1_after_failed_probe(void) {
    failed_probes++;
    // Stable index pulses: a disk is in and spinning, it just does not read.
    bool present = drive.motor_qualified && last_probe_err != DISK_ERR_MEDIA_CHANGED;
    if (present) {
        bool wp = false;
        floppy_write_protected(&drive, &wp); // still selected: WPT is valid
        unreadable_probes++;
        if (media_state == MEDIA_NONE) {
            printf("disk present but unreadable: reported as unformatted\n");
            write_error_sticky = false;
        }
        reported_generation = probe_generation;
        core1_set_media(MEDIA_UNFORMATTED, NULL, wp);
        if (unreadable_probes >= DRIVE_PROBE_FAST_ATTEMPTS &&
            floppy_media_generation(&drive, &parked_generation) == DISK_OK) {
            probe_parked = true;
            printf("disk unreadable after %u probes: left alone until it is "
                   "changed (or formatted)\n", unreadable_probes);
        }
    } else {
        unreadable_probes = 0;
        if (media_state == MEDIA_UNFORMATTED) core1_media_lost();
    }
    if (last_probe_err == DISK_ERR_NO_TRACK0) {
        if (++trk00_faults >= DRIVE_TRK00_FAULT_LIMIT) {
            printf("TRK00 fault latched after %u recalibrate failures — check "
                   "pin 26/GP2 wiring (or the drive is unpowered: adapter? F1?); "
                   "reset the Pico to retry\n", trk00_faults);
        }
    } else {
        trk00_faults = 0;
    }
    // Don't leave the drive selected and spinning between attempts.
    floppy_motor_off(&drive);
    floppy_select(&drive, false);
}

// Probing of a drive without a mounted disk: empty, or a disk no format
// reads (reported unformatted, parked after a few rounds).
static void core1_probe_idle(uint32_t now) {
    if (trk00_faults >= DRIVE_TRK00_FAULT_LIMIT) return;
#if FLOPPY_DEBUG
    if (!drive_debug_auto_probe()) return;
#endif
    if (probe_parked && !core1_parked_disk_changed(now)) return;
    if (!core1_probe_due(now)) return;
    if (media_state == MEDIA_NONE && core1_quick_check_empty()) return;
    if (core1_try_probe()) {
        failed_probes = 0;
        trk00_faults = 0;
        return;
    }
    core1_after_failed_probe();
}

static void core1_idle(void) {
    static uint32_t last_wp_ms;
    uint32_t now = to_ms_since_boot(get_absolute_time());

    floppy_poll(&drive);

    if (media_state != MEDIA_READY) {
        core1_probe_idle(now);
        return;
    }

    if (!drive.selected) core1_sample_dskchg_due(now);
    core1_check_media();
    if (media_state != MEDIA_READY) return;

    if (drive.selected && (uint32_t)(now - last_wp_ms) >= DRIVE_WP_POLL_MS) {
        last_wp_ms = now;
        bool wp;
        if (floppy_write_protected(&drive, &wp) == DISK_OK) media_wp = wp;
    }

    if (cache_dirty(&cache) &&
        (uint32_t)(now - core1_last_io_ms) >= DRIVE_FLUSH_IDLE_MS) {
        drive_busy = true;
        disk_err_t status = cache_flush(&cache);
        drive_busy = false;
        if (status == DISK_ERR_MEDIA_CHANGED) core1_media_lost();
        core1_collect_write_faults("sectors dropped after repeated write failures");
        media_dirty = cache_dirty(&cache);
        core1_last_io_ms = to_ms_since_boot(get_absolute_time());
        return;
    }

#if FLOPPY_READ_AHEAD
    if (prefetch_armed && queue_is_empty(&req_q) && !cache_dirty(&cache)) {
        prefetch_armed = false;
        drive_busy = true;
        disk_err_t status = cache_prefetch(&cache, prefetch_cylinder, prefetch_head);
        drive_busy = false;
        if (status == DISK_ERR_MEDIA_CHANGED) core1_media_lost();
        core1_collect_write_faults("sectors dropped after repeated write failures");
        core1_last_io_ms = to_ms_since_boot(get_absolute_time());
    }
#endif
}

// FORMAT UNIT, one track. The first track of a format switches the medium
// to the requested geometry (UFI: "the UFI device resets the Flexible Disk
// pages to reflect the received Format Descriptor"); the host gets no UNIT
// ATTENTION for that, it asked for it.
static disk_err_t core1_format_track(uint8_t cylinder, uint8_t head,
                                     const floppy_format_t *fmt) {
    if (!fmt || media_state == MEDIA_NONE) return DISK_ERR_MEDIA_CHANGED;
    if (!disk_ch_valid(fmt, cylinder, head)) return DISK_ERR_INVALID;
    // Core 0 refused already; checked again where the write happens.
    if (drive_forced_ro()) return DISK_ERR_WRITE_PROTECTED;
    if (media_state != MEDIA_READY || media_fmt != fmt) {
        // Writes the host already got GOOD for land first, in the format
        // they were written in, as a drive would have done long before.
        if (media_state == MEDIA_READY && cache_dirty(&cache)) {
            disk_err_t flushed = cache_flush(&cache);
            if (flushed == DISK_ERR_MEDIA_CHANGED) return flushed;
        }
        if (floppy_set_format(&drive, fmt) != DISK_OK) return DISK_ERR_INVALID;
        if (cache_bind(&cache, fmt, reported_generation) != DISK_OK) {
            return DISK_ERR_MEDIA_CHANGED;
        }
        core1_collect_write_faults("unflushed writes discarded by a format");
        media_dirty = false;
        probe_parked = false;
        unreadable_probes = 0;
        media_fmt = fmt;
        __sync_synchronize();
        media_state = MEDIA_READY;
        printf("format: medium set to %s\n", fmt->name);
    }
    if (cylinder == 0 && head == 0) floppy_invalidate_position(&drive);
    track_t *t = &probe_track;
    memset(t, DRIVE_FORMAT_FILL, sizeof(*t));
    t->cylinder = cylinder;
    t->head = head;
    t->valid = track_valid_mask(fmt);
    disk_err_t err = floppy_format_track(&drive, cache.generation, t);
    printf("format C%u H%u: %s\n", cylinder, head, disk_err_name(err));
    if (err == DISK_OK) {
        cache_seed(&cache, t);
    } else {
        // A format that failed half-way leaves the cache's picture of this
        // track unknown, and the disk readable in no format: it is an
        // unformatted medium again (no new sequence number, the host asked
        // for this and gets FORMAT COMMAND FAILED).
        cache_clear(&cache);
        core1_collect_write_faults("unflushed writes discarded by a failed format");
        media_dirty = false;
        if (err != DISK_ERR_MEDIA_CHANGED && err != DISK_ERR_WRITE_PROTECTED &&
            err != DISK_ERR_INVALID) {
            media_fmt = NULL;
            __sync_synchronize();
            media_state = MEDIA_UNFORMATTED;
            unreadable_probes = DRIVE_PROBE_FAST_ATTEMPTS; // known unreadable
            floppy_media_generation(&drive, &parked_generation);
            probe_parked = true;
            printf("format failed: medium is unformatted\n");
        }
    }
    return err;
}

static void core1_handle(const drive_req_t *req) {
    drive_busy = true;
    disk_err_t err;
    if (req->op == OP_FORMAT) {
        err = core1_format_track((uint8_t)(req->lba & 0xFFu),
                                 (uint8_t)((req->lba >> 8) & 0xFFu),
                                 format_by_block_count(req->lba >> 16));
        if (err == DISK_ERR_MEDIA_CHANGED) core1_media_lost();
    } else if (media_state != MEDIA_READY) {
        err = DISK_ERR_MEDIA_CHANGED;
    } else {
        switch (req->op) {
        case OP_READ:
            err = cache_read(&cache, (uint16_t)req->lba, io_buf);
#if FLOPPY_READ_AHEAD
            if (err == DISK_OK) {
                const floppy_format_t *fmt = media_fmt;
                uint8_t c, h, s;
                if (fmt && disk_lba_to_chs(fmt, (uint16_t)req->lba, &c, &h, &s) &&
                    s == fmt->sectors_per_track - 1u &&
                    (uint32_t)req->lba + 1u < format_total_sectors(fmt)) {
                    // Last sector of a track read: the next track is likely.
                    disk_lba_to_chs(fmt, (uint16_t)(req->lba + 1u), &prefetch_cylinder,
                                    &prefetch_head, &s);
                    prefetch_armed = !cache_has_track(&cache, prefetch_cylinder,
                                                      prefetch_head);
                }
            }
#endif
            break;
        case OP_WRITE:
            err = cache_writable(&cache);
            if (err == DISK_OK) {
                err = cache_write(&cache, (uint16_t)req->lba, io_buf);
            }
            break;
        case OP_FLUSH:
            err = cache_flush(&cache);
            break;
        default:
            err = DISK_ERR_INVALID;
            break;
        }
        if (err == DISK_ERR_MEDIA_CHANGED) core1_media_lost();
        // An eviction inside a read or write can drop a victim's sectors too.
        core1_collect_write_faults("sectors dropped after repeated write failures");
    }
    if (media_state == MEDIA_READY) {
        media_dirty = cache_dirty(&cache);
        bool wp;
        if (drive.selected && floppy_write_protected(&drive, &wp) == DISK_OK) media_wp = wp;
    }
    drive_busy = false;
    drive_resp_t resp = {.seq = req->seq, .err = (int8_t)err};
    core1_last_io_ms = to_ms_since_boot(get_absolute_time());
    // The mailbox protocol guarantees resp_q has room (one request in
    // flight), but never block core 1 unconditionally: a wedged core 0 must
    // not be able to pin the motor on by freezing us mid-add.
    while (!queue_try_add(&resp_q, &resp)) {
        floppy_poll(&drive);
        sleep_ms(1);
    }
}

static void core1_entry(void) {
#if FLOPPY_DEBUG
    drive_debug_init();
#endif
    for (;;) {
        core1_beat();
#if FLOPPY_DEBUG
        drive_debug_poll();
#endif
        drive_req_t req;
        if (queue_try_remove(&req_q, &req)) {
            core1_handle(&req);
        } else {
            core1_idle();
            drive_fault = trk00_faults >= DRIVE_TRK00_FAULT_LIMIT ? FAULT_DRIVE
                          : media_state == MEDIA_UNFORMATTED      ? FAULT_UNREADABLE
                                                                  : FAULT_NONE;
            // queue_try_add on core 0 signals an event (SEV), so a new
            // request ends this wait at once instead of after up to 2 ms.
            best_effort_wfe_or_timeout(make_timeout_time_ms(2));
        }
    }
}

//--------------------------------------------------------------------
// Core 0: blockdev facade
//--------------------------------------------------------------------

#if FLOPPY_LED_ACTIVITY_PIN >= 0 || FLOPPY_LED_DIRTY_PIN >= 0 || \
    FLOPPY_DRIVE_POWER_PIN >= 0 || FLOPPY_READONLY_PIN >= 0
// An auxiliary pin (LED, drive power, J3) that collides with a drive-interface
// GPIO would drive the bus (a mis-edited pins.h is enough); leave it
// un-inited instead — gpio_put on a pin never routed to SIO has no
// electrical effect. Checks the pins the drive was actually initialized
// with, plus UART0 (GP0/GP1 debug stdio), the heartbeat LED, and `other`.
static bool aux_pin_free(int pin, int other) {
    const floppy_pins_t *p = &drive.pins;
    const uint8_t used[] = {p->index,        p->track0,    p->write_protect,
                            p->read_data,    p->write_data, p->write_gate,
                            p->motor_enable, p->drive_select, p->direction,
                            p->step,         p->side_select, p->disk_change,
                            p->density};
    bool taken = pin == 0 || pin == 1 || pin == other;
#ifdef PICO_DEFAULT_LED_PIN
    taken = taken || pin == PICO_DEFAULT_LED_PIN;
#endif
    for (size_t i = 0; !taken && i < sizeof used; i++) {
        taken = (int)used[i] == pin;
    }
    if (taken) printf("aux pin %d collides with another net, disabled\n", pin);
    return !taken;
}
#endif

// The cache's view of write protection: the disk's notch, plus J3 and the
// read-only build. Dirty data cached before J3 closed is then never
// written: it goes through the cache's failure policy (dropped after the
// flush attempts, reported once as a write error), like data the notch
// keeps out.
static disk_err_t drive_device_write_protected(void *ctx, bool *write_protected) {
    disk_err_t status = floppy_write_protected(ctx, write_protected);
    if (status == DISK_OK && drive_forced_ro()) *write_protected = true;
    return status;
}

static disk_device_t drive_device(void) {
    disk_device_t device = floppy_device(&drive);
    device.write_protected = drive_device_write_protected;
#if FLOPPY_FORCE_READ_ONLY
    // No write path at all for the cache (cache_write and cache_writable
    // refuse on a NULL write_sectors).
    device.write_sectors = NULL;
#endif
    return device;
}

#if FLOPPY_DRIVE_POWER_PIN >= 0
// ---- drive power: GP18 and the adapter sense (rev C) ------------------
#define DRIVE_POWER_POLL_MS 50u
#define ADAPTER_ADC_SAMPLES 8u // averaged per reading
static bool power_pin_ok;
static uint32_t power_off_ms; // when GP18 was last driven low
volatile uint32_t adapter_last_mv;

// Adapter voltage in millivolts from ADC0 through the R31/R32 divider;
// UINT32_MAX (treated as present) when the board has no sense pin (rev B:
// GP18 is then driven unconditionally).
static uint32_t adapter_mv(void) {
#if FLOPPY_ADAPTER_SENSE_PIN >= 0
    uint32_t sum = 0;
    for (unsigned i = 0; i < ADAPTER_ADC_SAMPLES; i++) sum += adc_read();
    // 12-bit against the 3.3V reference, then the divider ratio
    uint32_t mv = (sum / ADAPTER_ADC_SAMPLES) * 3300u / 4095u *
                  FLOPPY_ADAPTER_DIVIDER_X1000 / 1000u;
    adapter_last_mv = mv;
    return mv;
#else
    return UINT32_MAX;
#endif
}

static void adapter_sense_init(void) {
#if FLOPPY_ADAPTER_SENSE_PIN >= 0
    adc_init();
    adc_gpio_init(FLOPPY_ADAPTER_SENSE_PIN);
    adc_select_input(FLOPPY_ADAPTER_SENSE_PIN - 26);
#endif
}

static void drive_power_set(bool on, uint32_t mv) {
    gpio_put(FLOPPY_DRIVE_POWER_PIN, on);
    if (!on) power_off_ms = to_ms_since_boot(get_absolute_time());
    printf("drive power %s (GP%d, adapter %lu mV)\n", on ? "on" : "OFF", FLOPPY_DRIVE_POWER_PIN,
           (unsigned long)mv);
}

// Every DRIVE_POWER_POLL_MS from core 0 (main loop and command waits,
// core0_background): drop GP18 the moment the adapter goes away (the
// regulators stop within microseconds, the bus then bleeds unloaded), and
// only re-enable with the adapter back AND the full
// FLOPPY_DRIVE_POWER_CYCLE_MS hold elapsed, so the rails always restart
// through Q1's soft ramp. Core 1 keeps running: a drive that lost power
// just fails its next operation and re-probes like after a disk swap.
static void drive_power_poll(void) {
#if FLOPPY_ADAPTER_SENSE_PIN >= 0
    static uint32_t next_ms;
    if (!power_pin_ok) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now - next_ms) < 0) return;
    next_ms = now + DRIVE_POWER_POLL_MS;
    uint32_t mv = adapter_mv();
    bool on = gpio_get_out_level(FLOPPY_DRIVE_POWER_PIN);
    if (on && mv < FLOPPY_ADAPTER_OFF_MV) {
        drive_power_set(false, mv);
    } else if (!on && mv >= FLOPPY_ADAPTER_ON_MV && now - power_off_ms >= FLOPPY_DRIVE_POWER_CYCLE_MS) {
        drive_power_set(true, mv);
    }
#endif
}
#endif // FLOPPY_DRIVE_POWER_PIN

void bdev_init(void) {
    queue_init(&req_q, sizeof(drive_req_t), 1);
    queue_init(&resp_q, sizeof(drive_resp_t), 1);
    disk_err_t status = floppy_init(&drive, FLOPPY_PINS_DEFAULT);
    drive_ok = status == DISK_OK;
    drive.heartbeat = core1_beat;
    printf("floppy_init: %d\n", (int)status);
#if FLOPPY_LED_ACTIVITY_PIN >= 0
    if (aux_pin_free(FLOPPY_LED_ACTIVITY_PIN, FLOPPY_LED_DIRTY_PIN)) {
        gpio_init(FLOPPY_LED_ACTIVITY_PIN);
        gpio_set_dir(FLOPPY_LED_ACTIVITY_PIN, GPIO_OUT);
    }
#endif
#if FLOPPY_LED_DIRTY_PIN >= 0
    if (aux_pin_free(FLOPPY_LED_DIRTY_PIN, -1)) {
        gpio_init(FLOPPY_LED_DIRTY_PIN);
        gpio_set_dir(FLOPPY_LED_DIRTY_PIN, GPIO_OUT);
    }
#endif
#if FLOPPY_READONLY_PIN >= 0
    // Read-only jumper: closed to GND = no writes reach the drive,
    // whatever the host or the disk notch say.
    readonly_pin_ok = aux_pin_free(FLOPPY_READONLY_PIN, -1);
    if (readonly_pin_ok) {
        gpio_init(FLOPPY_READONLY_PIN);
        gpio_set_dir(FLOPPY_READONLY_PIN, GPIO_IN);
        gpio_pull_up(FLOPPY_READONLY_PIN);
    } else {
        printf("read-only jumper unusable: all writes refused\n");
    }
#endif
#if FLOPPY_DRIVE_POWER_PIN >= 0
    // Drive rails on only now: floppy_init has already released every
    // interface line (REDWC's parked level aside), so the drive never sees
    // an asserted bus at power-up. A reset returns the pad to hi-Z and the
    // regulators stop at once (ON/OFF pins), but the input bulk behind Q1
    // takes ~2.6 s to bleed below the regulators' lockout, far longer than
    // a reboot (watchdog, picotool, RUN tap): hold the pin low first on
    // every boot (FLOPPY_DRIVE_POWER_CYCLE_MS, pins.h), so the drive is
    // fully unpowered and gets a real power-on reset instead of a brown-out,
    // and the rails restart through Q1's soft ramp instead of a hard enable
    // into a charged input (overshoot). Suspected on the YD-380B: after
    // quick reboots it came back with DSKCHG clear (the spec says set at
    // power-on), and those sessions missed idle disk swaps.
    if (drive_ok && aux_pin_free(FLOPPY_DRIVE_POWER_PIN, -1)) {
        gpio_init(FLOPPY_DRIVE_POWER_PIN);
        gpio_put(FLOPPY_DRIVE_POWER_PIN, 0);
        gpio_set_dir(FLOPPY_DRIVE_POWER_PIN, GPIO_OUT);
        power_pin_ok = true;
        printf("drive power held off %d ms%s\n", FLOPPY_DRIVE_POWER_CYCLE_MS,
               watchdog_caused_reboot() ? " (watchdog reset)" : "");
        // ADC pad first: until adc_gpio_init the reset pull-down on GP26
        // drains C24 to about half the divider voltage (8.4 V read for a
        // 19.9 V adapter on the bench), and the divider (34.9k || 100nF,
        // tau 3.5 ms) needs ~20 ms to recover. The power hold covers that.
        adapter_sense_init();
        sleep_ms(FLOPPY_DRIVE_POWER_CYCLE_MS);
        uint32_t mv = adapter_mv();
        if (mv >= FLOPPY_ADAPTER_ON_MV) {
            drive_power_set(true, mv);
            sleep_ms(FLOPPY_DRIVE_POWER_SETTLE_MS);
        } else {
            // No adapter yet: the drive stays off, drive_power_poll switches it
            // on when the adapter appears (the drive then probes a little late).
            power_off_ms = to_ms_since_boot(get_absolute_time()) - FLOPPY_DRIVE_POWER_CYCLE_MS;
            printf("adapter not detected (%lu mV): drive stays off\n", (unsigned long)mv);
        }
    } else if (drive_ok) {
        printf("drive power pin %d unusable — drive stays UNPOWERED\n",
               FLOPPY_DRIVE_POWER_PIN);
    }
#endif
    if (drive_ok && cache_init(&cache, drive_device()) != DISK_OK) {
        // Only a device without a read/generation/protection hook is
        // refused: a build error, but the drive must then stay unused.
        printf("cache_init failed: drive disabled\n");
        drive_ok = false;
    }
    if (drive_ok) {
        core1_beat(); // fresh heartbeat before the watchdog first looks
        multicore_launch_core1(core1_entry);
    }
}

static void bdev_drain(void) {
    drive_resp_t resp;
    while (queue_try_remove(&resp_q, &resp)) {
        if (!cur.busy || resp.seq != cur.seq) continue;
        if (cur.discard) {
            cur.busy = false;
            cur.discard = false;
        } else {
            cur.done = true;
            cur.err = resp.err;
        }
    }
}

static bool bdev_post(uint8_t op, uint32_t lba) {
    drive_req_t req = {.op = op, .seq = (uint8_t)(req_seq + 1u), .lba = lba};
    if (!queue_try_add(&req_q, &req)) return false;
    req_seq = req.seq;
    cur.busy = true;
    cur.done = false;
    cur.discard = false;
    cur.op = op;
    cur.seq = req.seq;
    cur.lba = lba;
    return true;
}

static void bdev_update_leds(void) {
#if FLOPPY_LED_ACTIVITY_PIN >= 0 || FLOPPY_LED_DIRTY_PIN >= 0
    uint32_t now = to_ms_since_boot(get_absolute_time());
#endif
#if FLOPPY_LED_ACTIVITY_PIN >= 0
    // cur.busy stays set until bdev_submit collects the response, so even a
    // sub-millisecond cache hit is seen here at least once.
    static bool act_burst;
    static uint32_t act_start_ms, act_last_ms;
    if (cur.busy || drive_busy) {
        if (!act_burst) act_start_ms = now;
        act_burst = true;
        act_last_ms = now;
    } else if (act_burst && (uint32_t)(now - act_last_ms) >= LED_ACT_PHASE_MS &&
               ((now - act_start_ms) / LED_ACT_PHASE_MS) % 2 == 1) {
        act_burst = false; // end on a lit phase: every burst shows one dark
    }
    bool dark = act_burst && ((now - act_start_ms) / LED_ACT_PHASE_MS) % 2 == 0;
    gpio_put(FLOPPY_LED_ACTIVITY_PIN, bdev_media_ready() && !dark);
#endif
#if FLOPPY_LED_DIRTY_PIN >= 0
    bool red;
    if (!drive_ok || drive_fault == FAULT_DRIVE) {
        red = (now / LED_FAST_BLINK_MS) % 2 == 0;
    } else if (write_error_sticky) {
        red = (now / LED_ERROR_BLINK_MS) % 2 == 0;
    } else if (drive_fault == FAULT_UNREADABLE) {
        red = (now / LED_SLOW_BLINK_MS) % 2 == 0;
    } else {
        red = bdev_dirty(); // "don't pull the disk yet"
    }
    gpio_put(FLOPPY_LED_DIRTY_PIN, red);
#endif
}

// Core-0 duties that must not wait for a host command to finish: TinyUSB
// re-calls read10/write10 inside tud_task while core 1 works (bdev_again),
// so a command can hold core 0 there for up to FLOPPY_OPERATION_MAX_MS.
static void core0_background(void) {
#if FLOPPY_DEBUG
    if (drive_debug_bootsel_request) {
        sleep_ms(50); // let the UART line out
        reset_usb_boot(0, 0);
    }
#if FLOPPY_DRIVE_POWER_PIN >= 0
    if (drive_debug_power_cycle_request) {
        // 'P': drop the drive rails; drive_power_poll restores them after
        // the usual FLOPPY_DRIVE_POWER_CYCLE_MS hold.
        drive_debug_power_cycle_request = false;
        if (power_pin_ok && gpio_get_out_level(FLOPPY_DRIVE_POWER_PIN)) {
            drive_power_set(false, adapter_last_mv);
        }
    }
#endif
#endif
#if FLOPPY_DRIVE_POWER_PIN >= 0
    drive_power_poll();
#endif
}

void bdev_task(void) {
    core0_background();
    bdev_drain();
    bdev_update_leds();
    if (cur.busy && cur.done && cur.op == OP_FLUSH) {
        // Fire-and-forget flush completed.
        cur.busy = false;
        if (cur.err == (int8_t)DISK_OK) flush_pending = false;
    }
    if (flush_pending) {
        if (!bdev_media_ready()) {
            flush_pending = false; // nothing left to flush to
        } else if (!cur.busy) {
            bdev_post(OP_FLUSH, 0);
        }
    }
}

bdev_media_state_t bdev_media_state(void) {
    if (!drive_ok) return BDEV_MEDIA_NONE;
    switch (media_state) {
    case MEDIA_READY:
        return BDEV_MEDIA_READY;
    case MEDIA_UNFORMATTED:
        return BDEV_MEDIA_UNFORMATTED;
    default:
        return BDEV_MEDIA_NONE;
    }
}

bool bdev_media_ready(void) {
    return drive_ok && media_state == MEDIA_READY;
}

uint32_t bdev_media_seq(void) {
    return media_seq;
}

bool bdev_writable(void) {
    return bdev_media_ready() && !media_wp && !drive_forced_ro();
}

bool bdev_write_protected(void) {
    return bdev_media_state() != BDEV_MEDIA_NONE && (media_wp || drive_forced_ro());
}

bool bdev_healthy(void) {
    if (!drive_ok) return true; // core 1 never launched, nothing to monitor
    uint32_t age = to_ms_since_boot(get_absolute_time()) - core1_beat_ms;
    return age < DRIVE_HEARTBEAT_STALE_MS;
}

bool bdev_dirty(void) {
    return bdev_media_ready() && (media_dirty || flush_pending ||
                                  (cur.busy && cur.op == OP_FLUSH));
}

bool bdev_take_write_error(void) {
    uint32_t count = write_errors;
    if (count == write_errors_reported) return false;
    write_errors_reported = count;
    return true;
}

uint32_t bdev_block_count(void) {
    const floppy_format_t *fmt = media_fmt;
    return bdev_media_ready() && fmt ? format_total_sectors(fmt) : 0;
}

void bdev_geometry(uint16_t *cylinders, uint8_t *heads, uint8_t *spt,
                   uint16_t *rpm, uint16_t *kbps) {
    const floppy_format_t *fmt = media_fmt;
    if (!fmt) fmt = floppy_probe_candidates[0];
    *cylinders = fmt->cylinders;
    *heads = fmt->heads;
    *spt = fmt->sectors_per_track;
    *rpm = fmt->rpm;
    *kbps = fmt->data_rate_kbps;
}

unsigned bdev_formattable(uint32_t blocks[], unsigned max) {
    unsigned n = 0;
    for (unsigned i = 0; i < floppy_probe_candidate_count && n < max; i++) {
        uint32_t total = format_total_sectors(floppy_probe_candidates[i]);
        bool dup = false;
        for (unsigned j = 0; j < n; j++) dup = dup || blocks[j] == total;
        if (!dup) blocks[n++] = total;
    }
    // Largest first (insertion sort, three entries).
    for (unsigned i = 1; i < n; i++) {
        for (unsigned j = i; j > 0 && blocks[j] > blocks[j - 1]; j--) {
            uint32_t t = blocks[j];
            blocks[j] = blocks[j - 1];
            blocks[j - 1] = t;
        }
    }
    return n;
}

uint32_t bdev_max_blocks(void) {
    return format_max_blocks();
}

// A host command is waiting on core 1: do the main-loop duties tud_task is
// keeping from running (watchdog on the core-1 heartbeat, LED patterns).
static int32_t bdev_again(void) {
    core0_background();
    if (bdev_healthy()) watchdog_update();
    bdev_update_leds();
    return BDEV_AGAIN;
}

static int32_t bdev_submit(uint8_t op, uint32_t lba, const uint8_t *in,
                           uint8_t *out) {
    bool allowed = op == OP_FORMAT ? bdev_media_state() != BDEV_MEDIA_NONE
                                   : bdev_media_ready();
    if (!allowed) {
        // Poison any in-flight request: its completion must not be handed to
        // a later command that happens to reuse the same (op, lba).
        if (cur.busy) cur.discard = true;
        return BDEV_ENOMEDIUM;
    }
    if (op != OP_FORMAT && lba >= bdev_block_count()) return BDEV_EINVAL;
    bdev_drain();
    if (cur.busy && !(cur.op == op && cur.lba == lba && !cur.discard)) {
        // A different request (e.g. an async flush) still owns the mailbox.
        if (cur.done) cur.busy = false;
        return bdev_again();
    }
    if (!cur.busy) {
        if (in) memcpy(io_buf, in, BDEV_SECTOR_SIZE);
        bdev_post(op, lba);
        return bdev_again();
    }
    if (!cur.done) return bdev_again();
    cur.busy = false;
    switch ((disk_err_t)cur.err) {
    case DISK_OK:
        if (out) memcpy(out, io_buf, BDEV_SECTOR_SIZE);
        return BDEV_OK;
    case DISK_ERR_MEDIA_CHANGED:
        return BDEV_ENOMEDIUM;
    case DISK_ERR_WRITE_PROTECTED:
        return BDEV_EROFS;
    default:
        // DISK_ERR_INVALID from core 1 included: core 0 already refused an
        // out-of-range LBA (the only BDEV_EINVAL the host may see), so an
        // invalid request below is an internal fault, reported as I/O.
        return BDEV_EIO;
    }
}

// What each transfer may report (blockdev.h); anything else is a medium
// error. A read never reports BDEV_EROFS: it can only come from evicting a
// dirty victim track a write protection keeps out (notch, J3), which says
// nothing about the sector the host asked for. BDEV_EINVAL (LBA beyond the
// medium) is the host's mistake, not the disk's: the MSC layer turns it
// into ILLEGAL REQUEST instead of a medium error.
int32_t bdev_read(uint32_t lba, uint8_t *buf) {
    int32_t r = bdev_submit(OP_READ, lba, NULL, buf);
    switch (r) {
    case BDEV_OK:
    case BDEV_AGAIN:
    case BDEV_ENOMEDIUM:
    case BDEV_EINVAL:
        return r;
    default:
        return BDEV_EIO;
    }
}

int32_t bdev_write(uint32_t lba, const uint8_t *buf) {
    if (drive_forced_ro()) return BDEV_EROFS;
    int32_t r = bdev_submit(OP_WRITE, lba, buf, NULL);
    switch (r) {
    case BDEV_OK:
    case BDEV_AGAIN:
    case BDEV_ENOMEDIUM:
    case BDEV_EROFS:
    case BDEV_EINVAL:
        return r;
    default:
        return BDEV_EIO;
    }
}

void bdev_flush_async(void) {
    // Nothing cached, nothing to post: an empty flush would keep
    // bdev_dirty() true for a whole drive operation. media_dirty is current
    // here: core 1 updates it before answering each write.
    if (!bdev_media_ready() || !media_dirty) return;
    flush_pending = true;
    bdev_task(); // try to post immediately if the mailbox is free
}

bool bdev_flush_wait(void) {
    bdev_flush_async();
    while (bdev_dirty()) {
        if (!bdev_healthy()) return false;
        bdev_task(); // collects the flush, posts it again after a failure
        (void)bdev_again();
        best_effort_wfe_or_timeout(make_timeout_time_ms(2));
    }
    return true;
}

int32_t bdev_format_track(uint32_t blocks, uint8_t cylinder, uint8_t head) {
    const floppy_format_t *fmt = format_by_block_count(blocks);
    if (!fmt || blocks > 0xFFFFu || !disk_ch_valid(fmt, cylinder, head)) return BDEV_EINVAL;
    if (bdev_media_state() == BDEV_MEDIA_NONE) return BDEV_ENOMEDIUM;
    if (media_wp || drive_forced_ro()) return BDEV_EROFS;
    uint32_t arg = (uint32_t)cylinder | (uint32_t)head << 8 | blocks << 16;
    // The host waits for this command's status: block here, keeping the
    // watchdog and LEDs alive, until core 1 is done (its own deadline bounds
    // the wait).
    for (;;) {
        int32_t r = bdev_submit(OP_FORMAT, arg, NULL, NULL);
        if (r != BDEV_AGAIN) return r;
        if (!bdev_healthy()) return BDEV_EIO;
        best_effort_wfe_or_timeout(make_timeout_time_ms(2));
    }
}

void bdev_host_reset(void) {
    bdev_drain();
    if (cur.busy && !cur.done) cur.discard = true;
    else cur.busy = false;
}
