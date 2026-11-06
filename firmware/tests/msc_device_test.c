// Host-side test of the vendored TinyUSB MSC class driver
// (src/tinyusb/msc_device.c, compiled here as is, no shim): the sense an
// application callback sets when READ(10) or WRITE(10) fails is the sense
// the host reads with REQUEST SENSE. Upstream 0.18 replaced it with 02/3A/00
// "medium not present", so a host saw an unreadable sector, a deferred
// write fault or a write-protect flip as a pulled disk.
//
// The bulk-only transport is driven through the driver's own entry points
// (mscd_open, mscd_xfer_cb, mscd_control_xfer_cb) with the USB device layer
// stubbed below. TinyUSB headers come from the pico-sdk
// (tests/run_host_tests.sh passes the include path).

#include "../src/tinyusb/msc_device.c"
#include "device/dcd.h"

#include <stdio.h>
#include <string.h>

//--------------------------------------------------------------------
// USB device layer stubs: record the last transfer queued, endpoint stalls
//--------------------------------------------------------------------

#define EP_OUT 0x01
#define EP_IN 0x81

static uint8_t *xfer_buf;
static uint8_t xfer_ep;
static uint16_t xfer_len;
static bool stalled_in, stalled_out;

bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep, uint8_t *buf, uint16_t len) {
    (void)rhport;
    xfer_ep = ep;
    xfer_buf = buf;
    xfer_len = len;
    return true;
}
void usbd_edpt_stall(uint8_t rhport, uint8_t ep) {
    (void)rhport;
    if (ep == EP_IN) stalled_in = true;
    else stalled_out = true;
}
void usbd_edpt_clear_stall(uint8_t rhport, uint8_t ep) {
    (void)rhport;
    if (ep == EP_IN) stalled_in = false;
    else stalled_out = false;
}
bool usbd_edpt_stalled(uint8_t rhport, uint8_t ep) {
    (void)rhport;
    return ep == EP_IN ? stalled_in : stalled_out;
}
bool usbd_edpt_busy(uint8_t rhport, uint8_t ep) {
    (void)rhport;
    (void)ep;
    return false;
}
bool usbd_edpt_claim(uint8_t rhport, uint8_t ep) {
    (void)rhport;
    (void)ep;
    return true;
}
bool usbd_edpt_release(uint8_t rhport, uint8_t ep) {
    (void)rhport;
    (void)ep;
    return true;
}
bool usbd_open_edpt_pair(uint8_t rhport, uint8_t const *desc, uint8_t count, uint8_t type,
                         uint8_t *out, uint8_t *in) {
    (void)rhport;
    (void)desc;
    (void)count;
    (void)type;
    *out = EP_OUT;
    *in = EP_IN;
    return true;
}
// "Call the read/write callback again" (a callback that returned 0):
// nothing in these tests answers 0.
void dcd_event_handler(dcd_event_t const *event, bool in_isr) {
    (void)event;
    (void)in_isr;
}
bool tud_control_status(uint8_t rhport, tusb_control_request_t const *request) {
    (void)rhport;
    (void)request;
    return true;
}
bool tud_control_xfer(uint8_t rhport, tusb_control_request_t const *request, void *buffer,
                      uint16_t len) {
    (void)rhport;
    (void)request;
    (void)buffer;
    (void)len;
    return true;
}

//--------------------------------------------------------------------
// Application callbacks: scripted failure of READ(10) / WRITE(10)
//--------------------------------------------------------------------

typedef struct {
    uint8_t key, asc, ascq;
} sense_t;

static bool app_sets_sense; // the failing callback sets app_sense first
static sense_t app_sense;
static bool app_writable = true;

