#include "filesys/cache.h"
#include <string.h>
#include <debug.h>
#include "filesys/filesys.h"
#include "threads/synch.h"

#define CACHE_SIZE 64

struct cache_entry
{
    block_sector_t disk_sector;
    uint8_t buffer[BLOCK_SECTOR_SIZE];
    bool valid;
    bool dirty;
    int recent_used;
    struct list_elem elem;
};

static struct cache_entry cache[CACHE_SIZE];
static struct lock global_lock;
static struct list cache_list;
static bool cache_recent_used_more (const struct list_elem *lhs, const struct list_elem *rhs, void *aux UNUSED);

void
cache_init (void)
{
    list_init (&cache_list);
    lock_init (&global_lock);
    for (size_t i = 0; i < CACHE_SIZE; i++)
    {
        cache[i].valid = 0;
        cache[i].dirty = 0;
        cache[i].recent_used = 0;
        memset (cache[i].buffer, 0, BLOCK_SECTOR_SIZE);
        list_push_back (&cache_list, &cache[i].elem);
    }
}

static struct cache_entry *
cache_lookup (block_sector_t sector)
{
    struct cache_entry *ret = NULL;
    for (size_t i = 0; i < CACHE_SIZE; i++)
    {
        cache[i].recent_used ++;
        if (!cache[i].valid) continue;
        if (cache[i].disk_sector == sector)
            ret = &(cache[i]);
    }
    return ret;
}

static struct cache_entry *
cache_evcit (void)
{
    struct cache_entry *slot = list_entry (list_back (&cache_list), struct cache_entry, elem);
    if (slot->dirty)
    {
        block_write (fs_device, slot->disk_sector, slot->buffer);
        slot->dirty = 0;
    }
    slot->valid = 0;
    return slot;
}

void
cache_read (block_sector_t sector, void *target)
{
    lock_acquire (&global_lock);
    struct cache_entry *slot = cache_lookup (sector);
    if (slot == NULL)
    {
        slot = cache_evcit ();
        slot->valid = 1;
        slot->dirty = 0;
        slot->disk_sector = sector;
        block_read (fs_device, sector, slot->buffer);
    }
    slot->recent_used = 0;
    list_sort (&cache_list, cache_recent_used_more, NULL);
    memcpy (target, slot->buffer, BLOCK_SECTOR_SIZE);
    lock_release (&global_lock);
}

void
cache_write (block_sector_t sector, const void *source)
{
    lock_acquire (&global_lock);
    struct cache_entry *slot = cache_lookup (sector);
    if (slot == NULL)
    {
        slot = cache_evcit ();
        slot->valid = 1;
        slot->dirty = 0;
        slot->disk_sector = sector;
        block_read (fs_device, sector, slot->buffer);
    }
    slot->recent_used = 0;
    slot->dirty = 1;
    list_sort (&cache_list, cache_recent_used_more, NULL);
    memcpy (slot->buffer, source, BLOCK_SECTOR_SIZE);
    lock_release (&global_lock);
}

void
cache_close (void)
{
    lock_acquire (&global_lock);
    for (size_t i = 0; i < CACHE_SIZE; i++)
    {
        if (!cache[i].valid) continue;
        if (cache[i].dirty)
            block_write (fs_device, cache[i].disk_sector, cache[i].buffer);
    }
    lock_release (&global_lock);
}

static bool
cache_recent_used_more (const struct list_elem *lhs, const struct list_elem *rhs, void *aux UNUSED)
{
  struct cache_entry *a, *b;
  
  ASSERT (lhs != NULL && rhs != NULL);
  
  a = list_entry (lhs, struct cache_entry, elem);
  b = list_entry (rhs, struct cache_entry, elem);
  
  return (a->recent_used > b->recent_used);
}
