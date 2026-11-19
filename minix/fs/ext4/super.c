/* This file manages the super block structure.
 *
 * The entry points into this file are
 *   get_super:       search the 'superblock' table for a device
 *   read_super:      read a superblock
 *
 * Created (MFS based):
 *   February 2010 (Evgeniy Ivanov)
 */

#include "fs.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <minix/com.h>
#include <minix/u64.h>
#include <minix/bdev.h>
#include <machine/param.h>
#include <machine/vmparam.h>
#include <sys/mman.h>
#include "buf.h"
#include "inode.h"
#include "super.h"
#include "const.h"

static off_t ext2_max_size(int block_size);
static u32_t ext2_count_dirs(struct super_block *sp);

static void super_copy(register struct super_block *dest, register
	struct super_block *source);
static void copy_group_descriptors(struct group_desc *dest_array,
	const unsigned char *source_array, unsigned int ngroups,
	unsigned int desc_size, int is64);

static off_t super_block_offset;


/*===========================================================================*
 *                              get_super                                    *
 *===========================================================================*/
struct super_block *get_super(
  dev_t dev           /* device number whose super_block is sought */
)
{
  if (dev == NO_DEV)
	panic("request for super_block of NO_DEV");
  if (superblock->s_dev != dev)
	panic("wrong superblock: 0x%x", (int) dev);

  return(superblock);
}


/*===========================================================================*
 *                              get_block_size                               *
 *===========================================================================*/
unsigned int get_block_size(dev_t dev)
{
  if (dev == NO_DEV)
	panic("request for block size of NO_DEV");
  return(lmfs_fs_block_size());
}

static unsigned char *ondisk_group_descs;

/*===========================================================================*
 *                              read_super                                   *
 *                              sp: pointer to superblock					 *
 *===========================================================================*/
