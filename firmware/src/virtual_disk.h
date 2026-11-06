// RAM-backed virtual floppy image: the hardware-free test backend of the
// floppy_revival_vdisk build (behind blockdev_vdisk.c; msc_disk.c only sees
// blockdev.h, so the drive and vdisk backends are a link-time choice).
//
// Presents a 360KB (720 x 512) FAT12 disk formatted at boot with a single
// README.TXT, so the USB/UFI side can be tested with no drive attached.

#ifndef VIRTUAL_DISK_H_
#define VIRTUAL_DISK_H_

#include <stdbool.h>
#include <stdint.h>

// 360KB 5.25" DD geometry, as a 300 rpm drive spins it (250 kbps). The
// sector size equals BDEV_SECTOR_SIZE (checked in blockdev_vdisk.c); it is
// kept separate so this module stays free of the block-device API.
#define VDISK_SECTOR_SIZE 512u
#define VDISK_CYLS 40u
#define VDISK_HEADS 2u
#define VDISK_SPT 9u
#define VDISK_RPM 300u
#define VDISK_KBPS 250u
#define VDISK_SECTOR_COUNT (VDISK_CYLS * VDISK_HEADS * VDISK_SPT) // 720

void virtual_disk_init(void);
uint32_t virtual_disk_block_count(void);

// Both return the number of bytes handled, or -1 on out-of-range access.
int32_t virtual_disk_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
int32_t virtual_disk_write(uint32_t lba, uint32_t offset, const void *buffer, uint32_t bufsize);

#endif // VIRTUAL_DISK_H_
