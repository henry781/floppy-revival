#include "virtual_disk.h"

#include <string.h>

static uint8_t disk_image[VDISK_SECTOR_COUNT * VDISK_SECTOR_SIZE];

// 360KB FAT12 layout (all values in sectors):
//   0        boot sector
//   1..2     FAT #1 (2 sectors)
//   3..4     FAT #2
//   5..11    root directory (112 entries x 32 bytes = 7 sectors)
//   12..719  data area, 2 sectors per cluster; first cluster is #2
#define FAT1_SECTOR 1u // after the boot sector (1 reserved sector)
#define FAT_SECTORS 2u
#define ROOT_DIR_SECTOR (FAT1_SECTOR + 2u * FAT_SECTORS)    // 5
#define DATA_START_SECTOR (ROOT_DIR_SECTOR + 7u)             // 12
_Static_assert(VDISK_SECTOR_COUNT == 720u, "layout above is for 360KB");
#define SECTORS_PER_CLUSTER 2u

static const char readme_text[] =
    "FloppyRevival - 5.25\" floppy drive USB bridge (Raspberry Pi Pico 2)\r\n"
    "\r\n"
    "You are looking at a RAM-backed test disk built into the vdisk\r\n"
    "firmware (floppy_revival_vdisk.uf2), which never touches a drive.\r\n"
    "Flash floppy_revival.uf2 to use the real 5.25\" drive.\r\n"
    "\r\n"
    "Writes to this disk land in RAM and vanish at power-off.\r\n";

static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void format_boot_sector(uint8_t *bs) {
    bs[0] = 0xEB; // jmp short
    bs[1] = 0x3C;
    bs[2] = 0x90; // nop
    memcpy(&bs[3], "FLOPREV ", 8); // OEM name
    write_le16(&bs[11], VDISK_SECTOR_SIZE);
    bs[13] = SECTORS_PER_CLUSTER;
    write_le16(&bs[14], 1); // reserved sectors
    bs[16] = 2;             // number of FATs
    write_le16(&bs[17], 112); // root dir entries
    write_le16(&bs[19], VDISK_SECTOR_COUNT);
    bs[21] = 0xFD; // media descriptor: 360KB 5.25"
    write_le16(&bs[22], FAT_SECTORS);
    write_le16(&bs[24], VDISK_SPT);
    write_le16(&bs[26], VDISK_HEADS);
    write_le32(&bs[28], 0); // hidden sectors
    // Extended boot record
    bs[36] = 0x00; // drive number: floppy
    bs[38] = 0x29; // extended boot signature
    write_le32(&bs[39], 0x46525631); // volume serial "FRV1"
    memcpy(&bs[43], "FLOPPYREV  ", 11);
    memcpy(&bs[54], "FAT12   ", 8);
    bs[510] = 0x55;
    bs[511] = 0xAA;
}

// Mark cluster 2 as end-of-chain in a FAT12 table.
static void format_fat(uint8_t *fat) {
    fat[0] = 0xFD; // media descriptor echo
    fat[1] = 0xFF;
    fat[2] = 0xFF;
    // cluster 2 = 0xFFF (EOF): low 12 bits of bytes 3-4
    fat[3] = 0xFF;
    fat[4] = 0x0F;
}

static void format_root_dir(uint8_t *dir) {
    // Volume label entry
    memcpy(&dir[0], "FLOPPYREV  ", 11);
    dir[11] = 0x08; // ATTR_VOLUME_ID

    // README.TXT, cluster 2
    uint8_t *e = &dir[32];
    memcpy(&e[0], "README  TXT", 11);
    e[11] = 0x21; // read-only + archive
    write_le16(&e[22], 0x6000); // time 12:00:00
    write_le16(&e[24], 0x5D24); // date 2026-09-04
    write_le16(&e[26], 2);      // first cluster
    write_le32(&e[28], sizeof(readme_text) - 1);
}

void virtual_disk_init(void) {
    memset(disk_image, 0, sizeof(disk_image));
    format_boot_sector(&disk_image[0]);
    format_fat(&disk_image[FAT1_SECTOR * VDISK_SECTOR_SIZE]);
    format_fat(&disk_image[(FAT1_SECTOR + FAT_SECTORS) * VDISK_SECTOR_SIZE]); // FAT #2
    format_root_dir(&disk_image[ROOT_DIR_SECTOR * VDISK_SECTOR_SIZE]);
    memcpy(&disk_image[DATA_START_SECTOR * VDISK_SECTOR_SIZE], readme_text,
           sizeof(readme_text) - 1);
}

uint32_t virtual_disk_block_count(void) {
    return VDISK_SECTOR_COUNT;
}

int32_t virtual_disk_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    if (lba >= VDISK_SECTOR_COUNT || offset >= VDISK_SECTOR_SIZE) {
        return -1;
    }
    uint32_t avail = VDISK_SECTOR_SIZE - offset;
    uint32_t n = bufsize < avail ? bufsize : avail;
    memcpy(buffer, &disk_image[lba * VDISK_SECTOR_SIZE + offset], n);
    return (int32_t)n;
}

int32_t virtual_disk_write(uint32_t lba, uint32_t offset, const void *buffer, uint32_t bufsize) {
    if (lba >= VDISK_SECTOR_COUNT || offset >= VDISK_SECTOR_SIZE) {
        return -1;
    }
    uint32_t avail = VDISK_SECTOR_SIZE - offset;
    uint32_t n = bufsize < avail ? bufsize : avail;
    memcpy(&disk_image[lba * VDISK_SECTOR_SIZE + offset], buffer, n);
    return (int32_t)n;
}
