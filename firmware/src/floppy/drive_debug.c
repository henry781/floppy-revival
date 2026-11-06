// Bring-up monitor (floppy_revival_debug only): a status line every second
// and single-key commands on UART0. Everything runs on core 1, between
// drive operations, so it can never race the drive agent; two exceptions
// read or act on core 0: the status line's adapter mV (a core-0 snapshot)
// and 'B' / 'P' (core 0 reboots or power-cycles the drive, see
// drive_debug_bootsel_request). No command writes: /WGATE is only ever
// reported, never driven.

#include "drive_internal.h"
#include "mfm.h"
#include "pins.h"

#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

#if FLOPPY_DEBUG

volatile bool drive_debug_bootsel_request;
volatile bool drive_debug_power_cycle_request;
static bool dbg_auto = true; // automatic probing of an empty drive
static bool dbg_periodic = true;
static uint32_t dbg_status_ms;
static uint32_t dbg_status_edges;
static volatile uint32_t dbg_index_edges;
static volatile uint32_t dbg_index_last_us;
static volatile uint32_t dbg_index_period_us;

static void dbg_index_irq(uint gpio, uint32_t events) {
    (void)gpio;
    (void)events;
    uint32_t now = time_us_32();
    if (dbg_index_edges) dbg_index_period_us = now - dbg_index_last_us;
    dbg_index_last_us = now;
    dbg_index_edges++;
}

// Inputs: 'L' = line pulled low (asserted), 'h' = released.
static char dbg_level(uint pin) {
    return gpio_get(pin) ? 'h' : 'L';
}

// Outputs are open-collector: driven (pin set to output) means asserted low.
static const char *dbg_out(uint pin, const char *name) {
    return gpio_is_dir_out(pin) ? name : "";
}

static const char *dbg_media_name(void) {
    const floppy_format_t *fmt = media_fmt;
    switch (media_state) {
    case MEDIA_READY:
        return fmt ? fmt->name : "ready?";
    case MEDIA_UNFORMATTED:
        return "unformatted disk";
    default:
        return "no media";
    }
}

static void dbg_status(void) {
    const floppy_pins_t *p = &drive.pins;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t edges = dbg_index_edges;
    uint32_t window = now - dbg_status_ms;
    uint32_t count = edges - dbg_status_edges;
    dbg_status_ms = now;
    dbg_status_edges = edges;
    bool idx_recent = edges && time_us_32() - dbg_index_last_us < 1000000u;
    printf("st %lus | in IDX=%c TRK0=%c WPT=%c DCHG=%c RD=%c | asserted:%s%s%s%s%s%s%s"
           " | pwr %s %lu mV | index %lu in %lu ms, period %lu ms | cyl %u%s | %s | auto %s\n",
           (unsigned long)(now / 1000u), dbg_level(p->index), dbg_level(p->track0),
           dbg_level(p->write_protect), dbg_level(p->disk_change),
           dbg_level(p->read_data), dbg_out(p->drive_select, " SEL"),
           dbg_out(p->motor_enable, " MOTOR"), dbg_out(p->direction, " DIR-in"),
           dbg_out(p->step, " STEP"), dbg_out(p->side_select, " SIDE1"),
           dbg_out(p->density, " REDWC"), dbg_out(p->write_gate, " WGATE!"),
#if FLOPPY_DRIVE_POWER_PIN >= 0
           gpio_get_out_level(FLOPPY_DRIVE_POWER_PIN) ? "on" : "OFF",
           (unsigned long)adapter_last_mv,
#else
           "n/a", 0ul,
#endif
           (unsigned long)count, (unsigned long)window,
           idx_recent ? (unsigned long)(dbg_index_period_us / 1000u) : 0ul,
           drive.cylinder, drive.track0_confirmed ? "" : "?", dbg_media_name(),
           dbg_auto ? "on" : "PAUSED");
}

static void dbg_help(void) {
    printf("monitor keys:\n"
           "  ?  this help              r  status line now\n"
           "  v  1 s status on/off\n"
           "  a  pause/resume auto-probe (empty drive: quick DSKCHG check,\n"
           "     full probe once a disk is in)\n"
           "  p  raw probe now (no retry pacing, unformatted report or TRK00\n"
           "     fault count: those belong to the auto-probe)\n"
           "  s  select on/off (pin 12)\n"
           "  m  motor on/off (pin 16; on = select + wait for index)\n"
           "  i  count index pulses for 2 s (motor must run)\n"
           "  +  one step in            -  one step out\n"
           "  0  step out until TRK00 (max 90)\n"
           "  t  drive statistics (reads, retries, sector writes, ...)\n"
           "  w  180 s idle swap statistics (50 us sample every 100 ms, auto re-arm);\n"
           "     core 1 does nothing else meanwhile (heartbeat kept alive)\n"
           "  B  reboot into BOOTSEL (USB mass storage) for flashing\n"
           "  P  power-cycle the drive (off, back on after the usual hold)\n"
           "  f  flux histogram + MFM decode, head 0, current cyl, each density\n"
           "  s m i + - 0 f w need auto-probe paused (a). No key ever writes.\n");
}

