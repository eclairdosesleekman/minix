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

static off_t ext2_max_size(struct super_block *sp);
static u32_t ext2_count_dirs(struct super_block *sp);

static void super_copy(register struct super_block *dest, register
	struct super_block *source);
static void copy_group_descriptors(struct group_desc *dest_array,
	const unsigned char *source_array, unsigned int ngroups,
	unsigned int desc_size, int is64);
static u32_t ext4_read32(const unsigned char *p);

static off_t super_block_offset;

static int verify_group_descriptor_checksums(struct super_block *sp,
	const unsigned char *raw, unsigned int ngroups, unsigned int desc_size);
static int verify_group_bitmap_checksums(dev_t dev, struct super_block *sp,
	const unsigned char *raw, struct group_desc *groups,
	unsigned int ngroups, unsigned int desc_size);
static int verify_group_descriptor_block_ranges(struct super_block *sp,
	struct group_desc *groups, unsigned int ngroups);
static void update_group_descriptor_checksums(
	struct super_block *sp);


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

/*
 * META_BG stores the primary descriptor block in the first group of each
 * metablock group and redundant copies in its second and last groups. A
 * backup superblock, when present, occupies the preceding block(s).
 */
static int
ext4_meta_bg_is_power(u32_t group, u32_t base)
{
	if (group < base)
		return(FALSE);
	while (group % base == 0)
		group /= base;
	return(group == 1);
}

/*
 * Return whether a block group contains a superblock copy. sparse_super2
 * takes precedence over sparse_super and lists its backup groups in the raw
 * superblock's s_backup_bgs[2] field at offsets 0x24c and 0x250.
 */
static int
ext4_group_has_super(struct super_block *sp, u32_t group)
{
	const unsigned char *raw = (const unsigned char *)ondisk_superblock;
	u32_t backup0, backup1;

	if (group == 0)
		return(TRUE);
	if (HAS_COMPAT_FEATURE(sp, COMPAT_SPARSE_SUPER2)) {
		backup0 = ext4_read32(raw + 0x24c);
		backup1 = ext4_read32(raw + 0x250);
		return(group == backup0 || group == backup1);
	}
	if (!HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_SPARSE_SUPER))
		return(TRUE);
	if (group == 1 || ext4_meta_bg_is_power(group, 3) ||
	    ext4_meta_bg_is_power(group, 5) ||
	    ext4_meta_bg_is_power(group, 7))
		return(TRUE);
	return(FALSE);
}

static int
ext4_meta_bg_group_has_super(struct super_block *sp, u32_t group)
{
	return(ext4_group_has_super(sp, group));
}

/*
 * Validate the two optional sparse_super2 backup group numbers before they
 * are used to calculate any on-disk metadata locations. Zero means that an
 * entry is unused; either slot may therefore be zero independently.
 */
static int
ext4_validate_sparse_super2(struct super_block *sp)
{
	const unsigned char *raw = (const unsigned char *)ondisk_superblock;
	u32_t backup0, backup1;

	if (!HAS_COMPAT_FEATURE(sp, COMPAT_SPARSE_SUPER2))
		return(OK);
	if (opt.block_with_super != 0)
		return(EOPNOTSUPP);

	backup0 = ext4_read32(raw + 0x24c);
	backup1 = ext4_read32(raw + 0x250);
	if ((backup0 != 0 && backup0 >= sp->s_groups_count) ||
	    (backup1 != 0 && backup1 >= sp->s_groups_count))
		return(EINVAL);
	return(OK);
}

static int
ext4_meta_bg_group_descriptor_block(struct super_block *sp,
	unsigned int index, u32_t group, block64_t *blockp)
{
	u64_t first_group, last_group, first_block, block, max_value;
	unsigned int has_super;

	if (!HAS_INCOMPAT_FEATURE(sp, INCOMPAT_META_BG) ||
	    index < sp->s_first_meta_bg || index >= sp->s_gdb_count ||
	    sp->s_desc_per_block == 0 || sp->s_blocks_per_group == 0)
		return(FALSE);

	first_group = (u64_t)index * sp->s_desc_per_block;
	last_group = first_group + sp->s_desc_per_block - 1;
	if (group >= sp->s_groups_count ||
	    ((u64_t)group != first_group &&
	    (u64_t)group != first_group + 1 &&
	    (u64_t)group != last_group))
		return(FALSE);

	max_value = ((u64_t)~(u64_t)0) >> 1; /* signed off_t limit */
	if ((u64_t)group >
	    (max_value - sp->s_first_data_block) / sp->s_blocks_per_group)
		return(FALSE);
	first_block = (u64_t)sp->s_first_data_block +
	    (u64_t)group * sp->s_blocks_per_group;

	has_super = ext4_meta_bg_group_has_super(sp, group);
	/*
	 * With 1 KiB blocks, ext4's first metablock group can have an
	 * additional reserved block before its descriptor block.
	 */
	if (sp->s_block_size == 1024 && index == 0 && group == 0 &&
	    sp->s_first_data_block == 0)
		has_super++;

	if (first_block > max_value - has_super)
		return(FALSE);
	block = first_block + has_super;
	if (block >= sp->s_blocks_count || sp->s_block_size == 0 ||
	    block > (max_value - (sp->s_block_size - 1)) /
	    sp->s_block_size)
		return(FALSE);

	*blockp = (block64_t)block;
	return(TRUE);
}

static int
ext4_meta_bg_descriptor_block(struct super_block *sp, unsigned int index,
	block64_t *blockp)
{
	u64_t first_group;

	if (index >= sp->s_gdb_count || sp->s_desc_per_block == 0)
		return(FALSE);
	first_group = (u64_t)index * sp->s_desc_per_block;
	if (first_group >= sp->s_groups_count)
		return(FALSE);
	return(ext4_meta_bg_group_descriptor_block(sp, index,
	    (u32_t)first_group, blockp));
}

/*
 * BIGALLOC reserves allocation clusters, not individual blocks. File-data
 * mappings must not expose another block in a cluster containing a backup
 * superblock/GDT copy or any group's allocation bitmap/inode table. Metadata
 * pointers themselves are checked separately at block granularity because
 * ext4 may place multiple metadata blocks in one cluster.
 */
