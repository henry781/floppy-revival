# Testing

Three levels of testing, from cheapest to most convincing:

1. **Host tests** (`firmware/tests/run_host_tests.sh`, see
   [building.md](building.md#host-tests)): codec, cache and USB layer
   logic on a PC. Fast, repeatable, and blind to anything electrical.
2. **Board checks without the drive**: the power supply check with
   nothing connected, then the smoke test of the interface. Described in
   [hardware.md](hardware.md#drive-less-smoke-test-mandatory-before-the-drive-is-ever-connected)
   and [assembly.md](assembly.md). Do these on every new board.
3. **The bench plan below**, with a drive, real disks and a host computer.

## Known issues

Seen on the bench and not yet understood. If you build one and see either,
a UART log from the debug build would help.

- **A neighbouring sector damaged after a sector write.** Seen twice in
  one session, on new files around cylinders 12 to 15; the firmware
  detected the damage on read-back and rewrote the sector from its cache,
  so no data was lost. Not seen again in 781 sector writes afterwards. One
  hypothesis is that it happens on the first overwrite next to a sector
  recorded by another drive. The debug build dumps a track map (`map:`
  lines) when it happens.
- **Spindle does not start until the disk is reinserted.** Seen twice: the
  motor line was asserted, 12 V was present, but no index pulses came. With
  the drive lever settled, select, step and motor worked every time
  afterwards. The debug build's `P` key power-cycles the drive, to tell a
  drive state problem from a firmware one next time it happens.

Other limitations, by design:

- PC MFM formats only. FM, Amiga, Apple and copy-protected tracks are not
  read.
- One drive, jumpered DS1, with DISK CHANGE on pin 34.
- The USB VID/PID are test values (see [building.md](building.md#usb-identity)).
- A disk inserted into a drive that has been empty for more than a minute
  can take up to 30 s to mount (see [user-guide.md](user-guide.md#changing-disks)).

## Bench test plan

Each test gives its starting state and its pass criterion, so that a
failure points somewhere. Use the debug build (`floppy_revival_debug.uf2`,
UART 115200 on J2, `?` lists the keys) and disks you can afford to lose.
Key `t` prints the drive counters quoted below.

Hosts: macOS, Windows 10 or 11, Linux with `ufiformat`, and if possible a
PC with a real 5.25" drive for interoperability. Note the firmware commit,
host OS version and disk type for each run.

What has been run so far: the reads, writes, swaps, ejects and power
behaviour on macOS with a YD-380B. The Windows and Linux runs, the
formatting tests and the interoperability tests on a real PC are still
open.

### A. Basic operation

| # | Test | Reset | Pass |
|---|---|---|---|
| A1 | Mount 360KB, 720KB 96tpi and 1.2MB disks | empty drive, power cycle | each mounts within 3 s; log `media: <format>`; the 360KB disk shows `boot sector says 720 sectors: 5.25" 360KB (HD drive)` and only ONE read of track 0 |
| A2 | Empty drive | no disk, head parked at TRK00 | motor never spins and the head never moves (listen, watch): one short LED blink per check, every 5 s for 60 s after a removal, then every 30 s; debug log stays quiet (quick checks trace nothing until a disk is in), insertion logs `quick check: DSKCHG cleared <n> us after the pulse` then the probe |
| A3 | Finder swap scenario | disk A mounted, idle | pull A, insert B: device drops off USB ~1 s, B mounts; no "disk not ejected properly" with writes lost |
| A4 | Eject while red LED steady | copy a 100 KB file, eject at once | eject completes only after the LED goes out; file reads back after re-insertion |
| A5 | Watchdog | none | `recovered by watchdog` never appears across the whole session, including long flushes (copy 1 MB, sync at once: six dirty tracks flushed in one request must not trip it) |
| A6 | Drive power-cycle hold | adapter plugged, USB plugged, scope on J5 pin 1 (12V) | at every boot (USB replug, RUN tap, reflash) the log shows `drive power held off 3500 ms`, the 12V rail reaches 0 V within ~50 ms of the reset (regulators stopped by ON/OFF, not a dip) and stays there until `drive power on (GP18, adapter ... mV)` 3.5 s later; DSKCHG reads set after each boot |
| A6b | Adapter re-seat while running | disk mounted, idle, scope on J5 pin 1 | pull the adapter plug: log `drive power OFF` within 50 ms, 12V rail falls (no reverse, no spike); plug it back: `drive power on` 3.5 s later, rails rise through the soft ramp (no overshoot above 12.6V / 5.3V), the disk re-mounts |

### B. Sector writes

| # | Test | Reset | Pass |
|---|---|---|---|
| B1 | Single-sector write | freshly PC-formatted 1.2MB disk, one small file | log shows `sector writes` +1..2 per flush, `late_gate 0`, `not_found 0`, `collateral 0`; file reads back |
| B2 | Layout preserved | disk formatted on a PC (or by test C1) | after writing files here, `f` on a written track shows the same sector order and ~84-byte gap 3 (1.2MB) as before, i.e. the IAM and gaps of the original format are still there |
| B3 | Full track written in one revolution | copy a 300 KB file | per track: one `sector_pass` with 15 sectors (1.2MB) or 9 (360KB); `retries` grows by < 1 per 10 tracks |
| B4 | Bad sector does not block its track | disk with one unreadable sector (make one: scratch, or write a track at the wrong density with the previous firmware) | write a file whose sectors share the track: it lands and reads back; the bad sector still errors on read |
| B5 | Failure policy | sector that cannot be written (write-protect notch half covered during the flush, or a damaged spot) | after 3 flushes (~5 s) the log says `write error: sectors dropped`, red LED 2 Hz, the host's next write/sync/eject gets an I/O error exactly once, eject then succeeds |
| B6 | Write protect flip during a write | copy a 300 KB file, cover the notch mid-copy | host writes after the flip fail with DATA PROTECT ("read-only"); sectors cached before it are never written: after 3 flushes (a few seconds) the log says `write error: sectors dropped`, red LED 2 Hz, the host's next sync/eject gets an I/O error once, then eject succeeds and the disk stops spinning; no track damaged (every sector written before the flip, and every other sector of the tracks involved, still reads). Repeat with J3 closed mid-copy instead of the notch: same result, and the drive is not even selected for the dropped sectors |
| B9 | USB unplug during a write burst | copy a 1 MB file, pull the USB cable mid-copy (the Pico loses power, GP18 floats and the regulators stop within microseconds: a power cut in the middle of a sector write) | after replug the disk mounts; `chkdsk`/`fsck.fat` may report the half-copied file, but only the sector being written at the moment of the cut may be unreadable: every other sector of that track and of the disk reads (read the whole disk, e.g. `dd` of the raw device); repeat 5 times |
| B10 | Collateral damage reported | disk with a weak sector next to sectors the host writes (B4 disk, or a sector damaged on purpose); debug build | when a write disturbs a neighbour that cannot be restored, the log shows `collateral` growing and then `write error: sectors dropped`, the host gets an I/O error once on its next write/sync/eject (never a silent GOOD), red 2 Hz; a neighbour that is restored on the next flush gives no error |
| B7 | Interop | write files here on 360KB and 1.2MB disks | a PC with a real drive (or a Greaseweazle + decoder) reads every file intact; a disk written on the PC reads here |
| B8 | Precompensation | 1.2MB disk, files on cylinders 70-79 | `f` on cylinder 79 shows three narrow histogram peaks; verify never retries there |

### C. Formatting

| # | Test | Reset | Pass |
|---|---|---|---|
| C1 | Blank disk, Windows full format 1.2 MB | bulk-erased or never-formatted HD disk | TUR/RFC trace shows `unformatted` then 160 `FORMAT UNIT`; Windows reports success; disk mounts here and on a PC; `f` shows IAM + 15 sectors + gap 3 of 84 bytes |
| C2 | Windows format 360 KB | blank DD disk | the capacity list offers 360 KB; format succeeds; 720 blocks reported afterwards; mounts on a PC 360KB drive (bulk-erased disk) |
| C3 | Linux `ufiformat -i` then `-f 1200` | blank HD disk | `-i` lists 1200/720/360 and says unformatted; format + `mkfs.fat` + mount succeed |
| C4 | DD disk formatted as 1.2 MB | DD disk | format fails with FORMAT COMMAND FAILED on verify (host error), disk stays unformatted; no hang |
| C5 | Foreign disk | an Amstrad/Apple/C64 disk | reported unformatted (red 1 Hz), host offers to format, **decline**: nothing is written (confirm by reading it on its native machine afterwards) |
| C6 | Write-protected blank disk | notch covered | format refused with DATA PROTECT |
| C7 | macOS formatting | blank disk, `firmware/tools/ufi_format.py --format 1200`, then `diskutil eraseVolume` | format succeeds, the disk mounts here and on a PC. (Disk Utility's Erase never sends FORMAT UNIT: it only writes a file system.) |
| C8 | Power loss during a format | start a Windows format, pull the adapter plug mid-format | format fails cleanly, the next insertion probes normally, no other track damaged (compare `f` on neighbouring tracks) |

### D. Media states and host protocol

| # | Test | Reset | Pass |
|---|---|---|---|
| D1 | Unformatted disk inserted | empty drive | within 10 s: `disk present but unreadable: reported as unformatted`, red 1 Hz, host shows the drive as needing a format; TUR trace: one UA then 30/01 |
| D2 | Unformatted disk pulled | D1 | state returns to no medium; red LED off; probing resumes at the fast rate for the next disk |
| D3 | Bus reset in flight | copy a large file, unplug/replug USB mid-copy | device re-enumerates, next host command gets a UNIT ATTENTION, no stale completion matched (`[msc] bus reset` in the trace) |
| D4 | Read-ahead | copy a 1 MB file from the disk | log shows prefetches between host reads; throughput better than with `-DFLOPPY_READ_AHEAD=0` on the same disk (note both numbers) |
| D5 | SYNCHRONIZE CACHE honesty | copy a file, run `sync` (Linux) at once | the command returns only after the red LED went out |

### E. Release build

| # | Test | Pass |
|---|---|---|
| E1 | `floppy_revival.uf2` (release) | no monitor keys, log lines still present, everything in A-D behaves identically |
| E2 | USB identity | configured with `-DFLOPPY_RELEASE=ON` (configure fails on the test VID/PID); VID/PID from the allocated pair, `bcdDevice 1.00`, product revision `1.00` in INQUIRY |
| E4 | Read error reaches the host as a medium error | disk with an unreadable sector (B4), read the file that contains it | the host reports an I/O error on that file (sense 03/11/00 in the `[msc]` trace), the volume stays mounted (before the class-driver fix this read as "medium not present") |
| E3 | Power | bMaxPower 250 mA accepted by a laptop port; VBUS at the 7407s ≥ 4.75 V while the drive seeks |

### F. Timing and robustness checks

| # | Check | How |
|---|---|---|
| F1 | Drive write-to-read recovery vs. catching the next ID in the same revolution | `t` after a 300 KB copy: `not_found` stays 0 and revolutions per track flush ≈ 1 (else every other sector is caught one turn later: slower, still correct) |
| F2 | `FLOPPY_WGATE_SETUP_US` (4 µs) against the YD-380B spec's write-gate-to-first-data minimum | read the spec; if it asks more, raise the constant (the 12-byte 00 preamble makes an early first pulse harmless) |
| F3 | Gate-open jitter from XIP cache misses (core 0 runs TinyUSB from the same flash) | scope /WGATE against /RDATA on a 1.2MB write: the splice must sit inside gap 2, within ±1 byte of its end; `late_gate` stays 0 |
| F4 | Decoder resync when the gate opened up to 2 bytes late (one old 00 byte left before the splice) | `f` on tracks written under load (copy while the host also reads): no CRC/format errors |
| F5 | Reformat across track pitch (720 KB 96tpi disk formatted as 360 KB and back) | both formats succeed; log shows `stale ID ... formatting over it` without a recalibration per track |
| F6 | The 50 µs idle select pulse for DSKCHG sampling does not clock the latch or blink the drive LED | observe during a 2 min idle with a disk in |
| H1 | Watchdog on a hung drive core | debug build only, temporary test patch: spin forever in core 1 with /WGATE asserted on an expendable disk | board resets within ~30 s (22 s heartbeat + 8 s watchdog), /WGATE (TP) goes high at the reset, drive power-cycles; log `recovered by watchdog` |
| H2 | Swap between probe and mount | insert a disk and swap it within the first second of probing, repeat 10 times | the log never shows `media:` for a disk followed by the other disk being read under that format; `medium changed before mount` may appear, then the new disk is probed |
| H3 | Stack guards | full A-D run with the stack-guard build (default now) | no hard fault / unexplained watchdog reset |

A test that fails is something to understand (read the spec, rerun under
controlled conditions) before any threshold is loosened.
