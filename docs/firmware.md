# Firmware

The firmware runs on a Raspberry Pi Pico 2 (RP2350) and is built with
the Pico SDK. It makes a 5.25" drive look like a USB floppy drive. This
page explains how it is organised and why. For building and flashing, see
[building.md](building.md); for behaviour seen from the outside, see
[user-guide.md](user-guide.md).

## Source map

| Path | Role |
|---|---|
| `src/main.c` | Clocks, watchdog, starts both cores |
| `src/usb_descriptors.c`, `tusb_config.h` | USB descriptors: mass storage, UFI subclass, bulk-only transport |
| `src/msc_disk.c` | SCSI/UFI command layer: media states, sense codes, eject, FORMAT UNIT, READ FORMAT CAPACITIES |
| `src/tinyusb/msc_device.c` | TinyUSB's MSC class driver with two small changes (see below) |
| `src/blockdev.h` | Interface between the USB layer and a storage back end |
| `src/floppy/drive.c` | The drive back end on core 1: mailbox, media probe, write-back flushes, drive power |
| `src/floppy/floppy.c` | Drive control: select, motor, seek, flux capture and playback (PIO + DMA), sector writes |
| `src/floppy/mfm.c` | MFM encoder and streaming decoder, parameterised by data rate |
| `src/floppy/cache.c` | Write-back track cache with sector-granular flush and a bounded failure policy |
| `src/floppy/format.c` | The supported formats (geometry, data rate, gaps, precompensation) |
| `src/floppy/flux_read.pio`, `flux_write.pio` | PIO programs that time flux transitions and play them back |
| `src/floppy/pins.h` | GPIO assignment and board options |
| `src/floppy/drive_debug.c` | Bench monitor on the UART, debug build only |
| `src/virtual_disk.c`, `blockdev_vdisk.c` | RAM disk back end for testing USB without a drive |
| `tests/` | Host-side tests, run on a PC |
| `tools/ufi_format.py` | Low-level format from macOS (see the user guide) |

## Two cores, one mailbox

Core 0 runs TinyUSB and nothing that can block. Core 1 owns the drive.
When the host asks for a sector, the USB read or write callback posts a
one-sector request in a mailbox and returns 0, which in TinyUSB means "not
ready yet, call me again". Core 0 keeps servicing USB while core 1 seeks,
waits for the spindle or retries a read, and the host never sees a stalled
device. A floppy operation can take seconds, far longer than USB tolerates
a callback being blocked.

The drive builds run from RAM (`copy_to_ram`). With code in flash, a cache
miss on one core stalls the other on the shared XIP cache; measured on the
bench, the flux decoder fell up to 103 words behind its ring buffer from
flash and 4 words from RAM, for a 1024-word ring. Running from RAM also
takes cache misses out of the write-gate timing.

## Reading

`flux_read.pio` counts PIO clock cycles between falling edges of /RDATA
and pushes each count into a FIFO. A DMA channel drains the FIFO into a
ring buffer in RAM, so no flux transition is lost to interrupt latency.
The system clock is 144 MHz and the PIO counts at 72 MHz, which keeps USB
on its own 48 MHz PLL.

The MFM decoder consumes the ring as the track goes by. It classifies
each interval as 2, 3 or 4 half-cells, finds the A1 sync marks, then
decodes ID and data fields and checks their CRC. A read starts as soon as
the head is on the track, without waiting for the index pulse, and stops
once every sector of the track is in, so a full track costs about one
revolution. The cell time is a runtime parameter: the same decoder handles
250 kbps (360 KB in a 300 RPM drive), 300 kbps (360 KB in a 360 RPM drive)
and 500 kbps (1.2 MB). The host tests decode full tracks with plus or minus 12.5 % of a bit
cell of random timing jitter, and check that at 35 % no corrupted sector
is ever accepted.

### Media probe

The drive cannot tell the firmware what kind of disk is in it: 5.25" disks
have no density hole. The firmware tries each format on track 0 in turn
(1.2 MB at 500 kbps, then 720 KB and 360 KB at 300 kbps). The wrong data
rate fails pulse classification at once, so a wrong match is not a real
risk, and a candidate that shows no sync mark for a full revolution is
dropped without retries. For 360 KB the boot sector's sector count decides
between 360 KB (double-stepped in a 96 tpi drive) and 720 KB.

A disk that spins but reads in no format is reported to the host as
unformatted. It is never mounted at a guessed density: a disk from another
kind of computer must not be "initialised" by a host because it looked
blank.

## Writing

Writes are per sector, the way a PC floppy controller (NEC µPD765 WRITE
DATA) does them. The firmware reads the track until the ID field of the
target sector passes, opens /WGATE at the end of gap 2, timed from the
flux timestamps, and plays out a data field that was MFM-encoded in
advance. Several sectors of the same track go in one revolution. The
track is then read back and every sector the cache knows is compared.