static void dbg_result(const char *what, disk_err_t status) {
    printf("%s -> %s | TRK00 %c, cyl %u%s\n", what, disk_err_name(status),
           dbg_level(drive.pins.track0), drive.cylinder,
           drive.track0_confirmed ? "" : "?");
}

static void dbg_stats(void) {
    floppy_stats_t st;
    if (floppy_stats(&drive, &st) != DISK_OK) {
        printf("stats: drive busy\n");
        return;
    }
    printf("reads %lu retries %lu recovered %lu failed %lu | timeout %lu crc %lu "
           "wrong_track %lu wrong_side %lu | overruns %lu underruns %lu | "
           "media_changes %lu | format writes %lu sector writes %lu late_gate %lu "
           "not_found %lu collateral %lu | ring peak %lu\n",
           (unsigned long)st.reads, (unsigned long)st.retries,
           (unsigned long)st.recovered, (unsigned long)st.failed,
           (unsigned long)st.timeout, (unsigned long)st.crc,
           (unsigned long)st.wrong_track, (unsigned long)st.wrong_side,
           (unsigned long)st.overruns, (unsigned long)st.underruns,
           (unsigned long)st.media_changes, (unsigned long)st.dma_writes,
           (unsigned long)st.sector_writes, (unsigned long)st.late_gate,
           (unsigned long)st.not_found, (unsigned long)st.collateral,
           (unsigned long)st.ring_peak);
}

// Raw flux on head 0 of the current cylinder for ~2.5 revolutions at one
// format's density: pulse-spacing histogram (0.25 us bins) and what the MFM
// decoder makes of it. Healthy MFM shows three narrow peaks at 2/3/4 cells.
#define DBG_FLUX_BINS 64u
static uint32_t dbg_hist[DBG_FLUX_BINS + 1];
static floppy_pulse_t dbg_pulses[64];
static mfm_t dbg_mfm;

static void dbg_flux(const floppy_format_t *fmt) {
    disk_err_t status = floppy_set_format(&drive, fmt);
    uint32_t generation = 0;
    if (status == DISK_OK) status = floppy_media_generation(&drive, &generation);
    if (status == DISK_OK) status = floppy_flux_begin(&drive, generation);
    if (status != DISK_OK) {
        printf("flux %s: begin -> %s\n", fmt->name, disk_err_name(status));
        return;
    }
    memset(dbg_hist, 0, sizeof(dbg_hist));
    mfm_init(&dbg_mfm, fmt);
    mfm_sector_t sector;
    uint32_t total = 0, indexes = 0, sectors = 0;
    bool prev_index = false;
    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - start < 420u) {
        disk_result_t r = floppy_flux_read(&drive, dbg_pulses, 64);
        for (size_t i = 0; i < r.count; i++) {
            const floppy_pulse_t *q = &dbg_pulses[i];
            if (q->index && !prev_index) indexes++;
            prev_index = q->index;
            uint32_t bin = q->delta / 6u; // 6 ticks = 0.25 us
            dbg_hist[bin < DBG_FLUX_BINS ? bin : DBG_FLUX_BINS]++;
            total++;
            if (mfm_feed(&dbg_mfm, q->delta, &sector)) {
                sectors++;
                if (sectors <= 3) {
                    printf("  sector C%u H%u S%u\n", sector.cylinder, sector.head,
                           sector.sector + 1u);
                }
            }
        }
        if (r.error != DISK_OK) {
            printf("  read stopped: %s\n", disk_err_name(r.error));
            break;
        }
    }
    floppy_flux_end(&drive);
    printf("flux %s (cell %.2f us): %lu pulses, %lu index edges | syncs %lu, "
           "sectors %lu, crc err %lu, format err %lu\n",
           fmt->name, fmt->cell_ticks / 24.0, (unsigned long)total,
           (unsigned long)indexes, (unsigned long)dbg_mfm.syncs_found,
           (unsigned long)sectors, (unsigned long)dbg_mfm.crc_errors,
           (unsigned long)dbg_mfm.format_errors);
    static const char *const st[] = {"HUNT", "SYNC", "DATA", "CLOCK"};
    static const char *const rec[] = {"ID", "EXPECT_DATA", "DATA"};
    for (unsigned e = 0; e < dbg_mfm.err_log_count; e++) {
        printf("  fmt err @mfm.c:%u delta %.2f us, cell %.2f us, %s/%s, byte %u\n",
               dbg_mfm.err_log[e].line, dbg_mfm.err_log[e].delta / 24.0,
               dbg_mfm.err_log[e].cell / 24.0, st[dbg_mfm.err_log[e].state & 3],
               rec[dbg_mfm.err_log[e].record % 3], dbg_mfm.err_log[e].pos);
    }
    uint32_t peak = 1;
    for (unsigned b = 0; b <= DBG_FLUX_BINS; b++) {
        if (dbg_hist[b] > peak) peak = dbg_hist[b];
    }
    for (unsigned b = 0; b <= DBG_FLUX_BINS; b++) {
        if (dbg_hist[b] == 0) continue;
        char bar[51];
        unsigned n = (unsigned)(dbg_hist[b] * 50u / peak);
        memset(bar, '#', n);
        bar[n] = 0;
        if (b < DBG_FLUX_BINS) {
            printf("  %5.2f us %7lu %s\n", b * 0.25, (unsigned long)dbg_hist[b], bar);
        } else {
            printf("  >=16 us  %7lu %s\n", (unsigned long)dbg_hist[b], bar);
        }
    }
}

