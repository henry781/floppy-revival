#include "blockdev.h"
#include "msc_disk.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include <stdio.h>

// The flux read PIO runs at 72 MHz and requires clk_sys to be an exact
// multiple (floppy_init checks); 144 MHz keeps USB (48 MHz from PLL_USB)
// unaffected. The vdisk build doesn't care but uses the same clock.
#define SYS_CLOCK_KHZ 144000u

int main(void) {
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);
    stdio_init_all();
    printf("FloppyRevival starting (clk_sys=%lu)\n", clock_get_hz(clk_sys));

    if (watchdog_caused_reboot()) {
        printf("recovered by watchdog — check the previous session's logs\n");
    }
    bdev_init();

    // A watchdog reset returns all pads to hi-Z, so the board pull-ups
    // release every drive line — the hardware escape from a stuck /WGATE
    // or motor if firmware ever locks up mid-operation (core 0: within 8 s;
    // core 1: its heartbeat goes stale first, see bdev_healthy). Nothing
    // survives the reset: cached writes not yet on the disk are lost and
    // the host is not told beyond seeing the device re-enumerate; only the
    // UART line above records it.
    watchdog_enable(8000, true);

#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

    tud_init(0);

    absolute_time_t next_blink = get_absolute_time();
    bool led_on = false;

    while (true) {
        tud_task();
        bdev_task();
        msc_task();
        // Stop feeding once the backend looks dead so the watchdog resets
        // us; USB alone staying alive must not mask a hung drive core.
        if (bdev_healthy()) watchdog_update();

#ifdef PICO_DEFAULT_LED_PIN
        if (absolute_time_diff_us(get_absolute_time(), next_blink) <= 0) {
            led_on = !led_on;
            gpio_put(PICO_DEFAULT_LED_PIN, led_on);
            // Fast blink until a host configures us, slow heartbeat after.
            next_blink = make_timeout_time_ms(tud_mounted() ? 1000 : 150);
        }
#endif
    }
}
