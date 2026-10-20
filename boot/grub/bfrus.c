#define BFRUS_GRUB
#include "bfrus_format.h"

#include <grub/disk.h>
#include <grub/dl.h>
#include <grub/err.h>
#include <grub/file.h>
#include <grub/fs.h>
#include <grub/i18n.h>
#include <grub/lockdown.h>
#include <grub/mm.h>
#include <grub/misc.h>

GRUB_MOD_LICENSE ("GPLv3+");

struct grub_bfrus_data
{
  grub_disk_t disk;
  struct bfrus_superblock sb;
  struct bfrus_inode inode;
  grub_uint32_t inode_index;
};

static grub_dl_t my_mod;

static grub_uint32_t
grub_bfrus_crc32 (const void *data, grub_size_t len)
{
  const grub_uint8_t *p = data;
  grub_uint32_t crc = 0xffffffffU;

  while (len--)
    {
      int bit;
      crc ^= *p++;
      for (bit = 0; bit < 8; bit++)
        crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }

  return ~crc;
}

static grub_err_t
grub_bfrus_read_inode (struct grub_bfrus_data *data, grub_uint32_t index,
                       struct bfrus_inode *inode)
{
  grub_uint64_t byte_offset;
  grub_uint64_t sector;

  if (!index || index >= data->sb.inode_count)
    return grub_error (GRUB_ERR_BAD_FS, "BFRUS inode index out of range");

  byte_offset = (grub_uint64_t) index * sizeof (*inode);
  sector = data->sb.inode_start + byte_offset / 512;
  if (grub_disk_read (data->disk, sector, byte_offset % 512,
                      sizeof (*inode), inode))
    return grub_errno;
  if (inode->state != BFRUS_INODE_FILE && inode->state != BFRUS_INODE_DIR)
    return grub_error (GRUB_ERR_BAD_FS, "invalid BFRUS inode");
  if (inode->name_len >= sizeof (inode->name) ||
      inode->name[inode->name_len] != 0 ||
      inode->checksum != grub_bfrus_crc32 (inode, sizeof (*inode) - 4))
    return grub_error (GRUB_ERR_BAD_FS, "corrupt BFRUS inode");

  return GRUB_ERR_NONE;
}

static grub_err_t
grub_bfrus_read_extent (struct grub_bfrus_data *data, grub_uint32_t index,
                        struct bfrus_extent *extent)
{
  grub_uint64_t byte_offset;
  grub_uint64_t sector;

  if (!index || index >= data->sb.extent_count)
    return grub_error (GRUB_ERR_BAD_FS, "BFRUS extent index out of range");

  byte_offset = (grub_uint64_t) index * sizeof (*extent);
  sector = data->sb.extent_start + byte_offset / 512;
  if (grub_disk_read (data->disk, sector, byte_offset % 512,
                      sizeof (*extent), extent))
    return grub_errno;
  if (!extent->start_block || !extent->block_count ||
      extent->start_block > data->sb.data_blocks ||
      extent->block_count > data->sb.data_blocks - extent->start_block + 1 ||
      extent->checksum != grub_bfrus_crc32 (extent, sizeof (*extent) - 4))
    return grub_error (GRUB_ERR_BAD_FS, "corrupt BFRUS extent");

  return GRUB_ERR_NONE;
}

