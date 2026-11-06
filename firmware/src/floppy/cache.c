// Derived from jackdoe/pico-mfm-floppy src/cache.c (MIT); parameterized by
// the format bound via cache_bind (see cache.h).

#include "cache.h"
#include <string.h>

static uint64_t cache_tick(cache_t *cache) {
  cache->clock++;
  if (cache->clock != 0) return cache->clock;
  for (size_t i = 0; i < CACHE_TRACKS; i++) cache->slots[i].used = 0;
  cache->clock = 1;
  return cache->clock;
}

static void cache_touch(cache_t *cache, cache_slot_t *slot) {
  slot->used = cache_tick(cache);
}

static bool cache_slot_is(const cache_slot_t *slot, uint8_t cylinder, uint8_t head) {
  return slot->occupied && slot->track.cylinder == cylinder &&
         slot->track.head == head;
}

static uint16_t cache_slot_index(const cache_slot_t *slot) {
  return (uint16_t)(slot->track.cylinder * DISK_HEADS + slot->track.head);
}

// Forget every slot. Dirty sectors going away here (medium changed or
// pulled, format of another geometry) never reached the disk: that is a
// write fault for the host, reported through cache_take_write_fault.
void cache_clear(cache_t *cache) {
  if (!cache) return;
  for (size_t i = 0; i < CACHE_TRACKS; i++) {
    for (uint32_t bits = cache->slots[i].dirty; bits; bits &= bits - 1u) {
      cache->dropped_sectors++;
      cache->write_fault = true;
    }
  }
  memset(cache->slots, 0, sizeof(cache->slots));
  cache->clock = 0;
}

disk_err_t cache_init(cache_t *cache, disk_device_t device) {
  if (!cache || !device.read_track || !device.media_generation ||
      !device.write_protected) {
    return DISK_ERR_INVALID;
  }
  memset(cache, 0, sizeof(*cache));
  cache->device = device;
  return DISK_OK;
}

disk_err_t cache_bind(cache_t *cache, const floppy_format_t *fmt,
                      uint32_t generation) {
  if (!cache || !fmt) return DISK_ERR_INVALID;
  uint32_t current;
  disk_err_t status = cache->device.media_generation(cache->device.ctx, &current);
  if (status != DISK_OK) return status;
  // The medium the caller identified is gone: binding the format to its
  // successor would mount a disk nobody has read.
  if (current != generation) return DISK_ERR_MEDIA_CHANGED;
  cache_clear(cache);
  cache->fmt = fmt;
  cache->generation = generation;
  return DISK_OK;
}

disk_err_t cache_check(cache_t *cache) {
  if (!cache) return DISK_ERR_INVALID;
  uint32_t generation;
  disk_err_t status = cache->device.media_generation(cache->device.ctx, &generation);
  if (status != DISK_OK) {
    if (status == DISK_ERR_MEDIA_CHANGED) cache_clear(cache);
    return status;
  }
  if (generation != cache->generation) {
    cache_clear(cache);
    return DISK_ERR_MEDIA_CHANGED;
  }
  return DISK_OK;
}

disk_err_t cache_writable(cache_t *cache) {
  if (!cache) return DISK_ERR_INVALID;
  if (!cache->device.write_sectors) return DISK_ERR_WRITE_PROTECTED;
  bool write_protected;
  disk_err_t status =
      cache->device.write_protected(cache->device.ctx, &write_protected);
  if (status != DISK_OK) return status;
  return write_protected ? DISK_ERR_WRITE_PROTECTED : DISK_OK;
}

bool cache_dirty(const cache_t *cache) {
  if (!cache) return false;
  for (size_t i = 0; i < CACHE_TRACKS; i++) {
    if (cache->slots[i].dirty) return true;
  }
  return false;
}

static disk_err_t cache_merge(cache_t *cache, cache_slot_t *slot,
                              const track_t *fresh, bool authoritative) {
  disk_err_t status = DISK_OK;
  if (authoritative) slot->conflicted = 0;
  for (uint8_t sector = 0; sector < cache->fmt->sectors_per_track; sector++) {
    uint32_t bit = 1u << sector;
    if (!track_has(fresh, sector) || (slot->dirty & bit)) continue;
    if (authoritative) {
      memcpy(slot->track.data[sector], fresh->data[sector], DISK_SECTOR_SIZE);
      slot->track.valid |= bit;
    } else if (slot->conflicted & bit) {
      status = DISK_ERR_CORRUPT;
    } else if (track_has(&slot->track, sector)) {
      if (memcmp(slot->track.data[sector], fresh->data[sector],
                 DISK_SECTOR_SIZE) != 0) {
        slot->track.valid &= ~bit;
        slot->conflicted |= bit;
        status = DISK_ERR_CORRUPT;
      }
    } else {
      memcpy(slot->track.data[sector], fresh->data[sector], DISK_SECTOR_SIZE);
      slot->track.valid |= bit;
    }
  }
  return status;
}