int
ext4_data_overlaps_reserved_clusters(struct super_block *sp, block64_t block)
{
	block64_t cluster_first, cluster_end;
	u64_t cluster_index, start, end;
	u64_t lo, hi, mid;

	if (block == NO_BLOCK)
		return(FALSE);
	if (block >= sp->s_blocks_count)
		return(TRUE);
	if (!HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_BIGALLOC))
		return(ext4_overlaps_super_and_descriptor_blocks(sp, block,
		    block + 1));
	if (block < sp->s_first_data_block || sp->s_cluster_ratio == 0)
		return(TRUE);

	cluster_index = (block - sp->s_first_data_block) /
	    sp->s_cluster_ratio;
	start = (u64_t)sp->s_first_data_block +
	    cluster_index * sp->s_cluster_ratio;
	end = start + sp->s_cluster_ratio;
	if (end > sp->s_blocks_count)
		end = sp->s_blocks_count;
	if (start >= end || end > ((u64_t)~(u64_t)0 >> 1))
		return(TRUE);
	cluster_first = (block64_t)start;
	cluster_end = (block64_t)end;

	if (ext4_overlaps_super_and_descriptor_blocks(sp, cluster_first,
	    cluster_end))
		return(TRUE);

	/*
	 * Ranges are sorted and disjoint by verify_group_descriptor_block_ranges.
	 * Find the first range whose end exceeds this cluster's first block.
	 */
	if (sp->s_metadata_range_count != 0 &&
	    sp->s_metadata_ranges == NULL)
		return(TRUE);
	lo = 0;
	hi = sp->s_metadata_range_count;
	while (lo < hi) {
		mid = lo + (hi - lo) / 2;
		if (sp->s_metadata_ranges[mid].end <= cluster_first)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < sp->s_metadata_range_count &&
	    sp->s_metadata_ranges[lo].first < cluster_end)
		return(TRUE);
	return(FALSE);
}

/*
 * Test a physical half-open range against backup superblocks and all group
 * descriptor copies. Classic GDT copies are present in every backup group
 * selected by sparse_super/sparse_super2; META_BG keeps its classic prefix
 * there and places later descriptor blocks in the first, second, and last
 * groups of each metablock group.
 */
