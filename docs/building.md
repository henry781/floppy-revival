# Building and flashing the firmware

## Requirements

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.x with its TinyUSB
  submodule (`git submodule update --init` in the SDK). Developed with
  SDK 2.3.1, which ships TinyUSB 0.18.0.
- An `arm-none-eabi-gcc` toolchain with newlib. The Arm GNU Toolchain
  (developed with 14.3.rel1) or the xPack build both work. Homebrew's
  `arm-none-eabi-gcc` formula has no newlib and does not.
- CMake 3.13 or later, and a host C compiler (the SDK builds `pioasm` and
  `picotool` with it).

## Build

```sh
cd firmware
cmake -B build -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build -j
```

Add `-DPICO_TOOLCHAIN_PATH=/path/to/toolchain` if `arm-none-eabi-gcc` is
not on the `PATH`.

Three images come out in `build/`:

| File | What it is |
|---|---|
| `floppy_revival.uf2` | The firmware for the board. Logs on UART0. |
| `floppy_revival_debug.uf2` | Same, plus the bench monitor on UART0 (`?` lists the keys) and more tracing. Use it for bring-up. |
| `floppy_revival_vdisk.uf2` | A 360 KB RAM disk, no drive code. Use it to check the USB side on a bare Pico; it never switches the drive power on. |

CMake prints a warning about the USB VID/PID (see below). That is
expected for a local build.

## Build options

Pass them at configure time, for example `cmake -B build -DFLOPPY_DRIVE_DD=1 ...`.

| Option | Default | Effect |
|---|---|---|
| `FLOPPY_DRIVE_DD` | off | 48 tpi 360 KB-only drive instead of a 96 tpi 1.2 MB drive |
| `FLOPPY_FORCE_READ_ONLY` | 0 | 1 = refuse every write. For imaging disks you care about. |
| `FLOPPY_READ_AHEAD` | 1 | Fetch the next track while the host is still reading the current one |
| `FLOPPY_DENSITY_HD_RELEASED` | 1 | 0 if your drive expects the density line (pin 2) the other way round |
| `FLOPPY_LED_ACTIVITY_PIN`, `FLOPPY_LED_DIRTY_PIN` | 15, 16 | -1 to disable |
| `FLOPPY_READONLY_PIN` | 17 | Read-only jumper input, -1 to disable |
| `FLOPPY_DRIVE_POWER_PIN` | 18 | Drive power switch, -1 if the drive has its own supply |
| `FLOPPY_DRIVE_POWER_SETTLE_MS` | 1000 | Wait after switching the drive on before the first access |
| `FLOPPY_USB_VID`, `FLOPPY_USB_PID` | 0xCafe, 0x5250 | USB identity, see below |
| `FLOPPY_RELEASE` | OFF | ON makes the test VID/PID a configure error |

A note on 360 KB disks in a 1.2 MB drive: reading is fine, but writing
lays down narrow 96 tpi tracks. A real 360 KB drive, with its wider head,
may then read a mix of old wide tracks and new narrow ones unreliably.
For disks that must go back to a 40-track drive, start from a bulk-erased
disk.

The drive must output DISK CHANGE on pin 34. Drives jumpered to put READY
there are not supported, because the firmware detects disk swaps only
through DISK CHANGE.

## Flashing

Hold BOOTSEL on the Pico while plugging in the USB cable, then copy the
`.uf2` file to the `RP2350` drive that appears, or run
`picotool load -f build/floppy_revival.uf2`.

On the assembled board the BOOTSEL button is still reachable, and TP9
(RUN) lets you reset the Pico without unplugging: hold BOOTSEL, short TP9
to GND briefly, release BOOTSEL. Entering BOOTSEL lets GP18 float, so the
drive supply switches itself off. With the debug build, key `B` on the
UART reboots straight into BOOTSEL.

Eject the volume on the host before reflashing, and wait for the red LED
to go out: anything still in the write-back cache is lost otherwise.

## Debug UART

UART0 is on header J2 (GND, GP0 = TX, GP1 = RX), 115200 8N1, 3.3 V
logic. USB is used by the mass storage device, so there is no USB serial
console. Both drive builds log there; the debug build also accepts
single-key commands.

## USB identity

The default VID/PID, 0xCafe/0x5250, are TinyUSB's test values. They are
fine on your own bench and must not be used for devices given to other
people. Raspberry Pi allocates PIDs under its VID 0x2E8A for free to
RP2350-based projects, through a pull request to
<https://github.com/raspberrypi/usb-pid>. With an allocated PID:

```sh
cmake -B build -DFLOPPY_RELEASE=ON -DFLOPPY_USB_VID=0x2E8A -DFLOPPY_USB_PID=0x....
```

This project has not requested a PID.

## Host tests

The tests run on a PC with the system C compiler, no Pico needed:

```sh
PICO_SDK_PATH=/path/to/pico-sdk sh firmware/tests/run_host_tests.sh
```

`PICO_SDK_PATH` is used only to find the TinyUSB headers for the test of
the modified MSC driver. What the tests cover is described in
[firmware.md](firmware.md#host-tests).

## Updating TinyUSB

`firmware/src/tinyusb/msc_device.c` replaces the SDK's copy of TinyUSB's
MSC class driver. When you move to a newer SDK, apply
`firmware/tools/tinyusb-msc_device.patch` to the new upstream file, copy
the result over `src/tinyusb/msc_device.c`, and run the host tests.