static disk_err_t cache_refresh(cache_t *cache, cache_slot_t *slot) {
  uint8_t cylinder = slot->track.cylinder;
  uint8_t head = slot->track.head;
  uint32_t mask = track_valid_mask(cache->fmt);
  memset(&cache->work, 0, sizeof(cache->work));
  cache->work.cylinder = cylinder;
  cache->work.head = head;
  disk_err_t status = cache->device.read_track(
      cache->device.ctx, cache->generation, cylinder, head, &cache->work);
  disk_err_t media = cache_check(cache);
  if (media != DISK_OK) return media;
  if (cache->work.cylinder != cylinder || cache->work.head != head ||
      (cache->work.valid & ~mask) != 0) {
    return status == DISK_OK ? DISK_ERR_CORRUPT : status;
  }
  bool authoritative = status == DISK_OK && cache->work.valid == mask;
  disk_err_t merged = cache_merge(cache, slot, &cache->work, authoritative);
  return merged != DISK_OK ? merged : status;
}

// Write the dirty sectors of a slot in place. Sectors the medium refuses
// after CACHE_FLUSH_ATTEMPTS flushes are dropped: their dirty bits go, their
// data is no longer claimed valid, and write_fault tells the host side to
// report a MEDIUM ERROR once. The host sees every other sector as usual.
// A neighbour the write disturbed and could not restore becomes dirty: the
// slot holds its good copy, and it goes through the same policy. A write
// protection that appeared after the data was cached (notch, read-only
// jumper) counts as a refusal too: nothing is ever written through it, and
// the flush must end rather than retry (and spin the disk) forever.
static disk_err_t cache_flush_slot(cache_t *cache, cache_slot_t *slot) {
  if (!slot->dirty) return DISK_OK;
  disk_err_t status = cache_writable(cache);
  if (status == DISK_OK) {
    uint32_t written = 0;
    uint32_t damaged = 0;
    status = cache->device.write_sectors(cache->device.ctx, cache->generation,
                                         &slot->track, slot->dirty, &written,
                                         &damaged);
    if (status == DISK_ERR_MEDIA_CHANGED) {
      cache_clear(cache);
      return status;
    }
    written &= slot->dirty;
    slot->dirty &= ~written;
    slot->conflicted &= ~written;
    slot->dirty |= damaged & slot->track.valid;
    if (!slot->dirty) {
      slot->failures = 0;
      return status == DISK_OK ? cache_check(cache) : status;
    }
    if (status == DISK_OK) status = DISK_ERR_IO;
  } else if (status == DISK_ERR_MEDIA_CHANGED) {
    cache_clear(cache);
    return status;
  }
  if (++slot->failures >= CACHE_FLUSH_ATTEMPTS) {
    uint32_t dropped = slot->dirty;
    slot->track.valid &= ~dropped;
    slot->conflicted &= ~dropped;
    slot->dirty = 0;
    slot->failures = 0;
    for (uint32_t bits = dropped; bits; bits &= bits - 1u) cache->dropped_sectors++;
    cache->write_fault = true;
  }
  return status;
}

static disk_err_t cache_acquire(cache_t *cache, uint8_t cylinder, uint8_t head,
                                cache_slot_t **out) {
  cache_slot_t *victim = NULL;
  cache_slot_t *clean = NULL;
  cache_slot_t *empty = NULL;
  for (size_t i = 0; i < CACHE_TRACKS; i++) {
    cache_slot_t *slot = &cache->slots[i];
    if (cache_slot_is(slot, cylinder, head)) {
      cache_touch(cache, slot);
      *out = slot;
      return DISK_OK;
    }
    if (!slot->occupied) {
      if (!empty) empty = slot;
    } else if (!slot->dirty) {
      if (!clean || slot->used < clean->used) clean = slot;
    } else if (!victim || slot->used < victim->used) {
      victim = slot;
    }
  }
  cache_slot_t *slot = empty ? empty : clean ? clean : victim;
  if (slot->dirty) {
    disk_err_t status = cache_flush_slot(cache, slot);
    // A victim whose sectors were just dropped (recorded in write_fault)
    // is free: the caller's own request must not fail for it.
    if (status != DISK_OK && slot->dirty) return status;
    if (status == DISK_ERR_MEDIA_CHANGED) return status;
  }
  memset(slot, 0, sizeof(*slot));
  slot->track.cylinder = cylinder;
  slot->track.head = head;
  slot->occupied = true;
  cache_touch(cache, slot);
  *out = slot;
  return DISK_OK;
}

