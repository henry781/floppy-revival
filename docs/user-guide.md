# User guide

What the device does from the outside: how disks mount, what the two LEDs
mean, how to write safely, how to archive a disk as an image, how to
format a blank disk on each operating system, and what the error messages
mean. Hardware and assembly are in
[hardware.md](hardware.md) and [assembly.md](assembly.md); the bench test
plan is in [testing.md](testing.md).

## Overview

Plug the adapter, then the USB cable. The drive powers up about four
seconds after the board (it is held off for 3.5 seconds at every start so
that its supply is fully discharged and it gets a clean power-on reset).
The drive is only powered while the adapter is present: pulling the
adapter switches it off cleanly, plugging it back restarts it after the
same 3.5 s. Insert a PC-formatted 5.25" disk (360KB, 720KB 96tpi or
1.2MB): it mounts as a normal removable floppy, usually within a few
seconds (up to about 30 s into a drive left empty for a while, see
"Changing disks").
Copy files as with any USB stick. Before taking the disk out, eject it from
the host **and wait for the red LED to be off** (the eject itself finishes
only once the cached writes are on the disk). Change disks whenever you
like, with the volume mounted or not: the drive notices and the host
remounts.

## LEDs

| Green (ACT) | Meaning |
|---|---|
| off | no readable disk |
| steady | a disk is mounted |
| flicker | the drive is working for the host (seek, read, write, verify) |

| Red (STATUS) | Meaning | What to do |
|---|---|---|
| off | nothing pending | — |
| steady | writes cached, not yet on the disk | wait; it goes out by itself within about two seconds after the last write, or on eject |
| 1 Hz | a disk is in but reads in no PC format | format it (below), or it is not a PC disk |
| 2 Hz | a write to this disk failed (or cached writes were lost with a pulled disk) | check the file you just wrote; use another disk |
| 4 Hz | drive fault: track 0 not found three times | check the drive's power and the ribbon cable; power-cycle |

## Writing

The firmware behaves like a floppy drive in a PC: writable unless the disk
says otherwise. Three things block writes, in order of convenience:

1. **The write-protect notch.** Cover it (5.25" disks: tab over the notch =
   protected). The host sees a read-only volume. This is the control to use
   for any disk you care about.
