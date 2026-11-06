# References

Documents and projects this work relies on. Links were checked when this
page was written; old drive manuals in particular tend to move.

## Drive

- YE Data YD-380 5.25" half-height floppy drive, maintenance manual:
  <http://www.vintagecomputer.net/fjkraan/comp/divcomp/doc/YE_Data_YD-380_5.25inchHHHDFloppy.pdf>.
  Its test-point references do not match the PN128206-01 Rev. B board used
  here.
- YD-380B jumper settings for board PN128206-01 Rev. B:
  <https://wiki.joschy.net>.
- TEAC FD-55GFR specification:
  <https://deramp.com/downloads/floppy_drives/teac/TEAC%20FD-55GFR.pdf>.
  The YD-380 manual gives no power figures; this one gave the 12 V and 5 V
  currents, ripple limits and DISK CHANGE behaviour used for the design.
- NEC µPD765 floppy disk controller datasheet (widely mirrored): the
  sector-write behaviour (WRITE DATA) and the IBM System/34 MFM track
  layout that the firmware reproduces.

## USB

- USB Mass Storage Class, UFI Command Specification 1.0:
  <https://www.usb.org/sites/default/files/usbmass-ufi10.pdf>
- USB Mass Storage Class, Bulk-Only Transport 1.0:
  <https://www.usb.org/sites/default/files/usbmassbulk_10.pdf>
- Raspberry Pi USB PID allocation: <https://github.com/raspberrypi/usb-pid>

## Microcontroller

- RP2350 datasheet: <https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf>
- Raspberry Pi Pico 2 datasheet: <https://datasheets.raspberrypi.com/pico/pico-2-datasheet.pdf>
- Raspberry Pi Pico SDK: <https://github.com/raspberrypi/pico-sdk>
- TinyUSB: <https://github.com/hathach/tinyusb>

## Power and interface parts

- TI LM2678 (12 V regulator, also the source of the simulation model):
  <https://www.ti.com/product/LM2678>
- TI LM2676 (5 V regulator): <https://www.ti.com/product/LM2676>
- TI SN7407 (open-collector buffer): <https://www.ti.com/product/SN7407>
- Infineon IRF9540N (drive power switch):
  <https://www.infineon.com/cms/en/product/power/mosfet/p-channel/irf9540n/>

Datasheets for the other parts (Vishay SB560, Littelfuse P6KE series,
Bourns MF-R300 and 4600X networks, Feryster DTMSS toroids, Panasonic FR/FC
capacitors) are on the manufacturers' sites under the part numbers given
in the [bill of materials](hardware.md#bill-of-materials).

## Related projects

- pico-mfm-floppy, MFM floppy access from a Raspberry Pi Pico, the
  starting point of this firmware: <https://github.com/jackdoe/pico-mfm-floppy>
- Adafruit_Floppy, floppy library for RP2040/SAMD with a USB mass
  storage example: <https://github.com/adafruit/Adafruit_Floppy>
- Greaseweazle, flux-level imaging tool, the right tool for disks this
  device cannot read: <https://github.com/keirf/greaseweazle>
- KiCad-Spice-Library, source of the IRF9540N model:
  <https://github.com/kicad-spice-library/KiCad-Spice-Library>