int
ext4_overlaps_super_and_descriptor_blocks(struct super_block *sp,
	block64_t first, block64_t end)
{
	u64_t first_group, last_group, group, group_start;
	u64_t first_index, last_index, index, meta_first;
	u64_t classic_group_count, classic_last_group;
	u64_t descriptor_start, descriptor_end, max_value;
	u64_t span_groups, backup_first_group;
	u64_t candidates[3];
	unsigned int i;
	u64_t classic_reserved_blocks;
	block64_t descriptor;
	int meta_bg;

	if (first >= end || sp->s_blocks_per_group == 0 ||
	    sp->s_groups_count == 0 ||
	    (first < sp->s_first_data_block &&
	    end <= sp->s_first_data_block))
		return(FALSE);

	first_group = first <= sp->s_first_data_block ? 0 :
	    (first - sp->s_first_data_block) / sp->s_blocks_per_group;
	last_group = end - 1 <= sp->s_first_data_block ? 0 :
	    (end - 1 - sp->s_first_data_block) / sp->s_blocks_per_group;
	if (first_group >= sp->s_groups_count)
		return(FALSE);
	if (last_group >= sp->s_groups_count)
		last_group = sp->s_groups_count - 1;

	max_value = ((u64_t)~(u64_t)0) >> 1; /* signed off_t limit */
	meta_bg = HAS_INCOMPAT_FEATURE(sp, INCOMPAT_META_BG);

	/* Every backup superblock occupies the first block of its group. */
	for (group = first_group; group <= last_group; group++) {
		if (!ext4_group_has_super(sp, (u32_t)group))
			continue;
		if (group > (max_value - sp->s_first_data_block) /
		    sp->s_blocks_per_group)
			return(FALSE);
		group_start = (u64_t)sp->s_first_data_block +
		    group * sp->s_blocks_per_group;
		if (group_start < sp->s_blocks_count &&
		    group_start >= first && group_start < end)
			return(TRUE);
	}

	/*
	 * The primary classic GDT starts immediately after the primary
	 * superblock and can extend across a block-group boundary.
	 */
	if (meta_bg)
		classic_reserved_blocks = sp->s_first_meta_bg;
	else
		classic_reserved_blocks = sp->s_gdb_count;
	/* resize_inode reserves additional blocks after each classic GDT copy. */
	classic_reserved_blocks += sp->s_reserved_gdt_blocks;
	descriptor_start = (u64_t)sp->s_first_data_block + 1;
	if (sp->s_block_size == 1024 && sp->s_first_data_block == 0)
		descriptor_start++;
	if (classic_reserved_blocks != 0 &&
	    descriptor_start < sp->s_blocks_count) {
		if (classic_reserved_blocks > sp->s_blocks_count - descriptor_start)
			descriptor_end = sp->s_blocks_count;
		else
			descriptor_end = descriptor_start + classic_reserved_blocks;
		if (descriptor_start < end && first < descriptor_end)
			return(TRUE);
	}

	/*
	 * Classic backup GDT copies and any reserved resize_inode GDT blocks
	 * occupy the blocks immediately after each backup superblock. In
	 * META_BG filesystems only the prefix before s_first_meta_bg remains
	 * in this layout. Include preceding backup groups whose reserved
	 * range can extend into the queried physical group.
	 */
	if (meta_bg) {
		classic_group_count = (u64_t)sp->s_first_meta_bg *
		    sp->s_desc_per_block;
		if (classic_group_count > sp->s_groups_count)
			classic_group_count = sp->s_groups_count;
	} else {
		classic_group_count = sp->s_groups_count;
	}
	if (classic_reserved_blocks != 0) {
		span_groups = (u64_t)classic_reserved_blocks /
		    sp->s_blocks_per_group + 1;
		backup_first_group = first_group > span_groups ?
		    first_group - span_groups : 0;
		if (backup_first_group < classic_group_count) {
			classic_last_group = last_group;
			if (classic_last_group >= classic_group_count)
				classic_last_group = classic_group_count - 1;
			for (group = backup_first_group;
			    group <= classic_last_group; group++) {
				if (!ext4_group_has_super(sp, (u32_t)group))
					continue;
				if (group > (max_value - sp->s_first_data_block) /
				    sp->s_blocks_per_group)
					return(FALSE);
				group_start = (u64_t)sp->s_first_data_block +
				    group * sp->s_blocks_per_group;
				if (group_start >= sp->s_blocks_count)
					continue;
				descriptor_start = group_start + 1;
				/* Preserve the 1 KiB first-group layout special case. */
				if (sp->s_block_size == 1024 && group == 0 &&
				    sp->s_first_data_block == 0)
					descriptor_start++;
				if (descriptor_start >= sp->s_blocks_count)
					continue;
				if (classic_reserved_blocks >
				    sp->s_blocks_count - descriptor_start)
					descriptor_end = sp->s_blocks_count;
				else
					descriptor_end = descriptor_start +
					    classic_reserved_blocks;
				if (descriptor_start < end && first < descriptor_end)
					return(TRUE);
			}
		}
	}

	if (!meta_bg)
		return(FALSE);

	/* Later META_BG descriptor blocks live at three locations per metablock. */
	first_index = first_group / sp->s_desc_per_block;
	if (first_index < sp->s_first_meta_bg)
		first_index = sp->s_first_meta_bg;
	last_index = last_group / sp->s_desc_per_block;
	if (first_index > last_index)
		return(FALSE);

	for (index = first_index; index <= last_index &&
	    index < sp->s_gdb_count; index++) {
		meta_first = index * sp->s_desc_per_block;
		candidates[0] = meta_first;
		candidates[1] = meta_first + 1;
		candidates[2] = meta_first + sp->s_desc_per_block - 1;
		for (i = 0; i < 3; i++) {
			if (candidates[i] >= sp->s_groups_count)
				continue;
			if (ext4_meta_bg_group_descriptor_block(sp,
			    (unsigned int)index, (u32_t)candidates[i],
			    &descriptor) && descriptor >= first &&
			    descriptor < end)
				return(TRUE);
		}
	}
	return(FALSE);
}

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
	size_t gd_size, classic_gdt_size, off, chunk;
	unsigned int classic_gdb_count, gdb;
	block64_t descriptor_block;

	ondisk_superblock = (struct super_block *)mmap(NULL, SUPER_SIZE_D,
	    PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
	if (ondisk_superblock == MAP_FAILED)
		panic("can't allocate buffer for ext4 superblock");
	dev = sp->s_dev;
	if (dev == NO_DEV)
		panic("request for super_block of NO_DEV");

	/* Do not carry pending descriptor writes across failed mount attempts. */
	group_descriptors_dirty = 0;

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
	if (HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_METADATA_CSUM)) {
		u32_t stored_checksum, calculated_checksum;

		/* ext4 currently defines only CRC32C for metadata_csum. */
		if (*((unsigned char *)ondisk_superblock + 0x175) !=
		    EXT4_CRC32C_CHKSUM)
			return(EINVAL);
		memcpy(&stored_checksum,
		    (unsigned char *)ondisk_superblock + 0x3fc,
		    sizeof(stored_checksum));
		stored_checksum = (u32_t)conv4(le_CPU, stored_checksum);
		calculated_checksum = ext4_crc32c(~0U,
		    ondisk_superblock, 0x3fc);
		if (stored_checksum != calculated_checksum)
			return(EINVAL);
		if (HAS_INCOMPAT_FEATURE(sp, INCOMPAT_CSUM_SEED)) {
			/* s_checksum_seed is at offset 0x270 in the ext4 superblock. */
			sp->s_csum_seed = ext4_read32(
			    (const unsigned char *)ondisk_superblock + 0x270);
		} else {
			sp->s_csum_seed = ext4_crc32c(~0U, sp->s_uuid,
			    sizeof(sp->s_uuid));
		}
	} else {
		if (HAS_INCOMPAT_FEATURE(sp, INCOMPAT_CSUM_SEED)) {
			/* A checksum seed has no meaning without metadata_csum. */
			return(EINVAL);
		}
		/*
		 * EA inode value hashes use the UUID-derived CRC32C seed even
		 * when metadata_csum is disabled. Linux ext4 initializes this
		 * seed for either metadata_csum or ea_inode filesystems.
		 */
		if (HAS_INCOMPAT_FEATURE(sp, INCOMPAT_EA_INODE))
			sp->s_csum_seed = ext4_crc32c(~0U, sp->s_uuid,
			    sizeof(sp->s_uuid));
	}
	sp->s_is_ext4 = TRUE;
	if (sp->s_magic != SUPER_MAGIC || sp->s_log_block_size > 6)
		return(EINVAL);

	sp->s_block_size = 1024U << sp->s_log_block_size;
	if (sp->s_block_size < PAGE_SIZE ||
	    sp->s_block_size > (1U << 16) || /* larger blocks need unsupported rec_len formats */
	    (sp->s_block_size % 512) != 0 ||
	    SUPER_SIZE_D > sp->s_block_size)
		return(EINVAL);
	sp->s_sectors_in_block = sp->s_block_size / 512;

	/*
	 * s_log_frag_size and s_frags_per_group were renamed to
	 * s_log_cluster_size and s_clusters_per_group by ext4. Without
	 * BIGALLOC, the cluster is exactly one filesystem block. With
	 * BIGALLOC, each block bitmap bit denotes a power-of-two run of blocks.
	 */
	if (HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_BIGALLOC)) {
		u32_t cluster_bits;
		u64_t cluster_ratio, blocks_per_group;

		if (sp->s_log_frag_size < sp->s_log_block_size)
			return(EINVAL);
		cluster_bits = sp->s_log_frag_size - sp->s_log_block_size;
		if (cluster_bits >= 32)
			return(EINVAL);
		cluster_ratio = (u64_t)1 << cluster_bits;
		if (sp->s_frags_per_group == 0 ||
		    (u64_t)sp->s_frags_per_group >
		    (u64_t)sp->s_block_size * 8)
			return(EINVAL);
		blocks_per_group = (u64_t)sp->s_frags_per_group *
		    cluster_ratio;
		if (blocks_per_group != sp->s_blocks_per_group)
			return(EINVAL);
		sp->s_cluster_ratio = (u32_t)cluster_ratio;
		sp->s_clusters_per_group = sp->s_frags_per_group;
	} else {
		if (sp->s_log_frag_size != sp->s_log_block_size ||
		    sp->s_frags_per_group != sp->s_blocks_per_group)
			return(EINVAL);
		sp->s_cluster_ratio = 1;
		sp->s_clusters_per_group = sp->s_blocks_per_group;
	}

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
	sp->s_max_size = ext2_max_size(sp);
	sp->s_inodes_per_block = sp->s_block_size / EXT2_INODE_SIZE(sp);
	if (sp->s_inodes_per_block == 0 || sp->s_inodes_per_group == 0)
		return(EINVAL);
	/* Avoid overflow in the usual (n + d - 1) / d ceiling formula. */
	sp->s_itb_per_group = sp->s_inodes_per_group /
	    sp->s_inodes_per_block;
	if (sp->s_inodes_per_group % sp->s_inodes_per_block != 0)
		sp->s_itb_per_group++;
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

	/*
	 * sparse_super2 backup groups are read from s_backup_bgs[2]. Validate
	 * their indices before using them when checking reserved metadata.
	 * Alternate-superblock selection remains unsupported for sparse_super2
	 * and META_BG because this server reads the primary descriptor layout.
	 */
	r = ext4_validate_sparse_super2(sp);
	if (r != OK)
		return(r);
	if (HAS_INCOMPAT_FEATURE(sp, INCOMPAT_META_BG)) {
		/* e2fsprogs/Linux do not support resize_inode with META_BG. */
		if (HAS_COMPAT_FEATURE(sp, COMPAT_RESIZE_INO))
			return(EOPNOTSUPP);
		if (sp->s_first_meta_bg > sp->s_gdb_count ||
		    opt.block_with_super != 0)
			return(EOPNOTSUPP);
		classic_gdb_count = sp->s_first_meta_bg;
	} else {
		classic_gdb_count = sp->s_gdb_count;
	}

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

	/* Read the descriptor blocks that retain their classic contiguous location. */
	classic_gdt_size = (size_t)classic_gdb_count * sp->s_block_size;
	if (classic_gdt_size != 0 &&
	    (gdt_position > (((u64_t)~(u64_t)0) >> 1) ||
	    classic_gdt_size > (((u64_t)~(u64_t)0) >> 1) - gdt_position))
		return(EINVAL);
	for (off = 0; off < classic_gdt_size; off += chunk) {
		chunk = classic_gdt_size - off;
		if (chunk > PAGE_SIZE)
			chunk = PAGE_SIZE;
		r = bdev_read(dev, gdt_position + off,
		    (char *)ondisk_group_descs + off, chunk, BDEV_NOFLAGS);
		if (r != (ssize_t)chunk) {
			printf("ext4: cannot read classic group descriptors\\n");
			return(EINVAL);
		}
	}

	/*
	 * Read each META_BG primary descriptor block into its logical slot.
	 * Checksum validation below then uses the same dense descriptor array
	 * as the classic layout.
	 */
	for (gdb = classic_gdb_count; gdb < sp->s_gdb_count; gdb++) {
		if (!ext4_meta_bg_descriptor_block(sp, gdb, &descriptor_block))
			return(EINVAL);
		r = bdev_read(dev, (u64_t)descriptor_block * sp->s_block_size,
		    (char *)ondisk_group_descs +
		    (size_t)gdb * sp->s_block_size,
		    sp->s_block_size, BDEV_NOFLAGS);
		if (r != (ssize_t)sp->s_block_size) {
			printf("ext4: cannot read META_BG descriptor block %u\\n",
			    gdb);
			return(EINVAL);
		}
	}
	if ((HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_METADATA_CSUM) ||
	    HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_GDT_CSUM)) &&
	    !verify_group_descriptor_checksums(sp, ondisk_group_descs,
	    sp->s_groups_count, sp->s_desc_size))
		return(EINVAL);
	copy_group_descriptors(group_descs, ondisk_group_descs,
	    sp->s_groups_count, sp->s_desc_size, is64);
	r = verify_group_descriptor_block_ranges(sp, group_descs,
	    sp->s_groups_count);
	if (r != OK)
		return(r);
	if (HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_METADATA_CSUM)) {
		r = verify_group_bitmap_checksums(dev, sp, ondisk_group_descs,
		    group_descs, sp->s_groups_count, sp->s_desc_size);
		if (r != OK)
			return(r);
	}
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
static void
ext4_store16(unsigned char *p, u16_t value)
{
	value = (u16_t)conv2(le_CPU, value);
	memcpy(p, &value, sizeof(value));
}

