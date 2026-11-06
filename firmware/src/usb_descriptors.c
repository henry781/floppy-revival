// USB descriptors for a UFI-subclass mass-storage floppy device.
//
// Real USB floppy drives identify with bInterfaceSubClass = UFI (0x04)
// rather than the SCSI-transparent subclass (0x06) that flash drives use.
// TinyUSB's TUD_MSC_DESCRIPTOR macro hardcodes the SCSI subclass, so we
// build the interface descriptor ourselves.

#include "msc_disk.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include "device/usbd_pvt.h"

// Set from CMake for every target (configure_firmware: FLOPPY_USB_VID /
// FLOPPY_USB_PID, defaults and the release check live there, see README).
#if !defined(FLOPPY_USB_VID) || !defined(FLOPPY_USB_PID)
#error "FLOPPY_USB_VID / FLOPPY_USB_PID must come from CMake"
#endif
#define USB_BCD 0x0200
// bcdDevice: firmware revision, rev C = 1.00.
#define USB_DEVICE_BCD 0x0100

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
};

#define EPNUM_MSC_OUT 0x01
#define EPNUM_MSC_IN 0x81

//--------------------------------------------------------------------
// Device descriptor
//--------------------------------------------------------------------

static tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,
    .bDeviceClass = 0x00, // class defined at interface level
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = FLOPPY_USB_VID,
    .idProduct = FLOPPY_USB_PID,
    .bcdDevice = USB_DEVICE_BCD,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------
// Configuration descriptor
//--------------------------------------------------------------------

// Same layout as TUD_MSC_DESCRIPTOR but with the UFI subclass.
#define TUD_MSC_UFI_DESCRIPTOR(_itfnum, _stridx, _epout, _epin, _epsize)                             \
    9, TUSB_DESC_INTERFACE, _itfnum, 0, 2, TUSB_CLASS_MSC, MSC_SUBCLASS_UFI, MSC_PROTOCOL_BOT,       \
        _stridx,                                                                                     \
        7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0,                    \
        7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static uint8_t const desc_configuration[] = {
    // Bus powered: the Pico and the two SN7407N run from VBUS (the drive has
    // its own supply). Pico 2 + buffers stay well under this budget.
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 250),
    TUD_MSC_UFI_DESCRIPTOR(ITF_NUM_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

//--------------------------------------------------------------------
// UFI class driver
//--------------------------------------------------------------------

// TinyUSB's mscd_open() rejects any subclass but SCSI, so the stock MSC
// driver never claims a UFI interface and SET_CONFIGURATION fails. This app
// driver opens it through mscd_open() on a copy of the interface + endpoint
// descriptors with the subclass patched to SCSI (mscd_open keeps no pointer
// to it); everything else is the stock BOT driver. App drivers are probed
// before the built-in ones.
static uint16_t ufi_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc,
                         uint16_t max_len) {
    uint8_t desc[TUD_MSC_DESC_LEN];
    TU_VERIFY(itf_desc->bInterfaceClass == TUSB_CLASS_MSC &&
                  itf_desc->bInterfaceSubClass == MSC_SUBCLASS_UFI &&
                  itf_desc->bInterfaceProtocol == MSC_PROTOCOL_BOT &&
                  max_len >= sizeof(desc),
              0);
    memcpy(desc, itf_desc, sizeof(desc));
    ((tusb_desc_interface_t *)desc)->bInterfaceSubClass = MSC_SUBCLASS_SCSI;
    return mscd_open(rhport, (tusb_desc_interface_t const *)desc, sizeof(desc));
}

// A bus reset reaches the class driver but not tud_umount_cb: tell the MSC
// glue so a request in flight is not matched to the host's next command.
static void ufi_reset(uint8_t rhport) {
    mscd_reset(rhport);
    msc_bus_reset();
}

static usbd_class_driver_t const ufi_driver = {
    .name = "UFI",
    .init = mscd_init,
    .deinit = mscd_deinit,
    .reset = ufi_reset,
    .open = ufi_open,
    .control_xfer_cb = mscd_control_xfer_cb,
    .xfer_cb = mscd_xfer_cb,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &ufi_driver;
}

//--------------------------------------------------------------------
// String descriptors
//--------------------------------------------------------------------

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04}, // 0: English (0x0409)
    "FloppyRevival",            // 1: Manufacturer
    "5.25in Floppy Drive",      // 2: Product
    NULL,                       // 3: Serial (from unique board id)
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t chr_count;

    switch (index) {
    case STRID_LANGID:
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
        break;

    case STRID_SERIAL: {
        char serial[17];
        pico_get_unique_board_id_string(serial, sizeof(serial));
        chr_count = strlen(serial);
        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = (uint16_t)serial[i];
        }
        break;
    }

    default: {
        if (index >= TU_ARRAY_SIZE(string_desc_arr) || string_desc_arr[index] == NULL) {
            return NULL;
        }
        const char *str = string_desc_arr[index];
        chr_count = strlen(str);
        size_t const max_count = TU_ARRAY_SIZE(_desc_str) - 1;
        if (chr_count > max_count) {
            chr_count = max_count;
        }
        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = (uint16_t)str[i];
        }
        break;
    }
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
