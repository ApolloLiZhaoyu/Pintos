#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define DIRECT_BLOCK_SIZE 122
#define INDEX_SIZE 128
#define FIRST_INDEX_LEVEL (122 + 128)
#define SECOND_INDEX_LEVEL (122 + 128 + 128 * 128)
#define THIRD_INDEX_LEVEL (122 + 128 + 128 * 128 + 128 * 128 * 128)

static char zeros[BLOCK_SECTOR_SIZE];

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t direct_blocks[DIRECT_BLOCK_SIZE];
    block_sector_t first_index;
    block_sector_t second_index;
    block_sector_t third_index;

    bool is_dir;
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };

static block_sector_t index_to_sector (const struct inode_disk *inode_disk, off_t index);
static bool inode_allocate (struct inode_disk *inode_disk, off_t length);
static bool inode_allocate_index (block_sector_t *index, size_t sectors, off_t level);
static void inode_deallocate (struct inode *inode, off_t length);
static void inode_deallocate_index (block_sector_t index, size_t sectors, off_t level);

static block_sector_t
index_to_sector (const struct inode_disk *inode_disk, off_t index)
{
  block_sector_t ret;
  if (index < DIRECT_BLOCK_SIZE)
    return inode_disk->direct_blocks[index];
  else if (index < FIRST_INDEX_LEVEL)
  {
    off_t index_first = (index - DIRECT_BLOCK_SIZE);
    block_sector_t *sector = calloc (INDEX_SIZE, sizeof *sector);
    cache_read (inode_disk->first_index, sector);
    ret = *(sector + index_first);
    free (sector);
    return ret;
  }
  else if (index < SECOND_INDEX_LEVEL)
  {
    off_t index_first = (index - FIRST_INDEX_LEVEL) / INDEX_SIZE;
    off_t index_second = (index - FIRST_INDEX_LEVEL) % INDEX_SIZE;
    block_sector_t *sector = calloc (INDEX_SIZE, sizeof *sector);
    cache_read (inode_disk->second_index, sector);
    cache_read (*(sector + index_first), sector);
    ret = *(sector + index_second);
    free (sector);
    return ret;
  }
  else if (index < THIRD_INDEX_LEVEL)
  {
    off_t index_first = (index - SECOND_INDEX_LEVEL) / INDEX_SIZE;
    off_t index_second = (index - SECOND_INDEX_LEVEL - index_first * INDEX_SIZE) / INDEX_SIZE;
    off_t index_third = (index - SECOND_INDEX_LEVEL - index_first * INDEX_SIZE) % INDEX_SIZE;
    block_sector_t *sector = calloc (INDEX_SIZE, sizeof *sector);
    cache_read (inode_disk->third_index, sector);
    cache_read (*(sector + index_first), sector);
    cache_read (*(sector + index_second), sector);
    ret = *(sector + index_third);
    free (sector);
    return ret;
  }
  else 
    return -1;
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length){
    off_t index = pos / BLOCK_SECTOR_SIZE;
    return index_to_sector (&inode->data, index);
  }
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

static bool 
inode_allocate_index (block_sector_t *index, size_t sectors, off_t level)
{
  if (level == 0)
  {
    if (*index == 0)
    {
      if (!free_map_allocate (1, index))
        return false;
      cache_write (*index, zeros);
    }
    return true;
  }
  block_sector_t blocks[INDEX_SIZE];
  if (*index == 0)
  {
    free_map_allocate (1, index);
    cache_write (*index, zeros);
  }
  cache_read (*index, &blocks);
  if (level == 1)
  {
    for (size_t i = 0; i < sectors; i++)
    {
      if (!inode_allocate_index (&blocks[i], 1, level - 1))
        return false;
    }
  }
  else if (level == 2)
  {
    size_t num = DIV_ROUND_UP (sectors, INDEX_SIZE);
    for (size_t i = 0; i < num; i++)
    {
      size_t subsize = sectors < INDEX_SIZE ? sectors : INDEX_SIZE;
      if (!inode_allocate_index (&blocks[i], subsize, level - 1))
        return false;
      sectors -= subsize;
    }
  }
  else if (level == 3)
  {
    size_t num = DIV_ROUND_UP (sectors, INDEX_SIZE * INDEX_SIZE);
    for (size_t i = 0; i < num; i++)
    {
      size_t subsize = sectors < INDEX_SIZE * INDEX_SIZE ? sectors : INDEX_SIZE * INDEX_SIZE;
      if (!inode_allocate_index (&blocks[i], subsize, level - 1))
        return false;
      sectors -= subsize;
    }
  }

  cache_write (*index, &blocks);
  return true;
}