static void
ext4_store32(unsigned char *p, u32_t value)
{
	value = (u32_t)conv4(le_CPU, value);
	memcpy(p, &value, sizeof(value));
}

static int
ext4_write_descriptor_bytes(struct super_block *sp, u64_t disk_offset,
	const unsigned char *data, size_t length)
{
	u64_t max_offset;
	size_t off, chunk;
	ssize_t written;

	max_offset = ((u64_t)~(u64_t)0) >> 1;
	if (disk_offset > max_offset || length > max_offset - disk_offset)
		return(EINVAL);

	for (off = 0; off < length; off += chunk) {
		chunk = length - off;
		if (chunk > PAGE_SIZE)
			chunk = PAGE_SIZE;
		written = bdev_write(sp->s_dev, disk_offset + off,
		    (char *)data + off, chunk, BDEV_NOFLAGS);
		if (written != (ssize_t)chunk)
			return(EIO);
	}
	return(OK);
}

/*===========================================================================*
 *                              write_super                                  *
 *===========================================================================*/
void
write_super(struct super_block *sp)
{
	int r;
	size_t gd_size, classic_gd_size, off;
	u64_t gdt_position, descriptor_position;
	block64_t descriptor_block;
	unsigned int i;

	if (sp->s_rd_only)
		return;
	if (sp->s_dev == NO_DEV)
		panic("ext4: request to write superblock for NO_DEV");

	/* Keep the low and high halves of the 64-bit free-block count in sync. */
	sp->s_free_blocks_count_lo = (u32_t)sp->s_free_blocks_count;
	sp->s_wtime = clock_time(NULL);

	if (group_descriptors_dirty) {
		gd_size = (size_t)sp->s_gdb_count * sp->s_block_size;
		for (i = 0; i < sp->s_groups_count; i++) {
			unsigned char *p = ondisk_group_descs +
			    (size_t)i * sp->s_desc_size;
			struct group_desc *gd = &sp->s_group_desc[i];

			ext4_store16(p + 12, (u16_t)gd->free_blocks_count);
			ext4_store16(p + 14, (u16_t)gd->free_inodes_count);
			ext4_store16(p + 16, (u16_t)gd->used_dirs_count);
			if (sp->s_desc_size == 64) {
				ext4_store16(p + 44,
				    (u16_t)(gd->free_blocks_count >> 16));
				ext4_store16(p + 46,
				    (u16_t)(gd->free_inodes_count >> 16));
				ext4_store16(p + 48,
				    (u16_t)(gd->used_dirs_count >> 16));
			}
		}

		update_group_descriptor_checksums(sp);

		if (opt.block_with_super == 0)
			gdt_position = ((u64_t)sp->s_first_data_block + 1) *
			    sp->s_block_size;
		else
			gdt_position = ((u64_t)opt.block_with_super + 1) * 1024;

		if (!HAS_INCOMPAT_FEATURE(sp, INCOMPAT_META_BG)) {
			/*
			 * Classic filesystems keep every primary descriptor block
			 * in one contiguous table immediately after the superblock.
			 */
			r = ext4_write_descriptor_bytes(sp, gdt_position,
			    ondisk_group_descs, gd_size);
			if (r != OK) {
				printf("ext4: cannot write group descriptors\\n");
				return;
			}
		} else {
			/*
			 * META_BG keeps only the descriptor prefix before
			 * s_first_meta_bg in the classic table. Each remaining
			 * primary descriptor block lives in the first group of its
			 * metablock group. The in-memory/raw descriptor array stays
			 * dense, so checksums and descriptor indexing are unchanged.
			 */
			classic_gd_size = (size_t)sp->s_first_meta_bg *
			    sp->s_block_size;
			if (classic_gd_size > gd_size) {
				printf("ext4: invalid META_BG descriptor prefix\\n");
				return;
			}
			if (classic_gd_size != 0 &&
			    ext4_write_descriptor_bytes(sp, gdt_position,
			    ondisk_group_descs, classic_gd_size) != OK) {
				printf("ext4: cannot write classic META_BG prefix\\n");
				return;
			}

			for (i = sp->s_first_meta_bg; i < sp->s_gdb_count; i++) {
				if (!ext4_meta_bg_descriptor_block(sp, i,
				    &descriptor_block)) {
					printf("ext4: invalid META_BG descriptor block %u\\n",
					    i);
					return;
				}
				descriptor_position = (u64_t)descriptor_block *
				    sp->s_block_size;
				off = (size_t)i * sp->s_block_size;
				r = ext4_write_descriptor_bytes(sp,
				    descriptor_position, ondisk_group_descs + off,
				    sp->s_block_size);
				if (r != OK) {
					printf("ext4: cannot write META_BG descriptor block %u\\n",
					    i);
					return;
				}
			}
		}
		group_descriptors_dirty = 0;
	}

	/* Preserve all feature and geometry fields while updating counters. */
	super_copy(ondisk_superblock, sp);
	ext4_store32((unsigned char *)ondisk_superblock + 0x158,
	    (u32_t)(sp->s_free_blocks_count >> 32));
	if (HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_METADATA_CSUM)) {
		u32_t checksum;

		/* ext4 superblock checksum covers bytes [0, 0x3fc). */
		ext4_store32((unsigned char *)ondisk_superblock + 0x3fc, 0);
		checksum = ext4_crc32c(~0U, ondisk_superblock, 0x3fc);
		ext4_store32((unsigned char *)ondisk_superblock + 0x3fc,
		    checksum);
	}

	r = bdev_write(sp->s_dev, super_block_offset,
	    (char *)ondisk_superblock, SUPER_SIZE_D, BDEV_NOFLAGS);
	if (r != SUPER_SIZE_D)
		printf("ext4: warning, failed to write superblock\\n");
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