// Idle swap statistics: samples DSKCHG/WPT through a 50 us select pulse
// every 100 ms for 180 s and re-arms the latch after each swap. Drives the
// select/dir/step pads directly, so the drive must be idle and the agent
// paused (auto-probe off): floppy_t keeps no state for this test. Core 1
// stays in this loop the whole 180 s (no host request is served: core 0
// keeps answering BDEV_AGAIN) and beats the heartbeat itself, or the
// watchdog check would reset the chip mid-test.
static void dbg_swap_stats(void) {
    if (drive.selected || drive.motor_on) {
        printf("drive must be idle (s / m)\n");
        return;
    }
    uint sel = drive.pins.drive_select, dir = drive.pins.direction;
    uint stp = drive.pins.step;
    uint dcp = drive.pins.disk_change, wpp = drive.pins.write_protect;
    gpio_put(sel, 0);
    gpio_put(dir, 0);
    gpio_put(stp, 0);
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    bool in_swap = false, latched_in_swap = false, dc_was_low = false;
    uint32_t last_wp_low = 0;
    unsigned swaps = 0, caught = 0;
    printf("w: idle swap statistics for 180 s\n");
    while (to_ms_since_boot(get_absolute_time()) - t0 < 180000u) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        core1_beat();
        gpio_set_dir(sel, GPIO_OUT);
        busy_wait_us(50);
        bool dc_low = !gpio_get(dcp), wp_low = !gpio_get(wpp);
        gpio_set_dir(sel, GPIO_IN);
        float t = (now - t0) / 1000.0f;
        if (dc_low && !dc_was_low) {
            printf("w %6.1fs DSKCHG latched%s\n", t,
                   in_swap ? "" : " (before any WPT activity)");
            if (in_swap) latched_in_swap = true;
        }
        dc_was_low = dc_low;
        if (wp_low) {
            if (!in_swap) {
                in_swap = true;
                latched_in_swap = dc_low;
                printf("w %6.1fs disk movement (WPT)%s\n", t,
                       dc_low ? ", DSKCHG already latched" : "");
            }
            last_wp_low = now;
        }
        // Swap over: no WPT activity for 4 s.
        if (in_swap && now - last_wp_low > 4000u) {
            in_swap = false;
            swaps++;
            if (latched_in_swap || dc_low) caught++;
            printf("w %6.1fs swap #%u done: %s\n", t, swaps,
                   latched_in_swap || dc_low ? "DSKCHG latched (OK)" : "MISSED");
            if (dc_low) {
                // Re-arm: selected, step in then out (disk is back in).
                gpio_set_dir(sel, GPIO_OUT);
                busy_wait_us(50);
                gpio_set_dir(dir, GPIO_OUT);
                busy_wait_us(10);
                gpio_set_dir(stp, GPIO_OUT);
                busy_wait_us(3);
                gpio_set_dir(stp, GPIO_IN);
                sleep_ms(10);
                gpio_set_dir(dir, GPIO_IN);
                busy_wait_us(10);
                gpio_set_dir(stp, GPIO_OUT);
                busy_wait_us(3);
                gpio_set_dir(stp, GPIO_IN);
                sleep_ms(40);
                bool still = !gpio_get(dcp);
                gpio_set_dir(sel, GPIO_IN);
                dc_was_low = still;
                printf("w %6.1fs re-armed: DSKCHG %s\n", t, still ? "STILL LOW" : "clear");
            }
        }
        sleep_ms(100);
    }
    printf("w done: %u swap(s), %u caught, %u missed\n", swaps, caught, swaps - caught);
}