static bool
inode_allocate (struct inode_disk *inode_disk, off_t length)
{
  ASSERT (length >= 0);

  size_t sectors = bytes_to_sectors(length);
  size_t num;

  num = sectors < DIRECT_BLOCK_SIZE ? sectors : DIRECT_BLOCK_SIZE;
  for (size_t i = 0; i < num; i++)
  {
    if (inode_disk->direct_blocks[i] == 0)
    {
      if (!free_map_allocate (1, &inode_disk->direct_blocks[i]))
        return false;
      cache_write (inode_disk->direct_blocks[i], zeros);
    }
  }
  sectors -= num;
  if (sectors == 0) return true;

  num = sectors < INDEX_SIZE ? sectors : INDEX_SIZE;
  if (!inode_allocate_index (&inode_disk->first_index, num, 1))
    return false;
  sectors -= num;
  if (sectors == 0) return true;

  num = sectors < INDEX_SIZE * INDEX_SIZE ? sectors : INDEX_SIZE * INDEX_SIZE;
  if (!inode_allocate_index (&inode_disk->second_index, num, 2))
    return false;
  sectors -= num;
  if (sectors == 0) return true;

  num = sectors < INDEX_SIZE * INDEX_SIZE * INDEX_SIZE ? sectors : INDEX_SIZE;
  if (!inode_allocate_index (&inode_disk->third_index, num, 3))
    return false;
  sectors -= num;
  if (sectors == 0) return true;

  return false;
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_dir = is_dir;
      if (inode_allocate (disk_inode, disk_inode->length)) 
        {
          cache_write (sector, disk_inode);
          success = true; 
        } 
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  cache_read (inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

static void
inode_deallocate_index (block_sector_t index, size_t sectors, off_t level)
{
  if (level == 0)
  {
    free_map_release (index, 1);
    return;
  }
  block_sector_t blocks[INDEX_SIZE];
  cache_read (index, &blocks);
  if (level == 1)
  {
    for (size_t i = 0; i < sectors; i++)
    {
      inode_deallocate_index (blocks[i], 1, level - 1);
    }
  }
  else if (level == 2)
  {
    size_t num = DIV_ROUND_UP (sectors, INDEX_SIZE);
    for (size_t i = 0; i < num; i++)
    {
      size_t subsize = sectors > INDEX_SIZE ? INDEX_SIZE : sectors;
      inode_deallocate_index (blocks[i], subsize, level - 1);
      sectors -= subsize;
    }
  }
  else if (level == 3)
  {
    size_t num = DIV_ROUND_UP (sectors, INDEX_SIZE * INDEX_SIZE);
    for (size_t i = 0; i < num; i++)
    {
      size_t subsize = sectors > INDEX_SIZE * INDEX_SIZE ? INDEX_SIZE * INDEX_SIZE : sectors;
      inode_deallocate_index (blocks[i], subsize, level - 1);
      sectors -= subsize;
    }
  }
  free_map_release (index, 1);
  return;
}

static void
inode_deallocate (struct inode *inode, off_t length)
{
  ASSERT (length >= 0 && length < THIRD_INDEX_LEVEL);

  size_t sectors = bytes_to_sectors (length);
  if (sectors < DIRECT_BLOCK_SIZE)
  {
    for (size_t i = 0; i < sectors; i++)
    {
      free_map_release (inode->data.direct_blocks[i], 1);
    }
    return;
  }
  else
  {
    for (size_t i = 0; i < DIRECT_BLOCK_SIZE; i++)
    {
      free_map_release (inode->data.direct_blocks[i], 1);
    }
  }

  if (sectors < FIRST_INDEX_LEVEL)
  {
    inode_deallocate_index (inode->data.first_index, sectors - DIRECT_BLOCK_SIZE, 1);
    return;
  }
  else
  {
    inode_deallocate_index (inode->data.first_index, FIRST_INDEX_LEVEL - DIRECT_BLOCK_SIZE, 1);
  }

  if (sectors < SECOND_INDEX_LEVEL)
  {
    inode_deallocate_index (inode->data.second_index, sectors - FIRST_INDEX_LEVEL, 2);
    return;
  }
  else
  {
    inode_deallocate_index (inode->data.second_index, SECOND_INDEX_LEVEL - FIRST_INDEX_LEVEL, 2);
  }

  if (sectors < THIRD_INDEX_LEVEL)
  {
    inode_deallocate_index (inode->data.third_index, sectors - SECOND_INDEX_LEVEL, 3);
    return;
  }
  else
    return;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1);
          inode_deallocate (inode, inode->data.length); 
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          cache_read (sector_idx, buffer + bytes_read);
        }
      else 
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          cache_read (sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  /* Extend the file when EOF extends. */
  if (byte_to_sector (inode, offset + size - 1) == -1u)
  {
    bool success = inode_allocate (&inode->data, offset + size);
    if (!success) return 0;

    inode->data.length = offset + size;
    cache_write (inode->sector, &inode->data);
  }

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          cache_write (sector_idx, buffer + bytes_written);
        }
      else 
        {
          /* We need a bounce buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left) 
            cache_read (sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          cache_write (sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}

bool
inode_is_dir (const struct inode *inode)
{
  return inode->data.is_dir;
}

bool
inode_is_removed (const struct inode *inode)
{
  return inode->removed;
}

int
inode_get_open_cnt (const struct inode *inode)
{
  return inode->open_cnt;
}