/*
 * Update the ext4 block or inode bitmap checksum in its group descriptor.
 * The checksum is CRC32C(fs checksum seed, bitmap bytes); 64-byte descriptors
 * store the upper half while 32-byte descriptors retain only the low half.
 */
int
ext4_update_bitmap_checksum(struct super_block *sp, unsigned int group,
	const unsigned char *bitmap, int inode_bitmap)
{
	unsigned int bytes, checksum_offset, checksum_hi_offset;
	unsigned char *desc;
	u32_t checksum;

	if (!HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_METADATA_CSUM))
		return(OK);
	if (group >= sp->s_groups_count || bitmap == NULL ||
	    sp->s_desc_size < 32)
		return(EINVAL);

	if (inode_bitmap) {
		bytes = sp->s_inodes_per_group / 8;
		checksum_offset = 26;
		checksum_hi_offset = 58;
	} else {
		bytes = sp->s_blocks_per_group / 8;
		checksum_offset = 24;
		checksum_hi_offset = 56;
	}
	if (bytes == 0 || bytes > sp->s_block_size)
		return(EINVAL);

	checksum = ext4_crc32c(sp->s_csum_seed, bitmap, bytes);
	desc = ondisk_group_descs + (size_t)group * sp->s_desc_size;
	ext4_store16(desc + checksum_offset, (u16_t)checksum);
	if (sp->s_desc_size >= 64)
		ext4_store16(desc + checksum_hi_offset, (u16_t)(checksum >> 16));

	group_descriptors_dirty = 1;
	return(OK);
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
static off_t ext2_max_size(struct super_block *sp)
{
	u64_t max_bytes, logical_limit;

	/*
	 * Without HUGE_FILE, i_blocks is a 32-bit count of 512-byte sectors.
	 * With the ext4 huge_file feature, the on-disk counter is 48 bits
	 * wide (and may be expressed in filesystem blocks), so the extent
	 * logical-block limit becomes the tighter bound.
	 */
	logical_limit = ((u64_t)UINT32_MAX + 1) * sp->s_block_size;
	if (HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_HUGE_FILE))
		max_bytes = logical_limit;
	else
		max_bytes = (u64_t)UINT32_MAX * 512;

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
  dest->s_first_meta_bg = conv4(le_CPU, source->s_first_meta_bg);
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
  dest->s_reserved_gdt_blocks = conv2(le_CPU, source->s_reserved_gdt_blocks);
}



/*
 * The legacy gdt_csum format uses Linux's reflected CRC16 (polynomial
 * 0xa001) over the UUID, little-endian group number, and descriptor.
 * metadata_csum supersedes it and uses CRC32C with the filesystem seed.
 */
static u16_t
ext4_crc16(u16_t crc, const void *data, size_t len)
{
	const unsigned char *p = data;
	unsigned int bit;

	while (len-- != 0) {
		crc ^= *p++;
		for (bit = 0; bit < 8; bit++)
			crc = (u16_t)((crc >> 1) ^
			    (0xa001U & (u16_t)-(int)(crc & 1)));
	}
	return(crc);
}

/*
 * Recompute group descriptor checksums after the in-memory counters have
 * been serialized into the raw descriptor array. metadata_csum uses CRC32C
 * seeded by the filesystem checksum seed and group number; legacy gdt_csum
 * uses CRC16 seeded by the filesystem UUID and group number. Both formats
 * omit bg_checksum (bytes 30..31) while calculating the checksum.
 */