disk_err_t cache_read(cache_t *cache, uint16_t lba, uint8_t out[DISK_SECTOR_SIZE]) {
  if (!cache || !out || !cache->fmt) return DISK_ERR_INVALID;
  uint8_t cylinder;
  uint8_t head;
  uint8_t sector;
  if (!disk_lba_to_chs(cache->fmt, lba, &cylinder, &head, &sector)) {
    return DISK_ERR_INVALID;
  }
  disk_err_t status = cache_check(cache);
  if (status != DISK_OK) return status;
  cache_slot_t *slot;
  status = cache_acquire(cache, cylinder, head, &slot);
  if (status != DISK_OK) return status;
  if (!track_has(&slot->track, sector)) {
    status = cache_refresh(cache, slot);
    if (!cache_slot_is(slot, cylinder, head) || !track_has(&slot->track, sector)) {
      return status == DISK_OK ? DISK_ERR_CORRUPT : status;
    }
  }
  memcpy(out, slot->track.data[sector], DISK_SECTOR_SIZE);
  return cache_check(cache);
}

disk_err_t cache_write(cache_t *cache, uint16_t lba,
                       const uint8_t data[DISK_SECTOR_SIZE]) {
  if (!cache || !data || !cache->fmt) return DISK_ERR_INVALID;
  uint8_t cylinder;
  uint8_t head;
  uint8_t sector;
  if (!disk_lba_to_chs(cache->fmt, lba, &cylinder, &head, &sector)) {
    return DISK_ERR_INVALID;
  }
  if (!cache->device.write_sectors) return DISK_ERR_WRITE_PROTECTED;
  disk_err_t status = cache_check(cache);
  if (status != DISK_OK) return status;
  cache_slot_t *slot;
  status = cache_acquire(cache, cylinder, head, &slot);
  if (status != DISK_OK) return status;
  uint32_t bit = 1u << sector;
  memcpy(slot->track.data[sector], data, DISK_SECTOR_SIZE);
  slot->track.valid |= bit;
  slot->dirty |= bit;
  slot->conflicted &= ~bit;
  return DISK_OK;
}

disk_err_t cache_flush(cache_t *cache) {
  if (!cache) return DISK_ERR_INVALID;
  disk_err_t status = cache_check(cache);
  if (status != DISK_OK) return status;
  for (;;) {
    cache_slot_t *next = NULL;
    for (size_t i = 0; i < CACHE_TRACKS; i++) {
      cache_slot_t *slot = &cache->slots[i];
      if (!slot->dirty) continue;
      if (!next || cache_slot_index(slot) < cache_slot_index(next)) next = slot;
    }
    if (!next) return DISK_OK;
    status = cache_flush_slot(cache, next);
    if (status != DISK_OK) return status;
  }
}

disk_err_t cache_seed(cache_t *cache, const track_t *track) {
  if (!cache || !track || !cache->fmt) return DISK_ERR_INVALID;
  if (!disk_ch_valid(cache->fmt, track->cylinder, track->head) ||
      (track->valid & ~track_valid_mask(cache->fmt)) != 0) {
    return DISK_ERR_INVALID;
  }
  disk_err_t status = cache_check(cache);
  if (status != DISK_OK) return status;
  cache_slot_t *slot;
  status = cache_acquire(cache, track->cylinder, track->head, &slot);
  if (status != DISK_OK) return status;
  if (slot->dirty) return DISK_ERR_BUSY; // host data wins over what we read
  memcpy(&slot->track, track, sizeof(*track));
  slot->conflicted = 0;
  return DISK_OK;
}

bool cache_has_track(const cache_t *cache, uint8_t cylinder, uint8_t head) {
  if (!cache) return false;
  for (size_t i = 0; i < CACHE_TRACKS; i++) {
    if (cache_slot_is(&cache->slots[i], cylinder, head)) return true;
  }
  return false;
}

disk_err_t cache_prefetch(cache_t *cache, uint8_t cylinder, uint8_t head) {
  if (!cache || !cache->fmt) return DISK_ERR_INVALID;
  if (!disk_ch_valid(cache->fmt, cylinder, head)) return DISK_ERR_INVALID;
  if (cache_has_track(cache, cylinder, head)) return DISK_OK;
  disk_err_t status = cache_check(cache);
  if (status != DISK_OK) return status;
  cache_slot_t *slot;
  status = cache_acquire(cache, cylinder, head, &slot);
  if (status != DISK_OK) return status;
  return cache_refresh(cache, slot);
}

bool cache_take_write_fault(cache_t *cache) {
  if (!cache || !cache->write_fault) return false;
  cache->write_fault = false;
  return true;
}
