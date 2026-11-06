// USB mass-storage glue (msc_disk.c).

#ifndef MSC_DISK_H_
#define MSC_DISK_H_

// Main-loop housekeeping: re-enumerates on USB when the medium under a
// mounted volume changes (macOS stops polling a mounted UFI device, so the
// swap cannot be reported through TEST UNIT READY).
void msc_task(void);

// USB bus reset / unplug: forget the host's view of the medium and any
// request in flight (called from the class driver reset and tud_umount_cb).
void msc_bus_reset(void);

#endif // MSC_DISK_H_