static struct grub_bfrus_data *
grub_bfrus_mount (grub_disk_t disk)
{
  static const grub_uint8_t magic[8] = { 'B', 'F', 'R', 'U', 'S', '0', '1', 0 };
  struct grub_bfrus_data *data;
  grub_uint64_t inode_min, extent_min, bitmap_min;

  data = grub_zalloc (sizeof (*data));
  if (!data)
    return 0;
  data->disk = disk;
  if (grub_disk_read (disk, 0, 0, sizeof (data->sb), &data->sb))
    goto fail;

  inode_min = (grub_uint64_t) BFRUS_INODE_COUNT * sizeof (struct bfrus_inode) / 512;
  extent_min = (grub_uint64_t) BFRUS_EXTENT_COUNT * sizeof (struct bfrus_extent) / 512;
  bitmap_min = ((grub_uint64_t) data->sb.data_blocks + 1 + 4095) / 4096;
  if (grub_memcmp (data->sb.magic, magic, sizeof (magic)) ||
      data->sb.version != BFRUS_VERSION ||
      data->sb.block_sectors != BFRUS_BLOCK_SECTORS ||
      data->sb.inode_count != BFRUS_INODE_COUNT ||
      data->sb.extent_count != BFRUS_EXTENT_COUNT ||
      data->sb.root_inode != BFRUS_ROOT_INODE ||
      data->sb.checksum != grub_bfrus_crc32 (&data->sb, sizeof (data->sb) - 4) ||
      data->sb.inode_sectors < inode_min ||
      data->sb.extent_sectors < extent_min ||
      data->sb.bitmap_sectors < bitmap_min ||
      data->sb.inode_start > data->sb.volume_sectors ||
      data->sb.inode_sectors > data->sb.volume_sectors - data->sb.inode_start ||
      data->sb.extent_start > data->sb.volume_sectors ||
      data->sb.extent_sectors > data->sb.volume_sectors - data->sb.extent_start ||
      data->sb.bitmap_start > data->sb.volume_sectors ||
      data->sb.bitmap_sectors > data->sb.volume_sectors - data->sb.bitmap_start ||
      data->sb.data_start > data->sb.volume_sectors ||
      (grub_uint64_t) data->sb.data_blocks * BFRUS_BLOCK_SECTORS >
      data->sb.volume_sectors - data->sb.data_start)
    {
      grub_error (GRUB_ERR_BAD_FS, "invalid BFRUS superblock");
      goto fail;
    }

  if (grub_bfrus_read_inode (data, data->sb.root_inode, &data->inode))
    goto fail;
  if (data->inode.state != BFRUS_INODE_DIR)
    {
      grub_error (GRUB_ERR_BAD_FS, "BFRUS root is not a directory");
      goto fail;
    }
  data->inode_index = data->sb.root_inode;
  return data;

 fail:
  grub_free (data);
  return 0;
}

static grub_err_t
grub_bfrus_lookup_child (struct grub_bfrus_data *data, grub_uint32_t parent,
                         const char *name, grub_size_t name_len,
                         struct bfrus_inode *result, grub_uint32_t *result_index)
{
  struct bfrus_inode dir;
  grub_uint32_t child, steps;

  if (grub_bfrus_read_inode (data, parent, &dir))
    return grub_errno;
  if (dir.state != BFRUS_INODE_DIR)
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "not a directory");

  child = dir.first_child;
  for (steps = 0; child; steps++)
    {
      struct bfrus_inode entry;
      if (steps >= data->sb.inode_count ||
          grub_bfrus_read_inode (data, child, &entry))
        return grub_errno ? grub_errno : grub_error (GRUB_ERR_BAD_FS, "BFRUS directory cycle");
      if (entry.parent != parent)
        return grub_error (GRUB_ERR_BAD_FS, "invalid BFRUS parent link");
      if (entry.name_len == name_len &&
          !grub_memcmp (entry.name, name, name_len))
        {
          *result = entry;
          *result_index = child;
          return GRUB_ERR_NONE;
        }
      child = entry.next_sibling;
    }

  return grub_error (GRUB_ERR_FILE_NOT_FOUND, "file not found");
}

