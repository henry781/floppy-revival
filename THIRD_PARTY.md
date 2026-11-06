# Third-party code and material

This project reuses code and data from other projects. Each item keeps its
original licence. The notices below are reproduced as their licences
require.

## Code included in this repository

### pico-mfm-floppy (MIT)

<https://github.com/jackdoe/pico-mfm-floppy>, by jackdoe. The upstream
repository states "MIT" in its README and has no separate LICENSE file;
the MIT text in `LICENSES/MIT.txt` is applied to these files with the
copyright of their author.

The floppy back end started as a port of this project. Files derived from
it carry a header comment saying so:

| File | Relation to upstream |
|---|---|
| `firmware/src/floppy/crc.c`, `crc.h` | unchanged |
| `firmware/src/floppy/flux_read.pio` | small change to the wait loop |
| `firmware/src/floppy/flux_write.pio` | pin initialisation order changed |
| `firmware/src/floppy/mfm.c`, `mfm.h` | cell timing and geometry made runtime parameters, sector-level write support |
| `firmware/src/floppy/floppy.c`, `floppy.h` | 5.25" timings, density line, disk change handling, sector writes |
| `firmware/src/floppy/cache.c`, `cache.h`, `disk.h` | runtime geometry, sector-granular flush, bounded failure policy |

### TinyUSB (MIT)

<https://github.com/hathach/tinyusb>, Copyright (c) 2019 Ha Thach
(tinyusb.org). `firmware/src/tinyusb/msc_device.c` is a copy of TinyUSB
0.18.0 `src/class/msc/msc_device.c` with two changes, marked in the file;
the diff is `firmware/tools/tinyusb-msc_device.patch`. The file keeps its
original MIT header.

## Used at build time, not included

- **Raspberry Pi Pico SDK** (BSD-3-Clause),
  <https://github.com/raspberrypi/pico-sdk>. `firmware/pico_sdk_import.cmake`
  is the SDK's standard import script.
- **TinyUSB** (MIT), as the pico-sdk submodule.

## Consulted, not copied

- **Adafruit_Floppy** (MIT), <https://github.com/adafruit/Adafruit_Floppy>,
  Limor Fried and Jeff Epler for Adafruit Industries. Its format table,
  double-step detection and USB mass storage example informed the format
  probe and the USB layer; no code was taken.

## Simulation models

- `hardware/simulation/models/lm267x_sw.lib` is our own ngspice model of
  the LM267x. It follows the structure and parameter values of the LM2678
  PSpice transient model published by Texas Instruments (SNVM880, WEBENCH,
  2012). TI's model is not redistributed here, and neither is its internal
  switch subcircuit that the ringing study needs: download the model from
  the LM2678 product page (<https://www.ti.com/product/LM2678>, "Design &
  development", PSpice model) and see `hardware/simulation/README.md`.
- The IRF9540N model used by the switch-on simulations (Symmetry Design
  Systems / International Rectifier, from the KiCad-Spice-Library) carries a
  restrictive licence and is not included. See
  `hardware/simulation/README.md` for where to get it.

## KiCad libraries

Symbols, footprints and 3D models referenced from KiCad's standard
libraries are under the KiCad library licence (CC-BY-SA 4.0 with an
exception for use in designs), <https://www.kicad.org/libraries/license/>.
The project-specific parts in `hardware/kicad/lib/` were drawn for this
project and fall under CERN-OHL-P-2.0.