static void dbg_command(int c) {
    if (dbg_auto && c && strchr("smi+-0fw", c)) {
        printf("'%c': pause auto-probe first (a)\n", c);
        return;
    }
    switch (c) {
    case '?':
    case 'h':
        dbg_help();
        break;
    case 'r':
        dbg_status();
        break;
    case 'v':
        dbg_periodic = !dbg_periodic;
        printf("periodic status %s\n", dbg_periodic ? "on" : "off");
        break;
    case 'a':
        dbg_auto = !dbg_auto;
        if (!dbg_auto && media_state != MEDIA_READY) {
            floppy_motor_off(&drive);
            floppy_select(&drive, false);
        }
        printf("auto-probe %s\n", dbg_auto ? "resumed" : "PAUSED (drive idle)");
        break;
    case 'p':
        // core1_try_probe alone: none of the automatic probe's bookkeeping
        // (failed_probes pacing, unformatted report, parking, TRK00 fault
        // latch) is updated, so a bench probe never changes what the agent
        // does next.
        if (media_state == MEDIA_READY) {
            printf("media already probed\n");
        } else if (!core1_try_probe()) {
            printf("probe failed (last: %s)\n", disk_err_name(last_probe_err));
            floppy_motor_off(&drive);
            floppy_select(&drive, false);
        }
        break;
    case 's':
        dbg_result(drive.selected ? "deselect" : "select",
                   floppy_select(&drive, !drive.selected));
        break;
    case 'm':
        if (drive.motor_on) {
            dbg_result("motor off", floppy_motor_off(&drive));
        } else {
            printf("motor on, waiting up to 2 s for a stable index...\n");
            dbg_result("motor on", floppy_motor_on(&drive));
        }
        break;
    case 'i': {
        uint32_t start = dbg_index_edges;
        sleep_ms(2000);
        uint32_t n = dbg_index_edges - start;
        printf("index: %lu pulse(s) in 2 s, last period %lu ms%s\n", (unsigned long)n,
               (unsigned long)(dbg_index_period_us / 1000u),
               drive.motor_on ? "" : " (motor is OFF)");
        break;
    }
    case '+':
        dbg_result("step in", floppy_debug_step(&drive, true));
        break;
    case '-':
        dbg_result("step out", floppy_debug_step(&drive, false));
        break;
    case '0':
        dbg_result("home", floppy_debug_home(&drive));
        break;
    case 't':
        dbg_stats();
        break;
    case 'B':
        printf("rebooting into BOOTSEL\n");
        // Park the drive and stop serving: no operation can start under
        // the reboot core 0 is about to do.
        floppy_motor_off(&drive);
        floppy_select(&drive, false);
        drive_debug_bootsel_request = true;
        for (;;) sleep_ms(1000);
    case 'P':
        printf("drive power cycle requested\n");
        drive_debug_power_cycle_request = true;
        break;
    case 'w':
        dbg_swap_stats();
        break;
    case 'f':
        if (media_state == MEDIA_READY) {
            // dbg_flux switches the drive format: never under a mounted disk.
            printf("'f': a disk is mounted (eject or swap it first)\n");
            break;
        }
        for (unsigned i = 0; i < floppy_probe_candidate_count; i++) {
            dbg_flux(floppy_probe_candidates[i]);
        }
        floppy_motor_off(&drive);
        floppy_select(&drive, false);
        break;
    default:
        break;
    }
}

void drive_debug_init(void) {
    gpio_set_irq_enabled_with_callback(drive.pins.index, GPIO_IRQ_EDGE_FALL, true,
                                       dbg_index_irq);
    dbg_status_ms = to_ms_since_boot(get_absolute_time());
    printf("debug build: status every second, '?' for monitor keys\n");
}

void drive_debug_poll(void) {
    int c = getchar_timeout_us(0);
    if (c != PICO_ERROR_TIMEOUT) dbg_command(c);
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (dbg_periodic && now - dbg_status_ms >= 1000u) dbg_status();
}

bool drive_debug_auto_probe(void) {
    return dbg_auto;
}

#endif // FLOPPY_DEBUG
