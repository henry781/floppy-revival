cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(floppy_revival C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

# The MSC class driver is our own copy of TinyUSB's (src/tinyusb/msc_device.c,
# two changes, see tools/tinyusb-msc_device.patch): drop the SDK's copy from
# the device library so the two never meet in the link.
get_target_property(_tusb_sources tinyusb_device_base INTERFACE_SOURCES)
list(FILTER _tusb_sources EXCLUDE REGEX "class/msc/msc_device\\.c$")
set_target_properties(tinyusb_device_base PROPERTIES INTERFACE_SOURCES "${_tusb_sources}")

set(COMMON_SOURCES
    src/main.c
    src/usb_descriptors.c
    src/msc_disk.c
    src/tinyusb/msc_device.c
)

# Warnings on our own sources only (the SDK and the vendored TinyUSB driver
# keep their upstream flags); the host tests build with the same set.
set_source_files_properties(
    src/main.c src/usb_descriptors.c src/msc_disk.c
    src/virtual_disk.c src/blockdev_vdisk.c
    src/floppy/format.c src/floppy/crc.c src/floppy/mfm.c src/floppy/floppy.c
    src/floppy/cache.c src/floppy/drive.c src/floppy/drive_debug.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# USB identity. 0xCafe is TinyUSB's test VID and MUST NOT ship: request a
# PID under Raspberry Pi's VID 0x2E8A (github.com/raspberrypi/usb-pid) and
# configure -DFLOPPY_USB_VID=0x2E8A -DFLOPPY_USB_PID=<allocated>.
# Release builds: -DFLOPPY_RELEASE=ON makes the test identity a configure
# error instead of a warning (both VID and PID must be set, to values other
# than the defaults below).
set(FLOPPY_USB_VID 0xCafe CACHE STRING "USB vendor ID")
set(FLOPPY_USB_PID 0x5250 CACHE STRING "USB product ID")
option(FLOPPY_RELEASE "Release build: refuse the default (test) USB VID/PID" OFF)
math(EXPR _floppy_vid "${FLOPPY_USB_VID}")
math(EXPR _floppy_pid "${FLOPPY_USB_PID}")
if(_floppy_vid EQUAL 51966 OR _floppy_pid EQUAL 21072) # 0xCAFE, 0x5250
    if(FLOPPY_RELEASE)
        message(FATAL_ERROR "FLOPPY_RELEASE: USB VID/PID still use a test default "
            "(${FLOPPY_USB_VID}/${FLOPPY_USB_PID}); configure -DFLOPPY_USB_VID=0x2E8A "
            "-DFLOPPY_USB_PID=<allocated> (see README, USB identity)")
    endif()
    message(WARNING "USB VID/PID ${FLOPPY_USB_VID}/${FLOPPY_USB_PID} include TinyUSB's test values: fine on the bench, not for a release (see README, USB identity)")
endif()

function(configure_firmware target)
    target_include_directories(${target} PRIVATE src src/floppy)
    # Stack guards (MPU, both cores): an overflow faults (and the watchdog
    # resets) instead of silently overwriting the memory below the stack.
    target_compile_definitions(${target} PRIVATE
        FLOPPY_USB_VID=${FLOPPY_USB_VID} FLOPPY_USB_PID=${FLOPPY_USB_PID}
        PICO_USE_STACK_GUARDS=1)
    target_link_libraries(${target} pico_stdlib pico_unique_id tinyusb_device
                          hardware_watchdog)
    # USB is owned by the MSC device; debug output goes to UART0 (GP0/GP1).
    pico_enable_stdio_usb(${target} 0)
    pico_enable_stdio_uart(${target} 1)
    pico_add_extra_outputs(${target})
endfunction()

# Real 5.25" drive backend: floppy_revival (default firmware) and
# floppy_revival_debug (same, plus UART status/tracing/monitor for bring-up).
# Renamed option: refuse the old name rather than silently re-enable read-ahead.
if(DEFINED DRIVE_READ_AHEAD)
    message(FATAL_ERROR "DRIVE_READ_AHEAD was renamed FLOPPY_READ_AHEAD")
endif()

function(add_drive_firmware target)
    add_executable(${target}
        ${COMMON_SOURCES}
        src/floppy/format.c
        src/floppy/crc.c
        src/floppy/mfm.c
        src/floppy/floppy.c
        src/floppy/cache.c
        src/floppy/drive.c
        src/floppy/drive_debug.c
    )
    pico_generate_pio_header(${target} ${CMAKE_CURRENT_LIST_DIR}/src/floppy/flux_read.pio)
    pico_generate_pio_header(${target} ${CMAKE_CURRENT_LIST_DIR}/src/floppy/flux_write.pio)
    configure_firmware(${target})
    target_link_libraries(${target} hardware_pio hardware_dma hardware_adc pico_multicore)

    # 48tpi (360KB-only) drive: -DFLOPPY_DRIVE_DD=1. Pin 34 must be DISK
    # CHANGE (drives with READY there are not supported).
    if(FLOPPY_DRIVE_DD)
        target_compile_definitions(${target} PRIVATE FLOPPY_DRIVE_DD=1)
    endif()
    # Status LEDs (-1 disables): -DFLOPPY_LED_ACTIVITY_PIN=-1 / -DFLOPPY_LED_DIRTY_PIN=-1.
    # Read-only jumper pin: -DFLOPPY_READONLY_PIN=-1 to disable.
    # Hard read-only build (imaging precious disks): -DFLOPPY_FORCE_READ_ONLY=1.
    # Density line polarity: -DFLOPPY_DENSITY_HD_RELEASED=0 if the drive is inverted.
    # Drive power switch (rev B/C boards): -DFLOPPY_DRIVE_POWER_PIN=-1 on rev A /
    # external PSU; -DFLOPPY_DRIVE_POWER_SETTLE_MS=<ms> to tune the rail settle delay.
    # Read-ahead of the next track on sequential reads: -DFLOPPY_READ_AHEAD=0 off.
    foreach(opt FLOPPY_LED_ACTIVITY_PIN FLOPPY_LED_DIRTY_PIN FLOPPY_READONLY_PIN
            FLOPPY_FORCE_READ_ONLY FLOPPY_DENSITY_HD_RELEASED
            FLOPPY_DRIVE_POWER_PIN FLOPPY_DRIVE_POWER_SETTLE_MS FLOPPY_READ_AHEAD)
        if(DEFINED ${opt})
            target_compile_definitions(${target} PRIVATE ${opt}=${${opt}})
        endif()
    endforeach()
endfunction()

add_drive_firmware(floppy_revival)
add_drive_firmware(floppy_revival_debug)
target_compile_definitions(floppy_revival_debug PRIVATE FLOPPY_DEBUG=1)
# The drive firmwares run from RAM (copied at boot, ~230 KB of 520 KB): the
# flux decoder on core 1 then never waits on the XIP cache that core 0
# (TinyUSB) shares. Bench 2026-10-18, macOS erase of a 360KB disk: worst
# decoder lag 103 ring words from flash, 4 from RAM (ring = 1024). Also
# takes cache misses out of the /WGATE timing of sector writes.
pico_set_binary_type(floppy_revival copy_to_ram)
pico_set_binary_type(floppy_revival_debug copy_to_ram)

# RAM-disk backend (Milestone 0): USB/UFI testing with no drive hardware.
add_executable(floppy_revival_vdisk
    ${COMMON_SOURCES}
    src/virtual_disk.c
    src/blockdev_vdisk.c
)
configure_firmware(floppy_revival_vdisk)
