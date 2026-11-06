// Host test of the vdisk image (floppy_revival_vdisk): the boot sector,
// FAT and root directory a host mounts without a drive.
#include "virtual_disk.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static unsigned le16(const uint8_t *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

int main(void) {
    uint8_t s[VDISK_SECTOR_SIZE];
    virtual_disk_init();
    check(virtual_disk_block_count() == 720u, "720 sectors");

    check(virtual_disk_read(0, 0, s, sizeof(s)) == (int32_t)sizeof(s), "read boot sector");
    check(s[510] == 0x55 && s[511] == 0xAA, "boot signature");
    check(le16(&s[11]) == 512u, "BPB bytes/sector");
    check(s[13] == 2u, "BPB sectors/cluster");
    check(le16(&s[14]) == 1u && s[16] == 2u, "BPB reserved sectors, FAT count");
    check(le16(&s[17]) == 112u, "BPB root entries");
    check(le16(&s[19]) == 720u, "BPB total sectors");
    check(s[21] == 0xFD, "BPB media 0xFD");
    check(le16(&s[22]) == 2u && le16(&s[24]) == 9u && le16(&s[26]) == 2u,
          "BPB FAT size, spt, heads");

    // Both FAT copies: media echo, then cluster 2 (README) end of chain.
    for (uint32_t fat = 1; fat <= 3; fat += 2) {
        check(virtual_disk_read(fat, 0, s, sizeof(s)) == (int32_t)sizeof(s), "read FAT");
        check(s[0] == 0xFD && s[1] == 0xFF && s[2] == 0xFF, "FAT media bytes");
        check(s[3] == 0xFF && (s[4] & 0x0F) == 0x0F, "FAT cluster 2 EOF");
    }

    check(virtual_disk_read(5, 0, s, sizeof(s)) == (int32_t)sizeof(s), "read root dir");
    check(memcmp(&s[32], "README  TXT", 11) == 0, "README.TXT in root");
    check(le16(&s[32 + 26]) == 2u, "README first cluster 2");
    check(virtual_disk_read(12, 0, s, sizeof(s)) == (int32_t)sizeof(s), "read data");
    check(memcmp(s, "FloppyRevival", 13) == 0, "README contents");

    // Out of range, and a write that reads back.
    check(virtual_disk_read(720, 0, s, sizeof(s)) == -1, "read past end refused");
    memset(s, 0xA5, sizeof(s));
    check(virtual_disk_write(700, 0, s, sizeof(s)) == (int32_t)sizeof(s), "write");
    uint8_t r[VDISK_SECTOR_SIZE];
    check(virtual_disk_read(700, 0, r, sizeof(r)) == (int32_t)sizeof(r) &&
              memcmp(r, s, sizeof(r)) == 0,
          "write reads back");

    if (failures) return 1;
    printf("all vdisk tests passed\n");
    return 0;
}