static void
update_group_descriptor_checksums(struct super_block *sp)
{
	static const u16_t zero16 = 0;
	unsigned char *p;
	u32_t group_le, crc32;
	u16_t crc16;
	unsigned int i;

	if (!HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_GDT_CSUM) &&
	    !HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_METADATA_CSUM))
		return;

	for (i = 0; i < sp->s_groups_count; i++) {
		p = ondisk_group_descs + (size_t)i * sp->s_desc_size;
		group_le = (u32_t)conv4(le_CPU, i);

		if (HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_METADATA_CSUM)) {
			/*
			 * metadata_csum supersedes the legacy CRC16 format.
			 * The bg_checksum field is treated as zero and only the
			 * low 16 bits of CRC32C are stored.
			 */
			crc32 = ext4_crc32c(sp->s_csum_seed, &group_le,
			    sizeof(group_le));
			crc32 = ext4_crc32c(crc32, p, 30);
			crc32 = ext4_crc32c(crc32, &zero16, sizeof(zero16));
			if (sp->s_desc_size > 32)
				crc32 = ext4_crc32c(crc32, p + 32,
				    sp->s_desc_size - 32);
			ext4_store16(p + 30, (u16_t)crc32);
		} else {
			crc16 = ext4_crc16((u16_t)~0U, sp->s_uuid,
			    sizeof(sp->s_uuid));
			crc16 = ext4_crc16(crc16, &group_le, sizeof(group_le));
			crc16 = ext4_crc16(crc16, p, 30);
			if (sp->s_desc_size > 32)
				crc16 = ext4_crc16(crc16, p + 32,
				    sp->s_desc_size - 32);
			ext4_store16(p + 30, crc16);
		}
	}
}

static int
verify_group_descriptor_checksums(struct super_block *sp,
	const unsigned char *raw, unsigned int ngroups, unsigned int desc_size)
{
	static const u16_t zero16 = 0;
	unsigned int i;
	u32_t group_le, crc32;
	u16_t expected, crc16, stored;
	const unsigned char *p;

	if (desc_size < 32)
		return(FALSE);
	for (i = 0; i < ngroups; i++) {
		p = raw + (size_t)i * desc_size;
		group_le = (u32_t)conv4(le_CPU, i);
		if (HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_METADATA_CSUM)) {
			crc32 = ext4_crc32c(sp->s_csum_seed, &group_le,
			    sizeof(group_le));
			crc32 = ext4_crc32c(crc32, p, 30);
			crc32 = ext4_crc32c(crc32, &zero16, sizeof(zero16));
			if (desc_size > 32)
				crc32 = ext4_crc32c(crc32, p + 32,
				    desc_size - 32);
			expected = (u16_t)crc32;
		} else {
			crc16 = ext4_crc16((u16_t)~0U, sp->s_uuid,
			    sizeof(sp->s_uuid));
			crc16 = ext4_crc16(crc16, &group_le,
			    sizeof(group_le));
			crc16 = ext4_crc16(crc16, p, 30);
			/* The 64-bit descriptor extends beyond bg_checksum. */
			if (desc_size > 32)
				crc16 = ext4_crc16(crc16, p + 32,
				    desc_size - 32);
			expected = crc16;
		}
		memcpy(&stored, p + 30, sizeof(stored));
		stored = (u16_t)conv2(le_CPU, stored);
		if (expected != stored)
			return(FALSE);
	}
	return(TRUE);
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

/*
 * Verify both initialized allocation bitmaps for every block group before
 * the filesystem is made available. Bitmap checksums use CRC32C(seed, bitmap
 * bytes) over the defined bitmap length.
 *
 * Uninitialized bitmaps are intentionally skipped: the on-disk block is
 * not authoritative until ext4 initializes it from group metadata.
 */
static int
verify_one_group_bitmap(dev_t dev, struct super_block *sp,
	block64_t bitmap_block, const unsigned char *raw_desc,
	unsigned int checksum_offset, unsigned int checksum_hi_offset,
	unsigned int desc_size, unsigned int bitmap_bytes,
	unsigned char *bitmap)
{
	u64_t disk_offset;
	u32_t crc, stored;
	int r;

	if (bitmap_block <= sp->s_first_data_block ||
	    bitmap_block >= sp->s_blocks_count)
		return(EIO);
	if (bitmap_block > (((u64_t)~0 >> 1) / sp->s_block_size))
		return(EFBIG);

	disk_offset = bitmap_block * sp->s_block_size;
	r = bdev_read(dev, (off_t)disk_offset, (char *)bitmap,
	    sp->s_block_size, BDEV_NOFLAGS);
	if (r != (int)sp->s_block_size)
		return(EIO);
	if (bitmap_bytes == 0 || bitmap_bytes > sp->s_block_size)
		return(EINVAL);

	/*
	 * Linux ext4 hashes only the bitmap's defined bytes. With BIGALLOC,
	 * one block-bitmap bit represents one cluster, not one filesystem
	 * block. Inode bitmap bits continue to represent individual inodes.
	 */
	crc = ext4_crc32c(sp->s_csum_seed, bitmap, bitmap_bytes);

	stored = ext4_read16(raw_desc + checksum_offset);
	/* The low checksum field occupies 16 bits, not 32. */
	stored &= 0xffffU;
	if (desc_size >= 64) {
		stored |= (u32_t)ext4_read16(raw_desc + checksum_hi_offset) << 16;
		if (stored != crc)
			return(EIO);
	} else if ((u16_t)stored != (u16_t)crc) {
		return(EIO);
	}

	return(OK);
}

static int
verify_group_bitmap_checksums(dev_t dev, struct super_block *sp,
	const unsigned char *raw, struct group_desc *groups,
	unsigned int ngroups, unsigned int desc_size)
{
	unsigned char *bitmap;
	unsigned int i;
	const unsigned char *desc;
	u16_t flags;
	int r;

	bitmap = malloc(sp->s_block_size);
	if (bitmap == NULL)
		return(ENOMEM);

	for (i = 0; i < ngroups; i++) {
		desc = raw + (size_t)i * desc_size;
		flags = ext4_read16(desc + 18);

		if (!(flags & EXT4_BG_BLOCK_UNINIT)) {
			r = verify_one_group_bitmap(dev, sp,
			    groups[i].block_bitmap, desc, 24, 56,
			    desc_size, sp->s_clusters_per_group / 8, bitmap);
			if (r != OK) {
				printf("ext4: block bitmap checksum mismatch in group %u\n",
				    i);
				free(bitmap);
				return(r);
			}
		}

		if (!(flags & EXT4_BG_INODE_UNINIT)) {
			r = verify_one_group_bitmap(dev, sp,
			    groups[i].inode_bitmap, desc, 26, 58,
			    desc_size, sp->s_inodes_per_group / 8, bitmap);
			if (r != OK) {
				printf("ext4: inode bitmap checksum mismatch in group %u\n",
				    i);
				free(bitmap);
				return(r);
			}
		}
	}

	free(bitmap);
	return(OK);
}