2. **The read-only jumper (J3).** Closed = the firmware refuses every write,
   whatever the host or the notch say. For imaging sessions (see
   [Archiving a disk](#archiving-a-disk)).
3. A read-only firmware build (`-DFLOPPY_FORCE_READ_ONLY=1`).

Modern hosts write to a volume as soon as it mounts (macOS: `.fseventsd`,
Spotlight indexes, `._` files; Windows: `System Volume Information`). A
vintage disk that must stay as it is goes in with the notch covered.

Writes are cached for up to about 1.5 s and then written sector by sector,
exactly where the sector lives on the disk, and read back. A sector the
disk refuses is tried again on the next two flushes (about 1.5 s apart);
after the third failure it is given up, and the host is told on its next
write, sync or eject, once, as a medium error (write fault); the operating
system then shows its usual "the disk could not be written" message, and
an eject goes through after that. The red LED blinks at 2 Hz until the
next disk is detected. A neighbouring sector that a write disturbed and
the drive could not restore at once is handled the same way: it is
rewritten from the cached copy, and reported if it still fails.

Covering the notch or closing J3 while writes are still cached (red LED
steady) does not let them through: nothing is ever written past a write
protection. The cached sectors fail like a refused sector above: given up
after three flush attempts (a few seconds when the host is idle, at once
if it is syncing or ejecting), reported once as a write error. To keep
them, uncover the notch (or open J3) before then. Host writes made
after the protection appeared are refused at once as "read-only".

Pulling a disk while the red LED is steady loses the cached writes, and
the red LED blinks at 2 Hz. With the volume mounted, the device drops off
USB for a second and the host reports the disk as removed without being
ejected: that is how the host learns of it. A sync or eject sent before
the next disk is detected gets the medium error; once the next disk is
reported to the host the pending error is discarded (it does not belong to
the new disk), and the LED goes back to normal when that disk is
recognised.

## Changing disks

Swap at any time. With a volume mounted, the device drops off USB for one
second and comes back with the new disk; the host remounts it. If the host
had unsaved writes for the old disk they are lost (see "Pulling a disk"
above).

An ejected disk ("Eject" in the host) is reported absent until the next
insertion, so the host does not remount it on its own.

**Empty drive.** The PC interface has no "disk inserted" signal: the
drive only reports, on pin 34, that a disk was *removed*, and that latch
clears on a head step pulse once a disk is in. The firmware therefore
keeps the head parked on track 0 (it goes back there each time the motor
stops) and, with the drive empty, checks with one step pulse there, which
the drive ignores: the head never moves and the motor never spins. You
see a very short blink of the drive's front LED and may hear a faint tick
at each check: every 5 s during the minute after a disk was removed, then
every 30 s. A disk inserted after that minute can therefore take up to
30 s to mount. Commercial USB floppy drives behave the same way, for the
same reason.

## Archiving a disk

To keep a copy of a disk, read it as a raw image: every sector in order,
512 bytes each, the same layout emulators and `mtools` expect. Copying the
files only is not enough, because it loses the boot sector, deleted
files and anything outside the file system.

1. **Close J3 and cover the notch.** J3 makes the firmware refuse every
   write, whatever the host does. The host then mounts the volume
   read-only, so nothing is written when it mounts (`.fseventsd`,
   `System Volume Information`...). With the notch covered as well, a
   wrong jumper does not put the disk at risk. For a session of many
   disks, a `-DFLOPPY_FORCE_READ_ONLY=1` build does the same as J3
   ([building.md](building.md#build-options)).
2. **Insert the disk** and wait for the green LED to stay on.
3. **Find the device and unmount the volume, but do not eject it.** An
   ejected disk is reported absent until it is reinserted, and reading
   it fails.
4. **Read it with `dd`**, one 512-byte sector at a time, and keep going
   past errors.
5. **Check the image.** Its size gives the format: 368 640 bytes is
   360 KB, 737 280 bytes is 720 KB, 1 228 800 bytes is 1.2 MB. Take the
   disk out, put it back and read it a second time, then compare the
   checksums. Reinserting makes the second read come from the disk and
   not from the firmware's track cache.

**macOS:**

```sh
diskutil list                         # the floppy is a ~1.2 MB diskN with no partitions
diskutil unmountDisk /dev/diskN       # unmount, do not eject
sudo dd if=/dev/rdiskN of=disk.img bs=512 conv=noerror,sync
shasum -a 256 disk.img
```

`/dev/rdiskN` (the raw device) reads faster than `/dev/diskN`. The volume
stays unmounted until the next disk change.

**Linux:**

```sh
lsblk                                 # the floppy is a ~1.2M sdX with no partitions
sudo umount /dev/sdX
sudo dd if=/dev/sdX of=disk.img bs=512 conv=noerror,sync status=progress
sha256sum disk.img
```

**Windows** has no built-in raw-copy tool. Use any program that reads a
physical drive into an image file, with the volume read-only (J3).

**Bad sectors.** For each sector the firmware reads up to four
revolutions. If the sector is still missing, it moves the head away and
back twice and reads again. A sector that still does not read takes
several seconds and comes back as an I/O error. `conv=noerror,sync` makes
`dd` write zeros in its place and carry on, which is why `bs=512`: with a
larger block size, one bad sector would turn the whole block into zeros.
`dd` prints the error but not which sector it was. For a damaged disk, GNU
`ddrescue` is better: it keeps a map of the sectors it could not read, and
later runs retry only those (for example after cleaning the head):

```sh
sudo ddrescue -b 512 -r 3 /dev/sdX disk.img disk.map    # macOS: brew install ddrescue, /dev/rdiskN
```

What this cannot image: a disk the firmware does not mount. That means
foreign formats (FM, Amiga, Apple II, copy-protected tracks) and a PC disk
with an unreadable sector on track 0 side 0, both reported as unformatted
(red 1 Hz). Those need a flux-level imaging tool.

## Formatting a blank or foreign disk

A disk none of the three formats reads is reported to the host as an
**unformatted medium** (red LED 1 Hz). It is never mounted at a guessed
density, so a disk from another machine (Amstrad, Apple II, Commodore...)
cannot be damaged by a host offering to initialise it: the host cannot
write to it at all until it has formatted it on purpose.

The drive cannot tell high-density from double-density media (5.25" disks
have no sensor hole), so the host chooses the capacity:

| Capacity offered | Media | Tracks |
|---|---|---|
| 1.2 MB (2400 blocks) | HD (1.2MB) disks | 80 cylinders, 15 sectors |
| 720 KB (1440 blocks) | DD 96tpi ("2S/4D", "QD") disks | 80 cylinders, 9 sectors |
| 360 KB (720 blocks) | DD (360KB) disks, written with narrow tracks | 40 cylinders, 9 sectors |

Formatting a DD disk as 1.2 MB fails during verify (the medium cannot hold
the flux density); the host reports a format error. A 360 KB format written
by this 96tpi drive reads back fine here and in other 96tpi drives; a
real 40-track drive may read it unreliably unless the disk was bulk-erased
first (see [building.md](building.md#build-options)).

- **Windows:** Explorer, right-click the drive, *Format...*, choose the
  capacity, untick *Quick Format* (a quick format only writes the file
  system and needs an already formatted disk). Windows sends one FORMAT
  UNIT per track, then writes the FAT. About 80 s for 1.2 MB.
- **Linux:** `ufiformat -f 1200 /dev/sdX` (or `-f 720`, `-f 360`), then
  `mkfs.fat /dev/sdX`.
- **macOS:** Disk Utility never low-level formats a floppy (measured: its
  Erase only writes a new file system, it never sends FORMAT UNIT), so a
  blank disk needs `firmware/tools/ufi_format.py`, which sends the same per-track
  commands as Windows:

  ```
  pip3 install pyusb && brew install libusb
  sudo python3 firmware/tools/ufi_format.py --info
  sudo python3 firmware/tools/ufi_format.py --format 1200   # or 720, 360
  diskutil eraseVolume MS-DOS NAME diskN           # then the FAT
  ```

  `sudo` is needed to take the device from the macOS disk driver (if pyusb
  was installed with `--user`, pass `PYTHONPATH` through sudo). For the FAT
  use `diskutil eraseVolume` on the whole disk, or Disk Utility with no
  partition scheme: a PC floppy has no partition table (GUID or MBR makes
  it unreadable on a PC).

Formatting takes about half a second per track (position, write, read
back). The red LED stays off; the green one flickers.

## Messages the host may show

| Host message | Cause |
|---|---|
| "No disk", "Insert a disk" | empty drive |
| "Not formatted / unreadable, format?" | unformatted or foreign disk (red 1 Hz) |
| "Read-only" | notch covered or J3 closed |
| "Could not be written / I/O error" on write, sync or eject | a write failed after three flushes, or a write protection appeared with writes cached (red 2 Hz) |
| "I/O error" on read | a sector of the disk does not read (medium error, unrecovered read) |
| "Disk not ejected properly" | the disk was pulled (or swapped) with the volume mounted; writes still cached for it are lost (red 2 Hz) |
| "Device removed unexpectedly" then remount | you changed the disk under a mounted volume: expected |

## If the firmware locks up

A hardware watchdog resets the board when the firmware stops responding:
within 8 s if the USB side hangs, within about 30 s if the drive side
hangs (it is allowed up to 20 s for one write with its retries). The reset
releases every drive line, so the write gate can never stay on, and the
drive is power-cycled. Nothing survives the reset: writes still cached are
lost, and the host is not told beyond seeing the device disconnect and
come back (the volume disappears unejected). Only the UART log records it
(`recovered by watchdog`). If that happens, check the files written just
before.

## Limits

- PC MFM formats only (IBM System/34, 512-byte sectors). Not read: FM/single
  density, Amiga, Apple II GCR, copy-protected tracks.
- One drive per board, DS1, pin 34 must be DISK CHANGE (see [pinmap.md](pinmap.md)),
  and the drive must clear it on a step pulse it ignores at track 0 (the
  empty-drive check relies on it; verified on the YD-380B). A drive that
  only clears it on a real head move would always look empty.
- The motor stops 2 s after the last access; the first access afterwards
  takes about half a second longer.
- macOS Finder does not list a FAT volume that has neither a volume label
  nor a volume serial number (DOS 2.x/3.x disks, shown as "NO NAME") in
  its sidebar, whatever the device; it is mounted all the same: open it
  from the Desktop or with Go > Go to Folder `/Volumes/NO NAME`.
- One unreadable sector on track 0 side 0 makes the probe fail, and the
  disk is reported unformatted (red 1 Hz) rather than mounted with a hole
  in its boot area or FAT: clean the head, try again; a disk that still
  fails needs imaging tools.