static int32_t app_fail(uint8_t lun) {
    if (app_sets_sense) tud_msc_set_sense(lun, app_sense.key, app_sense.asc, app_sense.ascq);
    return -1;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer,
                          uint32_t bufsize) {
    (void)lba;
    (void)offset;
    (void)buffer;
    (void)bufsize;
    return app_fail(lun);
}
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer,
                           uint32_t bufsize) {
    (void)lba;
    (void)offset;
    (void)buffer;
    (void)bufsize;
    return app_fail(lun);
}
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4]) {
    (void)lun;
    memset(vendor_id, ' ', 8);
    memset(product_id, ' ', 16);
    memset(product_rev, ' ', 4);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return true;
}
void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = 2400;
    *block_size = 512;
}
bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return app_writable;
}
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)scsi_cmd;
    (void)buffer;
    (void)bufsize;
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}
// TinyUSB declares the rest weak; the firmware does not implement them.
// Defined here with the driver's default behaviour so the test links on
// every host toolchain.
uint8_t tud_msc_get_maxlun_cb(void) { return 1; }
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun;
    (void)power_condition;
    (void)start;
    (void)load_eject;
    return true;
}
bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun, uint8_t prohibit, uint8_t control) {
    (void)lun;
    (void)prohibit;
    (void)control;
    return true;
}
int32_t tud_msc_request_sense_cb(uint8_t lun, void *buffer, uint16_t bufsize) {
    (void)lun;
    (void)buffer;
    (void)bufsize;
    return (int32_t)sizeof(scsi_sense_fixed_resp_t); // response left as built
}
void tud_msc_read10_complete_cb(uint8_t lun) { (void)lun; }
void tud_msc_write10_complete_cb(uint8_t lun) { (void)lun; }
void tud_msc_scsi_complete_cb(uint8_t lun, uint8_t const scsi_cmd[16]) {
    (void)lun;
    (void)scsi_cmd;
}

//--------------------------------------------------------------------
// Host side of the bulk-only transport
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

static void open_interface(void) {
    static const uint8_t desc[] = {
        9, TUSB_DESC_INTERFACE, 0, 0, 2, TUSB_CLASS_MSC, MSC_SUBCLASS_SCSI, MSC_PROTOCOL_BOT, 0,
        7, TUSB_DESC_ENDPOINT, EP_OUT, TUSB_XFER_BULK, 64, 0, 0,
        7, TUSB_DESC_ENDPOINT, EP_IN, TUSB_XFER_BULK, 64, 0, 0,
    };
    mscd_init();
    stalled_in = stalled_out = false;
    mscd_open(0, (tusb_desc_interface_t const *)desc, sizeof desc);
}

// The host sends a CBW (the driver has queued its OUT transfer for it).
static void send_cbw(uint8_t opcode, uint32_t data_len, bool data_in) {
    CHECK(xfer_ep == EP_OUT && xfer_len == sizeof(msc_cbw_t), "driver waits for a CBW");
    msc_cbw_t cbw = {0};
    cbw.signature = MSC_CBW_SIGNATURE;
    cbw.tag = 0x1234;
    cbw.total_bytes = data_len;
    cbw.dir = data_in ? 0x80 : 0x00;
    cbw.cmd_len = opcode == SCSI_CMD_REQUEST_SENSE ? 6 : 10;
    cbw.command[0] = opcode;
    if (opcode == SCSI_CMD_REQUEST_SENSE) {
        cbw.command[4] = (uint8_t)data_len;
    } else {
        cbw.command[5] = 7;                         // LBA 7
        cbw.command[8] = (uint8_t)(data_len / 512); // transfer length in blocks
    }
    memcpy(xfer_buf, &cbw, sizeof cbw);
    mscd_xfer_cb(0, EP_OUT, XFER_RESULT_SUCCESS, sizeof cbw);
}

// Status stage: clear a stalled IN endpoint as a host does (CLEAR_FEATURE
// ENDPOINT_HALT), then take the CSW. Returns its status byte.
static uint8_t take_csw(void) {
    if (stalled_in) {
        stalled_in = false;
        tusb_control_request_t clear = {
            .bmRequestType_bit = {.recipient = TUSB_REQ_RCPT_ENDPOINT,
                                  .type = TUSB_REQ_TYPE_STANDARD,
                                  .direction = TUSB_DIR_OUT},
            .bRequest = TUSB_REQ_CLEAR_FEATURE,
            .wValue = TUSB_REQ_FEATURE_EDPT_HALT,
            .wIndex = EP_IN,
        };
        mscd_control_xfer_cb(0, CONTROL_STAGE_SETUP, &clear);
    }
    CHECK(xfer_ep == EP_IN && xfer_len == sizeof(msc_csw_t), "CSW queued");
    msc_csw_t csw;
    memcpy(&csw, xfer_buf, sizeof csw);
    mscd_xfer_cb(0, EP_IN, XFER_RESULT_SUCCESS, sizeof csw);
    return csw.status;
}