static void
ext4_bitmap_set(unsigned char *bitmap, unsigned int bit)
{
	bitmap[bit / CHAR_BIT] |= (unsigned char)(1U << (bit % CHAR_BIT));
}

/*
 * Materialize ext4 lazy allocation bitmaps before enabling writes.
 *
 * BLOCK_UNINIT represents an all-free bitmap except for backup super/GDT
 * metadata and the physical block ranges occupied by group bitmaps and inode
 * tables (which may be relocated by flex_bg). INODE_UNINIT represents a
 * zeroed inode bitmap with reserved inode numbers and out-of-range tail bits
 * marked allocated. The descriptor free counters are checked against the
 * generated bitmap before it is accepted.
 *
 * Bitmap blocks are flushed before the descriptor flags are cleared. This
 * ordering makes a failed mount safe to retry: while a flag remains set on
 * disk, the corresponding bitmap may always be reconstructed.
 */
int
ext4_initialize_uninit_bitmaps(struct super_block *sp)
{
	struct buf *bp;
	struct group_desc *gd;
	unsigned char *bitmap;
	unsigned char *desc;
	u64_t group_first, group_first_ino, group_blocks_left;
	unsigned int group, bit, bitmap_bits, valid_bits;
	unsigned int free_count, first_ino;
	u16_t flags, new_flags;
	block64_t block;
	int changed, r;

	if (sp->s_rd_only)
		return(EROFS);
	/* The allocator is still block-granular; BIGALLOC remains read-only. */
	if (HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_BIGALLOC))
		return(EOPNOTSUPP);
	if (sp->s_block_size > UINT_MAX / CHAR_BIT ||
	    sp->s_blocks_per_group == 0 ||
	    sp->s_inodes_per_group == 0)
		return(EIO);
	bitmap_bits = sp->s_block_size * CHAR_BIT;
	if (sp->s_blocks_per_group > bitmap_bits ||
	    sp->s_inodes_per_group > bitmap_bits ||
	    (sp->s_blocks_per_group & 7U) != 0 ||
	    (sp->s_inodes_per_group & 7U) != 0)
		return(EIO);

	bitmap = malloc(sp->s_block_size);
	if (bitmap == NULL)
		return(ENOMEM);
	changed = FALSE;

	for (group = 0; group < sp->s_groups_count; group++) {
		desc = ondisk_group_descs + (size_t)group * sp->s_desc_size;
		flags = ext4_read16(desc + 18);
		if (!(flags & (EXT4_BG_BLOCK_UNINIT | EXT4_BG_INODE_UNINIT)))
			continue;
		gd = get_group_desc(group);
		if (gd == NULL) {
			r = EIO;
			goto out;
		}

		if (flags & EXT4_BG_BLOCK_UNINIT) {
			group_first = (u64_t)sp->s_first_data_block +
			    (u64_t)group * sp->s_blocks_per_group;
			if (group_first >= sp->s_blocks_count) {
				r = EIO;
				goto out;
			}
			group_blocks_left = sp->s_blocks_count - group_first;
			valid_bits = group_blocks_left > sp->s_blocks_per_group ?
			    sp->s_blocks_per_group : (unsigned int)group_blocks_left;

			memset(bitmap, 0, sp->s_block_size);
			free_count = 0;
			for (bit = 0; bit < valid_bits; bit++) {
				block = (block64_t)(group_first + bit);
				if (ext4_overlaps_super_and_descriptor_blocks(sp,
				    block, block + 1) ||
				    block == gd->block_bitmap ||
				    block == gd->inode_bitmap ||
				    (block >= gd->inode_table &&
				    block - gd->inode_table < sp->s_itb_per_group)) {
					ext4_bitmap_set(bitmap, bit);
				} else {
					free_count++;
				}
			}
			for (bit = valid_bits; bit < bitmap_bits; bit++)
				ext4_bitmap_set(bitmap, bit);

			if (free_count != gd->free_blocks_count) {
				printf("ext4: initialized block bitmap count mismatch "
				    "in group %u (%u != %u)\n",
				    group, free_count, gd->free_blocks_count);
				r = EIO;
				goto out;
			}
			r = ext4_update_bitmap_checksum(sp, group, bitmap, FALSE);
			if (r != OK)
				goto out;
			bp = get_block(sp->s_dev, gd->block_bitmap, NORMAL);
			if (bp == NULL) {
				r = EIO;
				goto out;
			}
			memcpy(b_data(bp), bitmap, sp->s_block_size);
			lmfs_markdirty(bp);
			put_block(bp);
			changed = TRUE;
		}

		if (flags & EXT4_BG_INODE_UNINIT) {
			/* Linux ext4 never marks the special first inode group lazy. */
			if (group == 0) {
				r = EIO;
				goto out;
			}
			group_first_ino = (u64_t)group *
			    sp->s_inodes_per_group + 1;
			if (group_first_ino > sp->s_inodes_count) {
				r = EIO;
				goto out;
			}
			valid_bits = sp->s_inodes_count -
			    group_first_ino + 1;
			if (valid_bits > sp->s_inodes_per_group)
				valid_bits = sp->s_inodes_per_group;

			memset(bitmap, 0, sp->s_block_size);
			free_count = 0;
			first_ino = EXT2_FIRST_INO(sp);
			for (bit = 0; bit < valid_bits; bit++) {
				if (group_first_ino + bit < first_ino)
					ext4_bitmap_set(bitmap, bit);
				else
					free_count++;
			}
			for (bit = valid_bits; bit < bitmap_bits; bit++)
				ext4_bitmap_set(bitmap, bit);

			if (free_count != gd->free_inodes_count) {
				printf("ext4: initialized inode bitmap count mismatch "
				    "in group %u (%u != %u)\n",
				    group, free_count, gd->free_inodes_count);
				r = EIO;
				goto out;
			}
			r = ext4_update_bitmap_checksum(sp, group, bitmap, TRUE);
			if (r != OK)
				goto out;
			bp = get_block(sp->s_dev, gd->inode_bitmap, NORMAL);
			if (bp == NULL) {
				r = EIO;
				goto out;
			}
			memcpy(b_data(bp), bitmap, sp->s_block_size);
			lmfs_markdirty(bp);
			put_block(bp);
			changed = TRUE;
		}
	}

	if (changed) {
		/*
		 * Persist the newly materialized bitmaps before clearing their
		 * on-disk lazy-init flags. write_super() later commits the updated
		 * descriptor flags and checksums before accepting writes.
		 */
		lmfs_flushall();
		for (group = 0; group < sp->s_groups_count; group++) {
			desc = ondisk_group_descs +
			    (size_t)group * sp->s_desc_size;
			flags = ext4_read16(desc + 18);
			new_flags = flags &
			    ~(EXT4_BG_BLOCK_UNINIT | EXT4_BG_INODE_UNINIT);
			if (new_flags != flags) {
				ext4_store16(desc + 18, new_flags);
				group_descriptors_dirty = 1;
			}
		}
	}
	r = OK;

