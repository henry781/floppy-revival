// TinyUSB configuration for host-side tests that compile the real class
// driver (tests/msc_device_test.c): no MCU, no OS, one MSC interface with
// the firmware's endpoint buffer size (src/tusb_config.h).
#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#define CFG_TUSB_MCU OPT_MCU_NONE
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUD_ENABLED 1
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_MSC 1
#define CFG_TUD_MSC_EP_BUFSIZE 512
#define TUP_DCD_ENDPOINT_MAX 8

#endif
