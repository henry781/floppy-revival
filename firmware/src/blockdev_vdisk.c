// blockdev backend over the RAM FAT12 image — always ready, synchronous.

#include "blockdev.h"
#include "virtual_disk.h"

#include <string.h>

_Static_assert(VDISK_SECTOR_SIZE == BDEV_SECTOR_SIZE, "vdisk sector = block size");

void bdev_init(void) {
    virtual_disk_init();
}

void bdev_task(void) {}

bool bdev_healthy(void) {
    return true; // synchronous backend, nothing to hang
}

bdev_media_state_t bdev_media_state(void) {
    return BDEV_MEDIA_READY;
}

bool bdev_media_ready(void) {
    return true;
}

uint32_t bdev_media_seq(void) {
    return 1;
}

bool bdev_writable(void) {
    return true;
}

bool bdev_write_protected(void) {
    return false;
}

bool bdev_dirty(void) {
    return false;
}

bool bdev_take_write_error(void) {
    return false; // writes land in RAM synchronously, none can be dropped
}

unsigned bdev_formattable(uint32_t blocks[], unsigned max) {
    if (max == 0) return 0;
    blocks[0] = virtual_disk_block_count();
    return 1;
}

uint32_t bdev_max_blocks(void) {
    return virtual_disk_block_count();
}

int32_t bdev_format_track(uint32_t blocks, uint8_t cylinder, uint8_t head) {
    // The RAM image is always formatted; accept the matching geometry so a
    // host-side format run can be exercised without a drive.
    if (blocks != virtual_disk_block_count() || cylinder >= VDISK_CYLS || head >= VDISK_HEADS) return BDEV_EINVAL;
    return BDEV_OK;
}

void bdev_host_reset(void) {}

uint32_t bdev_block_count(void) {
    return virtual_disk_block_count();
}

void bdev_geometry(uint16_t *cylinders, uint8_t *heads, uint8_t *spt,
                   uint16_t *rpm, uint16_t *kbps) {
    *cylinders = VDISK_CYLS;
    *heads = VDISK_HEADS;
    *spt = VDISK_SPT;
    *rpm = VDISK_RPM;
    *kbps = VDISK_KBPS;
}

int32_t bdev_read(uint32_t lba, uint8_t *buf) {
    if (lba >= virtual_disk_block_count()) return BDEV_EINVAL;
    return virtual_disk_read(lba, 0, buf, BDEV_SECTOR_SIZE) == (int32_t)BDEV_SECTOR_SIZE
               ? BDEV_OK
               : BDEV_EIO;
}

int32_t bdev_write(uint32_t lba, const uint8_t *buf) {
    if (lba >= virtual_disk_block_count()) return BDEV_EINVAL;
    return virtual_disk_write(lba, 0, buf, BDEV_SECTOR_SIZE) == (int32_t)BDEV_SECTOR_SIZE
               ? BDEV_OK
               : BDEV_EIO;
}

void bdev_flush_async(void) {}
bool bdev_flush_wait(void) { return true; }