out:
	free(bitmap);
	return(r);
}

static int compare_ext4_metadata_ranges(const void *left, const void *right)
{
	const struct ext4_metadata_range *a = left;
	const struct ext4_metadata_range *b = right;

	if (a->first < b->first)
		return(-1);
	if (a->first > b->first)
		return(1);
	if (a->end < b->end)
		return(-1);
	if (a->end > b->end)
		return(1);
	return(0);
}

/*
 * Validate the metadata block pointers decoded from every group descriptor.
 * flex_bg permits these blocks to live outside their nominal block group, so
 * only filesystem-wide bounds are imposed here. The inode table occupies one
 * contiguous run of s_itb_per_group blocks.
 */
static int
verify_group_descriptor_block_ranges(struct super_block *sp,
	struct group_desc *groups, unsigned int ngroups)
{
	block64_t block_bitmap, inode_bitmap, inode_table;
	block64_t table_blocks;
	struct ext4_metadata_range *ranges;
	block64_t previous_end;
	size_t range_count, n;
	unsigned int i, previous_group;

	/*
	 * The block bitmap has one bit per cluster (one block without BIGALLOC)
	 * and the inode bitmap has one bit per inode. Both must fit in one
	 * filesystem block. The cluster geometry was validated from the
	 * superblock before group descriptors are interpreted.
	 */
	if ((u64_t)sp->s_inodes_per_group >
	    (u64_t)sp->s_block_size * 8 ||
	    sp->s_clusters_per_group == 0 ||
	    (u64_t)sp->s_clusters_per_group >
	    (u64_t)sp->s_block_size * 8 ||
	    sp->s_cluster_ratio == 0 ||
	    (u64_t)sp->s_clusters_per_group * sp->s_cluster_ratio !=
	    sp->s_blocks_per_group ||
	    sp->s_itb_per_group == 0)
		return(EINVAL);

	if ((size_t)ngroups > SIZE_MAX / (3 * sizeof(*ranges)))
		return(EFBIG);
	ranges = malloc((size_t)ngroups * 3 * sizeof(*ranges));
	if (ranges == NULL)
		return(ENOMEM);

	range_count = 0;
	table_blocks = sp->s_itb_per_group;
	for (i = 0; i < ngroups; i++) {
		block_bitmap = groups[i].block_bitmap;
		inode_bitmap = groups[i].inode_bitmap;
		inode_table = groups[i].inode_table;

		/*
		 * Do not constrain these pointers to group i: ext4's flex_bg
		 * feature intentionally packs metadata for several groups into
		 * one flex group. They must, however, refer to valid filesystem
		 * data blocks, and the complete inode table must fit on disk.
		 */
		if (block_bitmap <= sp->s_first_data_block ||
		    block_bitmap >= sp->s_blocks_count ||
		    inode_bitmap <= sp->s_first_data_block ||
		    inode_bitmap >= sp->s_blocks_count ||
		    block_bitmap == inode_bitmap ||
		    inode_table <= sp->s_first_data_block ||
		    inode_table >= sp->s_blocks_count ||
		    table_blocks > sp->s_blocks_count - inode_table) {
			printf("ext4: invalid metadata block range in group %u\n",
			    i);
			free(ranges);
			return(EIO);
		}

		/*
		 * Descriptor blocks are reserved metadata, even when META_BG
		 * places them outside the descriptor's nominal block group.
		 */
		if (ext4_overlaps_super_and_descriptor_blocks(sp, block_bitmap,
		    block_bitmap + 1) ||
		    ext4_overlaps_super_and_descriptor_blocks(sp, inode_bitmap,
		    inode_bitmap + 1) ||
		    ext4_overlaps_super_and_descriptor_blocks(sp, inode_table,
		    inode_table + table_blocks)) {
			printf("ext4: group %u metadata overlaps reserved superblock/GDT blocks\\n",
			    i);
			free(ranges);
			return(EIO);
		}

		/* A group's own bitmaps cannot occupy its inode table blocks. */
		if ((block_bitmap >= inode_table &&
		    block_bitmap - inode_table < table_blocks) ||
		    (inode_bitmap >= inode_table &&
		    inode_bitmap - inode_table < table_blocks)) {
			printf("ext4: group %u bitmap overlaps its inode table\n", i);
			free(ranges);
			return(EIO);
		}

		ranges[range_count].first = block_bitmap;
		ranges[range_count].end = block_bitmap + 1;
		ranges[range_count++].group = i;
		ranges[range_count].first = inode_bitmap;
		ranges[range_count].end = inode_bitmap + 1;
		ranges[range_count++].group = i;
		ranges[range_count].first = inode_table;
		ranges[range_count].end = inode_table + table_blocks;
		ranges[range_count++].group = i;
	}

	/* flex_bg relocates metadata but reservations must remain disjoint. */
	qsort(ranges, range_count, sizeof(*ranges),
	    compare_ext4_metadata_ranges);
	previous_end = ranges[0].end;
	previous_group = ranges[0].group;
	for (n = 1; n < range_count; n++) {
		/* Compare with the furthest-reaching prior range, not just
		 * the immediately preceding (possibly nested) range.
		 */
		if (ranges[n].first < previous_end) {
			printf("ext4: metadata ranges overlap between groups %u and %u\n",
			    previous_group, ranges[n].group);
			free(ranges);
			return(EIO);
		}
		if (ranges[n].end > previous_end) {
			previous_end = ranges[n].end;
			previous_group = ranges[n].group;
		}
	}

	/*
	 * Keep the sorted, non-overlapping ranges for O(log groups) BIGALLOC
	 * data-cluster checks. They remain valid for the life of this mount.
	 */
	sp->s_metadata_ranges = ranges;
	sp->s_metadata_range_count = (u64_t)range_count;
	return(OK);
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
