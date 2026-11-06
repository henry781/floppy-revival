#!/bin/sh
# Build and run the host-side tests (no Pico toolchain needed). Binaries go
# to a private temporary directory, removed on exit.
set -e
cd "$(dirname "$0")"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

cc -Wall -Wextra -O2 -o "$OUT/mfm_roundtrip" \
    mfm_roundtrip.c ../src/floppy/mfm.c ../src/floppy/crc.c ../src/floppy/format.c
"$OUT/mfm_roundtrip"

cc -Wall -Wextra -O2 -o "$OUT/mfm_jitter" \
    mfm_jitter.c ../src/floppy/mfm.c ../src/floppy/crc.c ../src/floppy/format.c
"$OUT/mfm_jitter"

cc -Wall -Wextra -O2 -o "$OUT/cache_test" \
    cache_test.c ../src/floppy/cache.c ../src/floppy/format.c
"$OUT/cache_test"

cc -Wall -Wextra -O2 -I shims -o "$OUT/msc_test" \
    msc_test.c ../src/msc_disk.c
"$OUT/msc_test"

cc -Wall -Wextra -O2 -I ../src -o "$OUT/vdisk_test" \
    vdisk_test.c ../src/virtual_disk.c
"$OUT/vdisk_test"

# The vendored TinyUSB MSC class driver, compiled as is: needs the TinyUSB
# headers of the pico-sdk the firmware builds with.
TUSB_SRC="${PICO_SDK_PATH:-$HOME/pico-sdk}/lib/tinyusb/src"
if [ ! -f "$TUSB_SRC/tusb_option.h" ]; then
    echo "TinyUSB headers not found in $TUSB_SRC (set PICO_SDK_PATH)" >&2
    exit 1
fi
cc -Wall -Wextra -O2 -I tusb_host -I "$TUSB_SRC" -o "$OUT/msc_device_test" \
    msc_device_test.c
"$OUT/msc_device_test"