Why not rewrite whole tracks, which is simpler? Because a whole-track
write needs every sector of the track. One unreadable sector the host
never touched would block the whole track forever, and a power cut or a
pulled disk would expose fifteen sectors instead of one. Sector writes
also keep the original layout of a PC-formatted disk. Whole-track writes
are used only by FORMAT.

Write precompensation (125 ns, from cylinder 40 for 1.2 MB and 720 KB,
from cylinder 20 for 360 KB) shifts transitions that the medium would otherwise pull
together on inner tracks.

### Write-back cache

Six tracks are cached. Host writes go into the cache and the red LED
lights; the cache is flushed about 1.5 s after the last write, on
SYNCHRONIZE CACHE and on eject. Eject and SYNCHRONIZE CACHE wait for the
flush to finish before answering, so "safely remove" really means the data
is on the disk. macOS sends the eject once and does not retry a "not
ready" answer, so this wait is the only way to honour it.

A sector that still fails after three flushes, each with the drive's own
retries and a recalibration, is dropped. The host then gets one MEDIUM
ERROR (write fault) on its next write, sync or eject, and the red LED
blinks at 2 Hz. Earlier versions retried forever; that wore the disk and
could block an eject indefinitely, which is worse than a reported error.

Write protection always wins: covering the notch or closing the read-only
jumper with writes still cached never lets them through. They fail like a
refused sector and are reported the same way.

## Media changes

The PC interface has no "disk inserted" signal. Pin 34 (DISK CHANGE)
latches when a disk is removed and clears on a step pulse once a disk is
in. With the drive empty, the firmware keeps the head on track 0 and sends
one step pulse outward there, which the drive ignores mechanically but
which clears the latch if a disk has been inserted. The head never moves
and the motor never starts while the drive is empty.

Each media change carries a sequence number through to the USB layer.
The host gets a UNIT ATTENTION even if the window between two disks was
shorter than its polling interval, and a disk swapped under a mounted
volume makes the device drop off USB for about a second so the host
cannot keep using cached data from the old disk.

## Drive power

GP18 switches the drive's 12 V and 5 V (see [hardware.md](hardware.md#power)).
At every boot the firmware holds the drive off for 3.5 s: the input
capacitors must discharge below the regulators' lockout, or the fixed
regulators restart hard and overshoot, and the drive gets a clean
power-on reset. It powers the drive only when the adapter, measured on
GP26, is above 16 V, and drops it within 50 ms below 15 V. Otherwise the
drive stays on: switching it off would re-arm DISK CHANGE, which the host
would see as a disk swap.

## Watchdog

An 8 s hardware watchdog is fed by core 0. Core 0 stops feeding it when
core 1's heartbeat, refreshed at the start of every drive operation, is
older than 22 s (the longest write with its retries is allowed 20 s). A
hung USB core is reset within 8 s, a hung drive core within about 30 s.
The reset releases every drive line through the pull-ups, so the write
gate cannot stay asserted. Writes still in the cache are lost; the UART
log says `recovered by watchdog`.

Stack guards are enabled on both cores: an overflow faults and resets
instead of overwriting memory.

## The modified TinyUSB driver

TinyUSB's MSC class driver handles READ FORMAT CAPACITIES itself and
replaces the sense data of a failed READ(10)/WRITE(10) with "medium not
present". A floppy drive needs both to reach the application: the format
list depends on the drive, and a bad sector must be reported as a medium
error, not as a missing disk (which makes the host unmount the volume).
`src/tinyusb/msc_device.c` is TinyUSB 0.18.0's file with those two
changes, marked in the source; `tools/tinyusb-msc_device.patch` is the
diff. CMake removes the SDK's copy from the link. When the SDK's TinyUSB
is updated, the patch has to be applied again to the new file.

## Host tests

`tests/run_host_tests.sh` builds and runs, with the host compiler:

- MFM round trips at all three data rates: full tracks with index mark
  and gaps, a track that fits one revolution, a rewritten sector behind a
  splice, precompensation that leaves the stream length unchanged;
- decoder behaviour under timing jitter;
- write-back cache semantics against a fake drive: merge, sector flush, a
  sector that never reads, the bounded failure policy, write protection,
  media change, eviction, read-ahead;
- the SCSI/UFI layer against a scripted back end: media states and sense
  codes, READ FORMAT CAPACITIES, FORMAT UNIT, a deferred write error
  reported once, eject, reconnect on swap, bus reset;
- the modified TinyUSB driver, compiled as is against the SDK's headers.

They do not replace bench tests: nothing here touches a real drive. The
bench test plan is in [testing.md](testing.md).
