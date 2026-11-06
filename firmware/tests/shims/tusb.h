// Host-test stand-in for TinyUSB: the SCSI constants msc_disk.c uses, the
// sense setter (recorded for the test) and the connect/disconnect calls.
#ifndef SHIM_TUSB_H_
#define SHIM_TUSB_H_
#include <stdbool.h>
#include <stdint.h>

enum {
    SCSI_SENSE_NONE = 0x00,
    SCSI_SENSE_NOT_READY = 0x02,
    SCSI_SENSE_MEDIUM_ERROR = 0x03,
    SCSI_SENSE_ILLEGAL_REQUEST = 0x05,
    SCSI_SENSE_UNIT_ATTENTION = 0x06,
    SCSI_SENSE_DATA_PROTECT = 0x07,
};

typedef struct { uint8_t key, asc, ascq; } shim_sense_t;
extern shim_sense_t shim_sense;
extern int shim_disconnects, shim_connects;
extern bool shim_mounted;

bool tud_msc_set_sense(uint8_t lun, uint8_t sense_key, uint8_t add_sense_code, uint8_t add_sense_qualifier);
void tud_disconnect(void);
void tud_connect(void);
bool tud_mounted(void);

// Callbacks msc_disk.c implements (declared so the test can call them).
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]);
bool tud_msc_test_unit_ready_cb(uint8_t lun);
void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size);
bool tud_msc_is_writable_cb(uint8_t lun);
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject);
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize);
void tud_umount_cb(void);
#endif
