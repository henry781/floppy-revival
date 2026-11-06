// Write-back track cache.
// Derived from jackdoe/pico-mfm-floppy src/cache.h (MIT); geometry comes from
// the floppy_format_t supplied at cache_bind time instead of compile-time
// 1.44MB constants.

#ifndef CACHE_H
#define CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include "disk.h"
#include "format.h"

#ifndef CACHE_TRACKS
#define CACHE_TRACKS 6u
#endif
// Flush attempts (each one the device's own retries) before the sectors a
// track refuses to take (or a write protection keeps from it) are dropped
// and reported as a write fault.
#ifndef CACHE_FLUSH_ATTEMPTS
#define CACHE_FLUSH_ATTEMPTS 3u
#endif

typedef struct {
  track_t track;
  uint32_t dirty;
  uint32_t conflicted;
  uint64_t used;
  uint8_t failures; // consecutive flushes that left dirty sectors behind
  bool occupied;
} cache_slot_t;

typedef struct {
  disk_device_t device;
  const floppy_format_t *fmt;
  cache_slot_t slots[CACHE_TRACKS];
  track_t work;
  uint64_t clock;
  uint32_t generation;
  uint32_t dropped_sectors; // given up after CACHE_FLUSH_ATTEMPTS, or lost with the medium
  bool write_fault;         // set with dropped_sectors; cleared by cache_take_write_fault
} cache_t;

disk_err_t cache_init(cache_t *cache, disk_device_t device);
// Latch `generation` (the medium the caller probed or formatted) and the
// format; clears all slots. DISK_ERR_MEDIA_CHANGED, nothing bound, when the
// medium is no longer that one.
disk_err_t cache_bind(cache_t *cache, const floppy_format_t *fmt,
                      uint32_t generation);
disk_err_t cache_check(cache_t *cache);
disk_err_t cache_writable(cache_t *cache);
void cache_clear(cache_t *cache);
bool cache_dirty(const cache_t *cache);
disk_err_t cache_read(cache_t *cache, uint16_t lba, uint8_t out[DISK_SECTOR_SIZE]);
disk_err_t cache_write(cache_t *cache, uint16_t lba,
                       const uint8_t data[DISK_SECTOR_SIZE]);
disk_err_t cache_flush(cache_t *cache);
// Store a track that is known to be on the medium (probe, format) so the
// first host reads of it are served without a revolution.
disk_err_t cache_seed(cache_t *cache, const track_t *track);
// Read a track into the cache ahead of the host (read-ahead); nothing to do
// when it is already cached.
disk_err_t cache_prefetch(cache_t *cache, uint8_t cylinder, uint8_t head);
bool cache_has_track(const cache_t *cache, uint8_t cylinder, uint8_t head);
// True once if sectors were dropped since the last call.
bool cache_take_write_fault(cache_t *cache);

#endif
