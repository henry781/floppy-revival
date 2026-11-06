#!/usr/bin/env python3
"""Low-level format of a 5.25" disk in the FloppyRevival drive, from any host.

The device formats only on the host's request (UFI FORMAT UNIT, one track
per command), which Windows and Linux issue natively but macOS never does.
This tool talks to the device directly over USB (pyusb + libusb), the way a
format utility would, and sends the same commands:

    ufi_format.py --info                 # media state and formattable capacities
    ufi_format.py --format 1200          # 1.2 MB (also 720, 360; or a block count)
    ufi_format.py --format 360 --yes     # no confirmation prompt

It must take the USB interface away from the operating system's disk
driver: on macOS that needs root (sudo) and the volume unmounted (done here
with diskutil); on Linux run as root or with a udev rule. When done, the
device is reset so the OS picks it up again; the host then sees a disk with
no file system (make one with Disk Utility / mkfs.fat / format).

Requires: pip install pyusb; libusb (brew install libusb / apt install libusb-1.0-0).
"""
import argparse
import os
import platform
import struct
import subprocess
import sys
import time

try:
    import usb.core
    import usb.util
    import usb.backend.libusb1
except ImportError:
    sys.exit("pyusb missing: pip3 install pyusb (then, under sudo, keep PYTHONPATH / use the same python3)")

CAPACITIES = {  # blocks: (name, cylinders, heads, sectors)
    2400: ("1.2 MB", 80, 2, 15),
    1440: ("720 KB (96 tpi DD)", 80, 2, 9),
    720: ("360 KB", 40, 2, 9),
}
KB_TO_BLOCKS = {1200: 2400, 720: 1440, 360: 720}

SENSE_TEXT = {
    (0x02, 0x3A, 0x00): "medium not present",
    (0x02, 0x04, 0x01): "becoming ready",
    (0x03, 0x30, 0x01): "cannot read medium - unknown format (unformatted)",
    (0x03, 0x31, 0x01): "format command failed",
    (0x03, 0x03, 0x00): "write fault (a previous write did not reach the disk)",
    (0x05, 0x26, 0x00): "invalid field in parameter list (capacity not formattable here)",
    (0x05, 0x24, 0x00): "invalid field in command",
    (0x06, 0x28, 0x00): "media changed",
    (0x07, 0x27, 0x00): "write protected",
}


class CheckCondition(Exception):
    def __init__(self, key, asc, ascq):
        self.key, self.asc, self.ascq = key, asc, ascq
        text = SENSE_TEXT.get((key, asc, ascq), "")
        super().__init__(f"CHECK CONDITION {key:02X}/{asc:02X}/{ascq:02X} {text}")