// REQUEST SENSE through the transport, as the host issues it after a
// failed command.
static sense_t request_sense(void) {
    send_cbw(SCSI_CMD_REQUEST_SENSE, 18, true);
    CHECK(xfer_ep == EP_IN && xfer_len == 18, "sense data queued");
    sense_t s = {(uint8_t)(xfer_buf[2] & 0x0F), xfer_buf[12], xfer_buf[13]};
    mscd_xfer_cb(0, EP_IN, XFER_RESULT_SUCCESS, 18);
    CHECK(take_csw() == MSC_CSW_STATUS_PASSED, "REQUEST SENSE passes");
    return s;
}

static sense_t failed_read10(void) {
    open_interface();
    send_cbw(SCSI_CMD_READ_10, 512, true);
    CHECK(take_csw() == MSC_CSW_STATUS_FAILED, "READ(10) fails");
    return request_sense();
}

static sense_t failed_write10(void) {
    open_interface();
    send_cbw(SCSI_CMD_WRITE_10, 512, false);
    if (xfer_ep == EP_OUT && xfer_len == 512) {
        // Data stage: the host's 512 bytes reach the write callback.
        memset(xfer_buf, 0x5A, 512);
        mscd_xfer_cb(0, EP_OUT, XFER_RESULT_SUCCESS, 512);
    }
    CHECK(take_csw() == MSC_CSW_STATUS_FAILED, "WRITE(10) fails");
    return request_sense();
}

static bool sense_is(sense_t s, uint8_t key, uint8_t asc, uint8_t ascq) {
    return s.key == key && s.asc == asc && s.ascq == ascq;
}

static void expect_kept(const char *what, sense_t (*command)(void), uint8_t key, uint8_t asc,
                        uint8_t ascq) {
    app_sets_sense = true;
    app_sense = (sense_t){key, asc, ascq};
    sense_t s = command();
    CHECK(sense_is(s, key, asc, ascq), "%s: host reads %02X/%02X/%02X, app set %02X/%02X/%02X",
          what, s.key, s.asc, s.ascq, key, asc, ascq);
}

int main(void) {
    // msc_disk.c on an unreadable sector: MEDIUM ERROR, unrecovered read error.
    expect_kept("READ(10) unreadable sector", failed_read10, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);
    // msc_disk.c on a deferred write error: MEDIUM ERROR, write fault.
    expect_kept("WRITE(10) deferred write fault", failed_write10, SCSI_SENSE_MEDIUM_ERROR, 0x03,
                0x00);
    // msc_disk.c on a write-protect tab flipped mid-command: DATA PROTECT.
    expect_kept("WRITE(10) write-protect flip", failed_write10, SCSI_SENSE_DATA_PROTECT, 0x27,
                0x00);
    // msc_disk.c on a pulled disk: its own NOT READY, medium not present.
    expect_kept("READ(10) pulled disk", failed_read10, SCSI_SENSE_NOT_READY, 0x3A, 0x00);

    // A callback that fails without a sense still gets the upstream default.
    app_sets_sense = false;
    sense_t s = failed_read10();
    CHECK(sense_is(s, SCSI_SENSE_NOT_READY, 0x3A, 0x00),
          "READ(10) with no app sense: default 02/3A/00 (got %02X/%02X/%02X)", s.key, s.asc,
          s.ascq);
    s = failed_write10();
    CHECK(sense_is(s, SCSI_SENSE_NOT_READY, 0x3A, 0x00),
          "WRITE(10) with no app sense: default 02/3A/00 (got %02X/%02X/%02X)", s.key, s.asc,
          s.ascq);

    // Unchanged upstream path: a medium the application calls read-only.
    app_writable = false;
    open_interface();
    send_cbw(SCSI_CMD_WRITE_10, 512, false);
    CHECK(take_csw() == MSC_CSW_STATUS_FAILED, "WRITE(10) on a read-only medium fails");
    s = request_sense();
    CHECK(sense_is(s, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00),
          "read-only medium: 07/27/00 (got %02X/%02X/%02X)", s.key, s.asc, s.ascq);
    app_writable = true;

    if (failures) {
        printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("all msc_device tests passed\n");
    return 0;
}