int read_super(register struct super_block *sp)
{
	dev_t dev;
	int r, is64;
	static struct group_desc *group_descs;
	u32_t blocks_hi, reserved_hi, free_hi;
	u16_t raw_desc_size;
	u64_t groups64, gdt_position;
	size_t gd_size, off, chunk;

	ondisk_superblock = (struct super_block *)mmap(NULL, SUPER_SIZE_D,
	    PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
	if (ondisk_superblock == MAP_FAILED)
		panic("can't allocate buffer for ext4 superblock");
	dev = sp->s_dev;
	if (dev == NO_DEV)
		panic("request for super_block of NO_DEV");

	if (opt.block_with_super == 0)
		super_block_offset = SUPER_BLOCK_BYTES;
	else
		super_block_offset = opt.block_with_super * 1024;
	r = bdev_read(dev, super_block_offset, (char *)ondisk_superblock,
	    SUPER_SIZE_D, BDEV_NOFLAGS);
	if (r != SUPER_SIZE_D)
		return(EINVAL);

	super_copy(sp, ondisk_superblock);
	sp->s_dev = NO_DEV;
	sp->s_is_ext4 = TRUE;
	if (sp->s_magic != SUPER_MAGIC || sp->s_log_block_size > 6)
		return(EINVAL);

	sp->s_block_size = 1024U << sp->s_log_block_size;
	if (sp->s_block_size < PAGE_SIZE ||
	    sp->s_block_size >= (1U << 16) || /* 64 KiB dirent rec_len needs decoding */
	    (sp->s_block_size % 512) != 0 ||
	    SUPER_SIZE_D > sp->s_block_size)
		return(EINVAL);
	sp->s_sectors_in_block = sp->s_block_size / 512;

	/* ext4 superblock high block counts (offsets 0x150, 0x154, 0x158). */
	memcpy(&blocks_hi, (char *)ondisk_superblock + 0x150, 4);
	memcpy(&reserved_hi, (char *)ondisk_superblock + 0x154, 4);
	memcpy(&free_hi, (char *)ondisk_superblock + 0x158, 4);
	blocks_hi = (u32_t)conv4(le_CPU, blocks_hi);
	reserved_hi = (u32_t)conv4(le_CPU, reserved_hi);
	free_hi = (u32_t)conv4(le_CPU, free_hi);
	sp->s_blocks_count = ((u64_t)blocks_hi << 32) |
	    sp->s_blocks_count_lo;
	sp->s_r_blocks_count = ((u64_t)reserved_hi << 32) |
	    sp->s_r_blocks_count_lo;
	sp->s_free_blocks_count = ((u64_t)free_hi << 32) |
	    sp->s_free_blocks_count_lo;

	memcpy(&raw_desc_size, (char *)ondisk_superblock + 0xFE, 2);
	raw_desc_size = (u16_t)conv2(le_CPU, raw_desc_size);
	is64 = HAS_INCOMPAT_FEATURE(sp, INCOMPAT_64BIT);
	if (is64) {
		if (raw_desc_size != 64)
			return(EINVAL);
		sp->s_desc_size = raw_desc_size;
	} else {
		if ((raw_desc_size != 0 && raw_desc_size != 32) ||
		    blocks_hi != 0 || reserved_hi != 0 || free_hi != 0)
			return(EINVAL);
		sp->s_desc_size = 32;
	}

	if (sp->s_blocks_count == 0 ||
	    sp->s_blocks_count > ((u64_t)1 << 48) ||
	    sp->s_free_blocks_count > sp->s_blocks_count ||
	    sp->s_r_blocks_count > sp->s_blocks_count ||
	    sp->s_blocks_per_group == 0)
		return(EINVAL);
	if ((EXT2_INODE_SIZE(sp) & (EXT2_INODE_SIZE(sp) - 1)) != 0 ||
	    EXT2_INODE_SIZE(sp) < EXT2_GOOD_OLD_INODE_SIZE ||
	    EXT2_INODE_SIZE(sp) > sp->s_block_size)
		return(EINVAL);

	sp->s_blocksize_bits = sp->s_log_block_size + 10;
	sp->s_max_size = ext2_max_size(sp->s_block_size);
	sp->s_inodes_per_block = sp->s_block_size / EXT2_INODE_SIZE(sp);
	if (sp->s_inodes_per_block == 0 || sp->s_inodes_per_group == 0)
		return(EINVAL);
	sp->s_itb_per_group =
	    (sp->s_inodes_per_group + sp->s_inodes_per_block - 1) /
	    sp->s_inodes_per_block;
	sp->s_desc_per_block = sp->s_block_size / sp->s_desc_size;
	if (sp->s_desc_per_block == 0 ||
	    sp->s_blocks_count <= (u64_t)sp->s_first_data_block + 1)
		return(EINVAL);
	groups64 = ((sp->s_blocks_count - sp->s_first_data_block - 1) /
	    sp->s_blocks_per_group) + 1;
	if (groups64 > ((u64_t)(u32_t)~0U))
		return(EFBIG);
	sp->s_groups_count = (u32_t)groups64;
	sp->s_gdb_count = (sp->s_groups_count + sp->s_desc_per_block - 1) /
	    sp->s_desc_per_block;

	if (sp->s_gdb_count > ((size_t)-1) / sp->s_block_size ||
	    sp->s_groups_count > SIZE_MAX / sizeof(*group_descs))
		return(EINVAL);
	gd_size = (size_t)sp->s_gdb_count * sp->s_block_size;
	group_descs = malloc((size_t)sp->s_groups_count * sizeof(*group_descs));
	if (group_descs == NULL)
		panic("can't allocate ext4 group descriptor array");
	ondisk_group_descs = mmap(NULL, gd_size,
	    PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
	if (ondisk_group_descs == MAP_FAILED)
		panic("can't allocate raw ext4 group descriptor buffer");

	if (opt.block_with_super == 0)
		gdt_position = ((u64_t)sp->s_first_data_block + 1) *
		    sp->s_block_size;
	else
		gdt_position = ((u64_t)opt.block_with_super + 1) * 1024;
	for (off = 0; off < gd_size; off += chunk) {
		chunk = gd_size - off;
		if (chunk > PAGE_SIZE)
			chunk = PAGE_SIZE;
		r = bdev_read(dev, gdt_position + off,
		    (char *)ondisk_group_descs + off, chunk, BDEV_NOFLAGS);
		if (r != (ssize_t)chunk) {
			printf("ext4: cannot read group descriptors\\n");
			return(EINVAL);
		}
	}
	copy_group_descriptors(group_descs, ondisk_group_descs,
	    sp->s_groups_count, sp->s_desc_size, is64);
	sp->s_group_desc = group_descs;
	if (sp->s_inodes_count < 1 || sp->s_blocks_count < 1)
		return(EINVAL);

	sp->s_dirs_counter = ext2_count_dirs(sp);
	sp->s_bsearch = (block64_t)sp->s_first_data_block + 1 +
	    sp->s_gdb_count + 2 + sp->s_itb_per_group;
	sp->s_igsearch = 0;
	sp->s_dev = dev;
	return(OK);
}

/*===========================================================================*
 *                              write_super									 *
 *                              super_block: pointer to a superblock		 *
 *===========================================================================*/
void write_super(struct super_block *sp)
{
	(void)sp;
	panic("ext4: attempted metadata write on read-only filesystem");
	return;
}

/*===========================================================================*
 *                              get_group_desc                               *
 *===========================================================================*/
struct group_desc* get_group_desc(unsigned int bnum)
{
  if (bnum >= superblock->s_groups_count) {
	printf("ext2, get_group_desc: wrong bnum (%d) requested\n", bnum);
	return NULL;
  }
  return &superblock->s_group_desc[bnum];
}


static u32_t ext2_count_dirs(struct super_block *sp)
{
  u32_t count = 0;
  unsigned int i;

  for (i = 0; i < sp->s_groups_count; i++) {
	struct group_desc *desc = get_group_desc(i);
	if (!desc)
		continue; /* TODO: fail? */
	count += desc->used_dirs_count;
  }
  return count;
}


/*===========================================================================*
 *                              ext2_max_size                                *
 *===========================================================================*/
/* There are several things, which affect max filesize:
 * - inode.i_blocks (512-byte blocks) is limited to (2^32 - 1).
 * - number of addressed direct, single, double and triple indirect blocks.
 * Number of addressed blocks depends on block_size only, thus unlike in
 * linux (ext2_max_size) we do not make calculations, but use constants
 * for different block sizes. Calculations (gcc code) are commented.
 * Note: linux ext2_max_size makes calculated based on shifting, not
 * arithmetics.
 * (!!!)Note: constants hardly tight to EXT2_NDIR_BLOCKS, but I doubt its value
 * will be changed someday. So if it's changed, then just recalculate constatns.
 * Anyway this function is safe for any change.
 * Note: there is also limitation from VFS (to LONG_MAX, i.e. 2GB).
 */
static off_t ext2_max_size(int block_size)
{
	u64_t max_bytes, logical_limit;

	/* Without HUGE_FILE, i_blocks is a 32-bit count of 512-byte sectors. */
	max_bytes = (u64_t)UINT32_MAX * 512;
	logical_limit = ((u64_t)UINT32_MAX + 1) * (u64_t)block_size;
	if (max_bytes > logical_limit)
		max_bytes = logical_limit;
	return((off_t)max_bytes);
}

/*===========================================================================*
 *				super_copy				     *
 *===========================================================================*/
static void super_copy(
  register struct super_block *dest,
  register struct super_block *source
)
/* Note: we don't convert stuff, used in ext3. */
{
/* Copy super_block to the in-core table, swapping bytes if need be. */
  if (le_CPU) {
	/* Just use memcpy */
	memcpy(dest, source, SUPER_SIZE_D);
	return;
  }
  dest->s_inodes_count = conv4(le_CPU, source->s_inodes_count);
  dest->s_blocks_count_lo = conv4(le_CPU, source->s_blocks_count_lo);
  dest->s_r_blocks_count_lo = conv4(le_CPU, source->s_r_blocks_count_lo);
  dest->s_free_blocks_count_lo = conv4(le_CPU, source->s_free_blocks_count_lo);
  dest->s_free_inodes_count = conv4(le_CPU, source->s_free_inodes_count);
  dest->s_first_data_block = conv4(le_CPU, source->s_first_data_block);
  dest->s_log_block_size = conv4(le_CPU, source->s_log_block_size);
  dest->s_log_frag_size = conv4(le_CPU, source->s_log_frag_size);
  dest->s_blocks_per_group = conv4(le_CPU, source->s_blocks_per_group);
  dest->s_frags_per_group = conv4(le_CPU, source->s_frags_per_group);
  dest->s_inodes_per_group = conv4(le_CPU, source->s_inodes_per_group);
  dest->s_mtime = conv4(le_CPU, source->s_mtime);
  dest->s_wtime = conv4(le_CPU, source->s_wtime);
  dest->s_mnt_count = conv2(le_CPU, source->s_mnt_count);
  dest->s_max_mnt_count = conv2(le_CPU, source->s_max_mnt_count);
  dest->s_magic = conv2(le_CPU, source->s_magic);
  dest->s_state = conv2(le_CPU, source->s_state);
  dest->s_errors = conv2(le_CPU, source->s_errors);
  dest->s_minor_rev_level = conv2(le_CPU, source->s_minor_rev_level);
  dest->s_lastcheck = conv4(le_CPU, source->s_lastcheck);
  dest->s_checkinterval = conv4(le_CPU, source->s_checkinterval);
  dest->s_creator_os = conv4(le_CPU, source->s_creator_os);
  dest->s_rev_level = conv4(le_CPU, source->s_rev_level);
  dest->s_def_resuid = conv2(le_CPU, source->s_def_resuid);
  dest->s_def_resgid = conv2(le_CPU, source->s_def_resgid);
  dest->s_first_ino = conv4(le_CPU, source->s_first_ino);
  dest->s_inode_size = conv2(le_CPU, source->s_inode_size);
  dest->s_block_group_nr = conv2(le_CPU, source->s_block_group_nr);
  dest->s_feature_compat = conv4(le_CPU, source->s_feature_compat);
  dest->s_feature_incompat = conv4(le_CPU, source->s_feature_incompat);
  dest->s_feature_ro_compat = conv4(le_CPU, source->s_feature_ro_compat);
  memcpy(dest->s_uuid, source->s_uuid, sizeof(dest->s_uuid));
  memcpy(dest->s_volume_name, source->s_volume_name,
	 sizeof(dest->s_volume_name));
  memcpy(dest->s_last_mounted, source->s_last_mounted,
	 sizeof(dest->s_last_mounted));
  dest->s_algorithm_usage_bitmap =
			conv4(le_CPU, source->s_algorithm_usage_bitmap);
  dest->s_prealloc_blocks = source->s_prealloc_blocks;
  dest->s_prealloc_dir_blocks = source->s_prealloc_dir_blocks;
  dest->s_padding1 = conv2(le_CPU, source->s_padding1);
}


/*===========================================================================*
 *				gd_copy 				     *
 *===========================================================================*/
static u16_t ext4_read16(const unsigned char *p)
{
	u16_t v;
	memcpy(&v, p, sizeof(v));
	return((u16_t)conv2(le_CPU, v));
}

static u32_t ext4_read32(const unsigned char *p)
{
	u32_t v;
	memcpy(&v, p, sizeof(v));
	return((u32_t)conv4(le_CPU, v));
}

/* Decode the low 32-byte or full 64-byte ext4 group descriptor. */
static void copy_group_descriptors(struct group_desc *dest_array,
	const unsigned char *source_array, unsigned int ngroups,
	unsigned int desc_size, int is64)
{
	unsigned int i;

	for (i = 0; i < ngroups; i++) {
		const unsigned char *p = source_array + (size_t)i * desc_size;
		struct group_desc *gd = &dest_array[i];

		gd->block_bitmap = ext4_read32(p);
		gd->inode_bitmap = ext4_read32(p + 4);
		gd->inode_table = ext4_read32(p + 8);
		gd->free_blocks_count = ext4_read16(p + 12);
		gd->free_inodes_count = ext4_read16(p + 14);
		gd->used_dirs_count = ext4_read16(p + 16);
		if (is64) {
			gd->block_bitmap |= (u64_t)ext4_read32(p + 32) << 32;
			gd->inode_bitmap |= (u64_t)ext4_read32(p + 36) << 32;
			gd->inode_table |= (u64_t)ext4_read32(p + 40) << 32;
			gd->free_blocks_count |=
			    (u32_t)ext4_read16(p + 44) << 16;
			gd->free_inodes_count |=
			    (u32_t)ext4_read16(p + 46) << 16;
			gd->used_dirs_count |=
			    (u32_t)ext4_read16(p + 48) << 16;
		}
	}
}
