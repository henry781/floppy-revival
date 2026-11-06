// Block-device facade between the USB MSC callbacks and a storage backend.
//
// Two implementations exist, selected at link time:
//   - blockdev_vdisk.c: the RAM FAT12 image (Milestone 0, no hardware)
//   - floppy/drive.c:   the real 5.25" drive, running on core 1
//
// Read/write are asynchronous-friendly: BDEV_AGAIN means "not done yet, call
// again with the same arguments" and maps to TinyUSB's return-0 retry
// contract, so multi-second mechanical floppy I/O never blocks tud_task().

#ifndef BLOCKDEV_H_
#define BLOCKDEV_H_

#include <stdbool.h>
#include <stdint.h>

#define BDEV_SECTOR_SIZE 512u
#define BDEV_OK 0
#define BDEV_EIO (-1)          // medium error (unreadable, write/verify failed)
#define BDEV_AGAIN (-2)        // not done yet, call again
#define BDEV_ENOMEDIUM (-3)    // no disk in the drive (or it was pulled)
#define BDEV_EROFS (-4)        // write-protected (notch, jumper, build)
#define BDEV_EINVAL (-5)       // bad argument (capacity, track)

typedef enum {
    BDEV_MEDIA_NONE = 0,    // empty drive
    BDEV_MEDIA_UNFORMATTED, // disk spinning, none of the formats reads
    BDEV_MEDIA_READY,       // format known, sectors available
} bdev_media_state_t;

void bdev_init(void);
// Housekeeping from the core-0 main loop (drains async completions).
void bdev_task(void);
// False when the backend looks locked up (drive backend: core-1 heartbeat
// stale). The main loop stops feeding the watchdog on false so a hang mid-
// operation resets the chip and releases the drive lines.
bool bdev_healthy(void);

bdev_media_state_t bdev_media_state(void);
bool bdev_media_ready(void);
// Increments every time a medium is detected (readable or not); the MSC
// layer raises a UNIT ATTENTION when it changes, even if the not-ready
// window between two disks was too short for the host to observe.
uint32_t bdev_media_seq(void);
bool bdev_writable(void);
// The medium (readable or not) refuses writes: notch, jumper or build. What
// the WP bits of the mode pages report; an unformatted disk that is not
// protected must not look protected, or hosts refuse to format it.
bool bdev_write_protected(void);
// True while the write-back cache holds data not yet on the medium.
bool bdev_dirty(void);
// A write did not reach the medium: dirty sectors dropped after repeated
// failures, or discarded because the medium vanished. True once per such
// event (the MSC layer reports it on the next write-related command).
bool bdev_take_write_error(void);
uint32_t bdev_block_count(void); // 0 when no media
void bdev_geometry(uint16_t *cylinders, uint8_t *heads, uint8_t *spt,
                   uint16_t *rpm, uint16_t *kbps);
// Capacities this device can lay down, in 512-byte blocks, largest first.
// Returns how many were stored (at most `max`).
unsigned bdev_formattable(uint32_t blocks[], unsigned max);
uint32_t bdev_max_blocks(void);

// Transfer one full sector. Return BDEV_OK, BDEV_AGAIN (call again with the
// same arguments), BDEV_ENOMEDIUM, BDEV_EROFS (write only), BDEV_EINVAL (LBA
// beyond the medium) or BDEV_EIO.
int32_t bdev_read(uint32_t lba, uint8_t *buf);
int32_t bdev_write(uint32_t lba, const uint8_t *buf);

// Request a write-back flush of the cached writes; returns immediately, and
// does nothing when no write is cached.
void bdev_flush_async(void);
// Flush and wait until no write is cached: written, given up on after the
// retry policy (then bdev_take_write_error reports it), or the medium gone.
// Keeps the watchdog and LEDs alive. False only if the drive side stopped
// answering, with the writes still cached.
bool bdev_flush_wait(void);

// UFI FORMAT UNIT, one track: lay down `cylinder`/`head` for the format
// whose capacity is `blocks`, then read it back. Blocks until done (the
// host is waiting for this very command), feeding the watchdog. The medium
// takes that format from the first track on. Returns BDEV_OK, BDEV_EIO,
// BDEV_ENOMEDIUM, BDEV_EROFS or BDEV_EINVAL.
int32_t bdev_format_track(uint32_t blocks, uint8_t cylinder, uint8_t head);

// The USB bus was reset: a request in flight belongs to a host context that
// no longer exists; its completion must not answer a later command.
void bdev_host_reset(void);

#endif // BLOCKDEV_H_