class Ufi:
    def __init__(self, dev):
        self.dev = dev
        self.tag = 1
        cfg = dev.get_active_configuration()
        intf = cfg[(0, 0)]
        self.intf = intf.bInterfaceNumber
        self.ep_out = usb.util.find_descriptor(
            intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
        self.ep_in = usb.util.find_descriptor(
            intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
        if self.ep_out is None or self.ep_in is None:
            raise SystemExit("no bulk endpoints on interface 0")

    def command(self, cdb, data_out=None, data_in=0, timeout=90000):
        """Bulk-only transport: CBW, data, CSW. Returns the data read."""
        cdb = bytes(cdb) + bytes(16 - len(cdb))
        length = len(data_out) if data_out is not None else data_in
        flags = 0x80 if data_in else 0x00
        tag = self.tag
        self.tag = (self.tag + 1) & 0xFFFFFFFF
        cbw = struct.pack("<4sIIBBB16s", b"USBC", tag, length, flags, 0, 12, cdb)
        self.dev.write(self.ep_out.bEndpointAddress, cbw, timeout)
        got = b""
        if data_out:
            self.dev.write(self.ep_out.bEndpointAddress, data_out, timeout)
        elif data_in:
            try:
                got = bytes(self.dev.read(self.ep_in.bEndpointAddress, data_in, timeout))
            except usb.core.USBError as e:
                if e.errno not in (32, None):  # EPIPE: stalled data phase, CSW follows
                    raise
                self.dev.clear_halt(self.ep_in.bEndpointAddress)
        csw = self.read_csw(timeout)
        sig, rtag, residue, status = struct.unpack("<4sIIB", csw)
        if sig != b"USBS" or rtag != tag:
            raise SystemExit(f"bad CSW {csw.hex()}")
        if status == 1:
            sense = self.request_sense()
            raise CheckCondition(sense[2] & 0x0F, sense[12], sense[13])
        if status != 0:
            raise SystemExit(f"CSW phase error {status}")
        return got

    def read_csw(self, timeout):
        # A device that sent less data than the host asked for stalls the IN
        # endpoint first (BOT case 5); the CSW follows once the halt is cleared.
        for _ in range(2):
            try:
                return bytes(self.dev.read(self.ep_in.bEndpointAddress, 13, timeout))
            except usb.core.USBError as e:
                if e.errno != 32:
                    raise
                self.dev.clear_halt(self.ep_in.bEndpointAddress)
        return bytes(self.dev.read(self.ep_in.bEndpointAddress, 13, timeout))

    def request_sense(self):
        cdb = bytes([0x03, 0, 0, 0, 18, 0, 0, 0, 0, 0, 0, 0])
        cbw = struct.pack("<4sIIBBB16s", b"USBC", self.tag, 18, 0x80, 0, 12, cdb + bytes(4))
        self.tag += 1
        self.dev.write(self.ep_out.bEndpointAddress, cbw, 5000)
        data = bytes(self.dev.read(self.ep_in.bEndpointAddress, 18, 5000))
        self.read_csw(5000)
        return data

    def test_unit_ready(self):
        """True when ready; absorbs one UNIT ATTENTION. Raises otherwise."""
        for _ in range(3):
            try:
                self.command(bytes(12))
                return True
            except CheckCondition as e:
                if e.key == 0x06:
                    continue
                raise
        return False

    def read_format_capacities(self):
        data = self.command(bytes([0x23, 0, 0, 0, 0, 0, 0, 0, 64, 0, 0, 0]), data_in=64)
        n = data[3] // 8
        descs = []
        for i in range(n):
            d = data[4 + 8 * i:12 + 8 * i]
            blocks = struct.unpack(">I", d[:4])[0]
            code = d[4] & 0x03
            block_len = struct.unpack(">I", b"\0" + d[5:8])[0]
            descs.append((blocks, code, block_len))
        return descs

    def format_track(self, blocks, track, side):
        cdb = bytes([0x04, 0x17, track, 0, 0, 0, 0, 0, 12, 0, 0, 0])
        header = bytes([0x00, 0xB0 | (side & 1), 0x00, 0x08])
        desc = struct.pack(">IB", blocks, 0) + struct.pack(">I", 512)[1:]
        self.command(cdb, data_out=header + desc)


def find_device(vid, pid):
    backend = None
    if platform.system() == "Darwin":
        for lib in ("/opt/homebrew/lib/libusb-1.0.dylib", "/usr/local/lib/libusb-1.0.dylib"):
            if os.path.exists(lib):
                backend = usb.backend.libusb1.get_backend(find_library=lambda x, lib=lib: lib)
                break
    dev = usb.core.find(idVendor=vid, idProduct=pid, backend=backend)
    if dev is None:
        sys.exit(f"no USB device {vid:04x}:{pid:04x} (is the drive plugged in? --vid/--pid)")
    return dev


def unmount_macos(vid, pid):
    """Unmount the volume the OS may hold on the device (best effort)."""
    try:
        out = subprocess.run(["diskutil", "list", "external", "physical"], capture_output=True, text=True).stdout
    except FileNotFoundError:
        return
    for line in out.splitlines():
        if line.startswith("/dev/disk"):
            disk = line.split()[0]
            info = subprocess.run(["diskutil", "info", disk], capture_output=True, text=True).stdout
            if "Floppy" in info or "FLOPREV" in info:
                subprocess.run(["diskutil", "unmountDisk", disk], capture_output=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vid", type=lambda x: int(x, 0), default=0xCafe)
    ap.add_argument("--pid", type=lambda x: int(x, 0), default=0x5250)
    ap.add_argument("--info", action="store_true", help="show media state and capacities")
    ap.add_argument("--format", type=int, metavar="KB|BLOCKS", help="format: 1200, 720, 360 (KB) or a block count")
    ap.add_argument("--yes", action="store_true", help="no confirmation prompt")
    ap.add_argument("--no-reset", action="store_true", help="leave the device detached from the OS afterwards")
    a = ap.parse_args()
    if not a.info and a.format is None:
        ap.error("--info or --format needed")

    # One instance at a time: a second one fighting over the bulk endpoints
    # leaves both waiting on transfers that never complete.
    import fcntl
    lock = open("/tmp/ufi_format.lock", "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        sys.exit("another ufi_format.py is running (stop it first: sudo pkill -f ufi_format.py)")
    if platform.system() == "Darwin":
        if os.geteuid() != 0:
            sys.exit("macOS: run with sudo (the disk driver must release the device)")
        unmount_macos(a.vid, a.pid)
    dev = find_device(a.vid, a.pid)
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except (usb.core.USBError, NotImplementedError) as e:
        print(f"note: kernel driver detach: {e}", file=sys.stderr)
    try:
        dev.set_configuration()
    except usb.core.USBError:
        pass
    usb.util.claim_interface(dev, 0)
    ufi = Ufi(dev)

    try:
        state = "ready"
        try:
            ufi.test_unit_ready()
        except CheckCondition as e:
            state = str(e)
        caps = ufi.read_format_capacities()
        print(f"medium: {state}")
        for blocks, code, block_len in caps:
            kind = {1: "unformatted, maximum", 2: "current (formatted)", 3: "no medium, maximum"}.get(code, "formattable")
            name = CAPACITIES.get(blocks, ("?",))[0]
            print(f"  {blocks:5d} blocks x {block_len}  {name:22s} {kind}")
        if a.format is None:
            return
        blocks = KB_TO_BLOCKS.get(a.format, a.format)
        if blocks not in CAPACITIES:
            sys.exit(f"unknown capacity {a.format}; use 1200, 720, 360 or a listed block count")
        if blocks not in [c[0] for c in caps]:
            sys.exit(f"{blocks} blocks is not offered by the device for this medium")
        name, cyls, heads, spt = CAPACITIES[blocks]
        if not a.yes:
            answer = input(f"FORMAT the disk as {name} ({cyls} cylinders x {heads} sides)? Everything on it is lost. [y/N] ")
            if answer.strip().lower() != "y":
                sys.exit("aborted")
        t0 = time.time()
        for cyl in range(cyls):
            for side in range(heads):
                try:
                    ufi.format_track(blocks, cyl, side)
                except CheckCondition as e:
                    sys.exit(f"\ntrack {cyl} side {side}: {e}")
                done = cyl * heads + side + 1
                print(f"\r  track {cyl:2d} side {side}  {done}/{cyls * heads}  {time.time() - t0:5.1f} s", end="", flush=True)
        print(f"\nformatted as {name} in {time.time() - t0:.1f} s")
        ufi.test_unit_ready()
        print("ready: now create a file system on it (Disk Utility: Erase MS-DOS (FAT) / mkfs.fat / format)")
    finally:
        usb.util.release_interface(dev, 0)
        if not a.no_reset:
            try:
                dev.reset()  # hand the device back to the operating system
            except usb.core.USBError:
                pass


if __name__ == "__main__":
    main()