static grub_err_t
grub_bfrus_lookup_path (struct grub_bfrus_data *data, const char *path)
{
  grub_uint32_t current = data->sb.root_inode;
  struct bfrus_inode inode = data->inode;
  const char *p = path;

  if (!p || *p != '/')
    return grub_error (GRUB_ERR_BAD_FILENAME, "invalid BFRUS path");
  while (*p)
    {
      char component[sizeof (inode.name)];
      grub_size_t len = 0;
      grub_uint32_t next;

      while (*p == '/')
        p++;
      if (!*p)
        break;
      while (*p && *p != '/')
        {
          if (len + 1 >= sizeof (component))
            return grub_error (GRUB_ERR_BAD_FILENAME, "BFRUS path component too long");
          component[len++] = *p++;
        }
      component[len] = 0;
      if (grub_bfrus_lookup_child (data, current, component, len, &inode, &next))
        return grub_errno;
      current = next;
    }

  data->inode = inode;
  data->inode_index = current;
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_bfrus_map_block (struct grub_bfrus_data *data, grub_uint32_t logical,
                      grub_uint32_t *physical)
{
  grub_uint32_t extent_index = data->inode.first_extent;
  grub_uint32_t steps;

  for (steps = 0; extent_index; steps++)
    {
      struct bfrus_extent extent;
      if (steps >= data->sb.extent_count ||
          grub_bfrus_read_extent (data, extent_index, &extent))
        return grub_errno ? grub_errno : grub_error (GRUB_ERR_BAD_FS, "BFRUS extent cycle");
      if (logical < extent.block_count)
        {
          *physical = extent.start_block + logical;
          return GRUB_ERR_NONE;
        }
      logical -= extent.block_count;
      extent_index = extent.next_extent;
    }

  return grub_error (GRUB_ERR_BAD_FS, "BFRUS file extent is missing");
}

static grub_err_t
grub_bfrus_dir (grub_device_t device, const char *path,
                grub_fs_dir_hook_t hook, void *hook_data)
{
  struct grub_bfrus_data *data = grub_bfrus_mount (device->disk);
  grub_uint32_t child, steps;

  if (!data)
    return grub_errno;
  if (grub_bfrus_lookup_path (data, path))
    goto fail;
  if (data->inode.state != BFRUS_INODE_DIR)
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "not a directory");
      goto fail;
    }

  child = data->inode.first_child;
  for (steps = 0; child; steps++)
    {
      struct bfrus_inode entry;
      struct grub_dirhook_info info;
      if (steps >= data->sb.inode_count ||
          grub_bfrus_read_inode (data, child, &entry))
        {
          if (!grub_errno)
            grub_error (GRUB_ERR_BAD_FS, "BFRUS directory cycle");
          goto fail;
        }
      if (entry.parent != data->inode_index)
        {
          grub_error (GRUB_ERR_BAD_FS, "invalid BFRUS parent link");
          goto fail;
        }
      grub_memset (&info, 0, sizeof (info));
      info.dir = entry.state == BFRUS_INODE_DIR;
      if (hook (entry.name, &info, hook_data))
        break;
      child = entry.next_sibling;
    }

 fail:
  grub_free (data);
  return grub_errno;
}

static grub_err_t
grub_bfrus_open (struct grub_file *file, const char *name)
{
  struct grub_bfrus_data *data = grub_bfrus_mount (file->device->disk);

  if (!data)
    return grub_errno;
  if (grub_bfrus_lookup_path (data, name))
    goto fail;
  if (data->inode.state != BFRUS_INODE_FILE)
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "not a regular file");
      goto fail;
    }
  file->data = data;
  file->size = data->inode.size;
  return GRUB_ERR_NONE;

 fail:
  grub_free (data);
  return grub_errno;
}

static grub_ssize_t
grub_bfrus_read (grub_file_t file, char *buf, grub_size_t len)
{
  struct grub_bfrus_data *data = file->data;
  grub_off_t pos = file->offset;
  grub_size_t done = 0;

  if (pos >= data->inode.size)
    return 0;
  if (len > data->inode.size - pos)
    len = data->inode.size - pos;

  while (done < len)
    {
      grub_uint32_t logical = (grub_uint32_t) (pos / BFRUS_BLOCK_SIZE);
      grub_uint32_t physical;
      grub_size_t within = (grub_size_t) (pos % BFRUS_BLOCK_SIZE);
      grub_size_t amount = BFRUS_BLOCK_SIZE - within;
      if (amount > len - done)
        amount = len - done;
      if (grub_bfrus_map_block (data, logical, &physical))
        return done ? (grub_ssize_t) done : -1;
      data->disk->read_hook = file->read_hook;
      data->disk->read_hook_data = file->read_hook_data;
      if (grub_disk_read (data->disk,
                          data->sb.data_start +
                          (grub_uint64_t) (physical - 1) * BFRUS_BLOCK_SECTORS,
                          within, amount, buf + done))
        {
          data->disk->read_hook = 0;
          return done ? (grub_ssize_t) done : -1;
        }
      data->disk->read_hook = 0;
      done += amount;
      pos += amount;
    }

  return done;
}

static grub_err_t
grub_bfrus_close (grub_file_t file)
{
  grub_free (file->data);
  return GRUB_ERR_NONE;
}

static struct grub_fs grub_bfrus_fs =
  {
    .name = "bfrus",
    .fs_dir = grub_bfrus_dir,
    .fs_open = grub_bfrus_open,
    .fs_read = grub_bfrus_read,
    .fs_close = grub_bfrus_close,
    .next = 0
  };

GRUB_MOD_INIT (bfrus)
{
  if (!grub_is_lockdown ())
    {
      grub_bfrus_fs.mod = mod;
      grub_fs_register (&grub_bfrus_fs);
    }
  my_mod = mod;
}

GRUB_MOD_FINI (bfrus)
{
  if (!grub_is_lockdown ())
    grub_fs_unregister (&grub_bfrus_fs);
}
