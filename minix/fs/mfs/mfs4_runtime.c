/*
 * MFS4 v1 runtime codecs and allocator.
 * All on-disk values are decoded explicitly as little-endian bytes.
 */
#include "fs.h"
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <sys/param.h>
#include <minix/bdev.h>
#include <minix/partition.h>
#include <minix/mfs4_disk.h>
#include <minix/libminixfs.h>
#include <sys/ioc_disk.h>

#include "buf.h"
#include "inode.h"
#include "super.h"
#include "mfs4_alloc_scan.h"
#include "mfs4_journal.h"
#include "mfs4_map.h"

struct mfs4_geometry {
	uint64_t total_blocks;
	uint32_t total_inodes;
	uint64_t imap_start;
	uint32_t imap_blocks;
	uint64_t zmap_start;
	uint32_t zmap_blocks;
	uint64_t inode_table_start;
	uint64_t inode_table_blocks;
	uint64_t state_block;
	uint64_t data_start;
	uint64_t backup_block;
	uint64_t max_file_size;
	uint64_t volume_generation;
	uint32_t volume_flags;
	uint32_t ro_compat;
};

static uint64_t
mfs4_div_round_up(uint64_t n, uint64_t d)
{
	return n / d + ((n % d) != 0);
}

/*
 * MFS4 stores rdev as explicit 32-bit major/minor values. MINIX dev_t uses a
 * platform-specific bit layout, so disk values must be translated explicitly.
 */
int
mfs4_encode_rdev(dev_t native_dev, u64_t *disk_rdev)
{
	uint32_t dev_major, dev_minor;

	if (disk_rdev == NULL || (uint64_t)native_dev > UINT32_MAX)
		return EOVERFLOW;
	dev_major = (uint32_t)major(native_dev);
	dev_minor = (uint32_t)minor(native_dev);
	if (makedev(dev_major, dev_minor) != native_dev)
		return EOVERFLOW;
	*disk_rdev = ((uint64_t)dev_major << 32) | dev_minor;
	return OK;
}

int
mfs4_decode_rdev(u64_t disk_rdev, dev_t *native_dev)
{
	uint32_t dev_major, dev_minor;
	dev_t decoded;

	if (native_dev == NULL)
		return EINVAL;
	dev_major = (uint32_t)(disk_rdev >> 32);
	dev_minor = (uint32_t)disk_rdev;
	decoded = makedev(dev_major, dev_minor);
	if ((uint32_t)major(decoded) != dev_major ||
	    (uint32_t)minor(decoded) != dev_minor)
		return EIO;
	*native_dev = decoded;
	return OK;
}

static int
mfs4_decode_super(const uint8_t raw[MFS4_SUPER_SIZE],
    uint64_t partition_blocks, struct mfs4_geometry *g)
{
	uint64_t expected_imap, expected_zmap, expected_itbl;
	uint32_t i, crc;

	if (memcmp(raw + MFS4_SB_MAGIC, MFS4_SUPER_MAGIC,
	    MFS4_SUPER_MAGIC_SIZE) != 0)
		return ENOENT;

	if (mfs4_get_le16(raw + MFS4_SB_MAJOR) != MFS4_SUPER_MAJOR ||
	    mfs4_get_le16(raw + MFS4_SB_MINOR) > MFS4_SUPER_MINOR ||
	    mfs4_get_le32(raw + MFS4_SB_HEADER_SIZE) != MFS4_SUPER_SIZE ||
	    mfs4_get_le32(raw + MFS4_SB_BLOCK_SIZE) != MFS4_BLOCK_SIZE ||
	    mfs4_get_le16(raw + MFS4_SB_INODE_SIZE) != MFS4_INODE_SIZE ||
	    mfs4_get_le16(raw + MFS4_SB_DIRENT_SIZE) != MFS4_DIRENT_SIZE)
		return EINVAL;

	crc = mfs4_get_le32(raw + MFS4_SB_CRC32C);
	if (crc != mfs4_crc32c(raw, MFS4_SB_CRC32C))
		return EIO;

	if (mfs4_get_le32(raw + 52) != 0 ||
	    mfs4_get_le32(raw + 140) != 0 ||
	    mfs4_get_le32(raw + MFS4_SB_VOLUME_FLAGS) != 0)
		return EINVAL;
	for (i = 184; i < MFS4_SB_CRC32C; i++)
		if (raw[i] != 0)
			return EINVAL;

	memset(g, 0, sizeof(*g));
	g->total_blocks = mfs4_get_le64(raw + MFS4_SB_TOTAL_BLOCKS);
	g->total_inodes = mfs4_get_le32(raw + MFS4_SB_TOTAL_INODES);
	g->imap_start = mfs4_get_le64(raw + MFS4_SB_IMAP_START);
	g->imap_blocks = mfs4_get_le32(raw + MFS4_SB_IMAP_BLOCKS);
	g->zmap_start = mfs4_get_le64(raw + MFS4_SB_ZMAP_START);
	g->zmap_blocks = mfs4_get_le32(raw + MFS4_SB_ZMAP_BLOCKS);
	g->inode_table_start =
	    mfs4_get_le64(raw + MFS4_SB_INODE_TABLE_START);
	g->inode_table_blocks =
	    mfs4_get_le64(raw + MFS4_SB_INODE_TABLE_BLOCKS);
	g->state_block = mfs4_get_le64(raw + MFS4_SB_STATE_BLOCK);
	g->data_start = mfs4_get_le64(raw + MFS4_SB_DATA_START);
	g->backup_block = mfs4_get_le64(raw + MFS4_SB_BACKUP_BLOCK);
	g->max_file_size = mfs4_get_le64(raw + MFS4_SB_MAX_FILE_SIZE);
	g->volume_generation =
	    mfs4_get_le64(raw + MFS4_SB_VOLUME_GENERATION);
	g->ro_compat =
	    mfs4_get_le32(raw + MFS4_SB_RO_COMPAT_FEATURES);
	g->volume_flags =
	    mfs4_get_le32(raw + MFS4_SB_VOLUME_FLAGS);

	if (g->total_blocks != partition_blocks ||
	    g->total_blocks < MFS4_IMAP_START + 5 ||
	    g->total_blocks > MFS4_MAX_BLOCKS ||
	    g->total_inodes == 0 || g->total_inodes > MFS4_MAX_INODES ||
	    g->backup_block != g->total_blocks - 1 ||
	    g->imap_start != MFS4_IMAP_START ||
	    g->max_file_size != MFS4_MAX_FILE_SIZE ||
	    g->volume_generation == 0)
		return EINVAL;

	/* Journal fields are 64-bit start and fixed 32-bit geometry. */
	if (mfs4_get_le64(raw + MFS4_SB_JOURNAL_START) !=
	    MFS4_JOURNAL_START ||
	    mfs4_get_le32(raw + MFS4_SB_JOURNAL_BLOCKS) !=
	    MFS4_JOURNAL_BLOCKS ||
	    mfs4_get_le32(raw + MFS4_SB_MAX_TX_IMAGES) !=
	    MFS4_JOURNAL_MAX_IMAGES ||
	    mfs4_get_le32(raw + MFS4_SB_ROOT_INODE) != 1)
		return EINVAL;

	expected_imap = mfs4_div_round_up((uint64_t)g->total_inodes + 1,
	    MFS4_BITS_PER_BLOCK);
	expected_itbl = mfs4_div_round_up((uint64_t)g->total_inodes + 1,
	    MFS4_INODES_PER_BLOCK);
	if (g->imap_blocks != expected_imap ||
	    g->zmap_start != g->imap_start + g->imap_blocks ||
	    g->inode_table_start != g->zmap_start + g->zmap_blocks ||
	    g->inode_table_blocks != expected_itbl ||
	    g->state_block != g->inode_table_start + g->inode_table_blocks ||
	    g->data_start != g->state_block + 1 ||
	    g->data_start >= g->backup_block)
		return EINVAL;

	expected_zmap = mfs4_div_round_up(g->backup_block - g->data_start,
	    MFS4_BITS_PER_BLOCK);
	if (g->zmap_blocks != expected_zmap)
		return EINVAL;

	/* Unknown incompat features are fatal. Unknown RO-compatible bits are
	 * retained so mount.c can refuse a read-write mount.
	 */
	if (mfs4_get_le32(raw + MFS4_SB_INCOMPAT_FEATURES) != 0)
		return EOPNOTSUPP;

	return 0;
}

int
mfs4_probe_super(struct super_block *sp, int *is_mfs4)
{
	struct part_geom part;
	struct buf *bp;
	uint8_t primary[MFS4_SUPER_SIZE], backup[MFS4_SUPER_SIZE];
	struct mfs4_geometry pg, bg, *chosen;
	uint64_t partition_blocks;
	size_t old_block_size, probe_block_size;
	int r, primary_magic, backup_magic, primary_ok, backup_ok;

	if (sp == NULL || is_mfs4 == NULL || sp->s_dev == NO_DEV)
		return EINVAL;
	*is_mfs4 = 0;
	old_block_size = lmfs_fs_block_size();
	probe_block_size = old_block_size < MFS4_BLOCK_SIZE ?
	    MFS4_BLOCK_SIZE : old_block_size;
	if ((r = lmfs_set_blocksize_err(probe_block_size)) != OK)
		return r;

	if ((r = bdev_ioctl(sp->s_dev, DIOCGETP, &part, NONE)) != OK) {
		(void)lmfs_set_blocksize_err(old_block_size);
		return OK; /* Let the legacy MFS3 parser decide. */
	}
	partition_blocks = part.size / MFS4_BLOCK_SIZE;
	if (part.size % MFS4_BLOCK_SIZE != 0 || partition_blocks < 2) {
		(void)lmfs_set_blocksize_err(old_block_size);
		return OK;
	}

	if ((r = lmfs_get_block(&bp, sp->s_dev, 0, NORMAL)) != OK) {
		(void)lmfs_set_blocksize_err(old_block_size);
		return r;
	}
	memcpy(primary, (uint8_t *)b_data(bp) + MFS4_SUPER_PRIMARY_OFFSET,
	    sizeof(primary));
	put_block(bp);
	primary_magic = memcmp(primary + MFS4_SB_MAGIC, MFS4_SUPER_MAGIC,
	    MFS4_SUPER_MAGIC_SIZE) == 0;
	backup_magic = 0;

	if (primary_magic && NBPG > MFS4_BLOCK_SIZE) {
		(void)lmfs_set_blocksize_err(old_block_size);
		return EINVAL;
	}
	if (!primary_magic && NBPG > MFS4_BLOCK_SIZE) {
		(void)lmfs_set_blocksize_err(old_block_size);
		return OK;
	}

	if (primary_magic) {
		*is_mfs4 = 1;
		if ((r = lmfs_set_blocksize_err(MFS4_BLOCK_SIZE)) != OK)
			return r;
	}
	if ((r = lmfs_get_block(&bp, sp->s_dev, partition_blocks - 1,
	    NORMAL)) != OK) {
		if (*is_mfs4)
			return r;
		(void)lmfs_set_blocksize_err(old_block_size);
		return OK;
	}
	memcpy(backup, b_data(bp), sizeof(backup));
	put_block(bp);
	backup_magic = memcmp(backup + MFS4_SB_MAGIC, MFS4_SUPER_MAGIC,
	    MFS4_SUPER_MAGIC_SIZE) == 0;
	if (!primary_magic && !backup_magic) {
		(void)lmfs_set_blocksize_err(old_block_size);
		return OK;
	}
	*is_mfs4 = 1;
	if (NBPG > MFS4_BLOCK_SIZE)
		return EINVAL;
	if ((r = lmfs_set_blocksize_err(MFS4_BLOCK_SIZE)) != OK)
		return r;

	primary_ok = mfs4_decode_super(primary, partition_blocks, &pg) == 0;
	backup_ok = mfs4_decode_super(backup, partition_blocks, &bg) == 0;
	if (!primary_ok && !backup_ok)
		return EIO;
	if (primary_ok && backup_ok &&
	    memcmp(primary, backup, MFS4_SUPER_SIZE) != 0)
		return EINVAL;
	chosen = primary_ok ? &pg : &bg;

	memset(sp, 0, sizeof(*sp));
	sp->s_dev = fs_dev;
	sp->s_block_size = MFS4_BLOCK_SIZE;
	sp->s_inodes_per_block = MFS4_INODES_PER_BLOCK;
	sp->s_ninodes = chosen->total_inodes;
	sp->s_nzones = (zone1_t)chosen->total_blocks;
	sp->s_zones = (zone_t)(chosen->backup_block - chosen->data_start);
	sp->s_imap_blocks = (short)chosen->imap_blocks;
	sp->s_zmap_blocks = (short)chosen->zmap_blocks;
	sp->s_firstdatazone = (zone_t)chosen->data_start;
	sp->s_max_size = INT32_MAX;
	sp->s_log_zone_size = 0;
	sp->s_ndzones = MFS4_MAP_DIRECT_COUNT;
	sp->s_nindirs = MFS4_POINTERS_PER_BLOCK;
	sp->s_version = V4;
	sp->s_native = 1;
	sp->s_isearch = 1;
	sp->s_zsearch = 1;
	sp->s_mfs4_total_blocks = chosen->total_blocks;
	sp->s_mfs4_total_inodes = chosen->total_inodes;
	sp->s_mfs4_imap_start = chosen->imap_start;
	sp->s_mfs4_imap_blocks = chosen->imap_blocks;
	sp->s_mfs4_zmap_start = chosen->zmap_start;
	sp->s_mfs4_zmap_blocks = chosen->zmap_blocks;
	sp->s_mfs4_inode_table_start = chosen->inode_table_start;
	sp->s_mfs4_inode_table_blocks = chosen->inode_table_blocks;
	sp->s_mfs4_state_block = chosen->state_block;
	sp->s_mfs4_data_start = chosen->data_start;
	sp->s_mfs4_backup_block = chosen->backup_block;
	sp->s_mfs4_max_file_size = chosen->max_file_size;
	sp->s_mfs4_volume_generation = chosen->volume_generation;
	sp->s_mfs4_volume_flags = chosen->ro_compat;
	return OK;
}

int
mfs4_read_super(struct super_block *sp)
{
	int is_mfs4, r;

	r = mfs4_probe_super(sp, &is_mfs4);
	if (r != OK)
		return r;
	return is_mfs4 ? OK : EINVAL;
}

static int
mfs4_own_tx(void)
{
	return !mfs4_tx_is_active();
}

static uint64_t
mfs4_bitmap_start(struct super_block *sp, int map)
{
	return map == IMAP ? sp->s_mfs4_imap_start :
	    sp->s_mfs4_zmap_start;
}

static uint64_t
mfs4_bitmap_blocks(struct super_block *sp, int map)
{
	return map == IMAP ? sp->s_mfs4_imap_blocks :
	    sp->s_mfs4_zmap_blocks;
}

static uint64_t
mfs4_bitmap_bits(struct super_block *sp, int map)
{
	return map == IMAP ? (uint64_t)sp->s_mfs4_total_inodes + 1 :
	    sp->s_mfs4_backup_block - sp->s_mfs4_data_start;
}

static uint32_t
mfs4_bitmap_type(int map)
{
	return map == IMAP ? MFS4_JMETA_IMAP : MFS4_JMETA_ZMAP;
}

static int
mfs4_bitmap_peek(struct super_block *sp, uint64_t block, uint32_t type,
    uint8_t local[MFS4_BLOCK_SIZE], const uint8_t **out)
{
	const uint8_t *shadow;
	struct buf *bp;
	int r;
	if (mfs4_tx_is_active() && mfs4_tx_find_meta(block, &shadow) == 0) {
		*out = shadow; return OK;
	}
	if ((r = lmfs_get_block(&bp, sp->s_dev, block, NORMAL)) != OK)
		return r;
	memcpy(local, b_data(bp), MFS4_BLOCK_SIZE);
	put_block(bp);
	*out = local;
	return OK;
}

bit_t
mfs4_alloc_bit(struct super_block *sp, int map, bit_t origin)
{
	uint64_t bits, start, blocks, first_allocatable, found_bit;
	uint32_t type;
	uint8_t local[MFS4_BLOCK_SIZE];
	const uint8_t *peek;
	struct mfs4_alloc_ctx ctx;
	struct mfs4_alloc_scan_io sio;
	bit_t result = NO_BIT;
	int own, r;

	if (sp == NULL || (map != IMAP && map != ZMAP) || sp->s_rd_only)
		return NO_BIT;
	bits = mfs4_bitmap_bits(sp, map);
	start = mfs4_bitmap_start(sp, map);
	blocks = mfs4_bitmap_blocks(sp, map);
	type = mfs4_bitmap_type(map);
	first_allocatable = map == IMAP ? 1 : 0;
	if (bits <= first_allocatable ||
	    bits > (uint64_t)UINT32_MAX + 1 || blocks == 0)
		return NO_BIT;
	search_count = bits - first_allocatable;
	if (origin < first_allocatable || origin >= bits)
		origin = (bit_t)first_allocatable;
	own = mfs4_own_tx();
	if (own && (r = mfs4_tx_begin()) != OK) {
		err_code = r;
		return NO_BIT;
	}

	/*
	 * mkfs reserves inode 1 for the root directory and data bit 0 for the
	 * root directory block.  Never allocate either one if its bitmap bit
	 * has been cleared by on-disk corruption.
	 */
	if ((r = mfs4_tx_get_meta(start, type, &image)) != OK) {
		err_code = r;
		goto fail;
	}
	if ((map == IMAP && (image[0] & 0x02U) == 0) ||
	    (map == ZMAP && (image[0] & 0x01U) == 0)) {
		err_code = EIO;
		goto fail;
	}

	for (step = 0; step < search_count; step++) {
		index = first_allocatable +
		    ((uint64_t)(origin - first_allocatable) + step) %
		    search_count;
		block_index = index / MFS4_BITS_PER_BLOCK;
		if (block_index >= blocks)
			break;
		byte_index = index % MFS4_BITS_PER_BLOCK;
		if ((r = mfs4_tx_get_meta(start + block_index, type,
		    &image)) != OK) {
			err_code = r;
			goto fail;
		}
		mask = (uint8_t)(1U << (byte_index & 7));
		if ((image[byte_index >> 3] & mask) == 0) {
			image[byte_index >> 3] |= mask;
			result = (bit_t)index;
			break;
		}
	}
	if (result == NO_BIT) {
		err_code = ENOSPC;
		goto fail;
	}
	if (map == ZMAP && (r = mfs4_tx_note_block_delta(1)) != OK) {
		err_code = r;
		goto fail;
	}
	if (own && (r = mfs4_tx_commit()) != OK) {
		err_code = r;
		return NO_BIT;
	}
	return result;

fail:
	if (own)
		mfs4_tx_abort();
	return NO_BIT;
}

void
mfs4_free_bit(struct super_block *sp, int map, bit_t bit_returned)
{
	uint64_t bits, start, block_index, within;
	uint8_t *image, mask;
	int own, r;

	if (sp == NULL || (map != IMAP && map != ZMAP) || sp->s_rd_only)
		return;
	bits = mfs4_bitmap_bits(sp, map);
	if ((map == IMAP && bit_returned <= 1) ||
	    (map == ZMAP && bit_returned == 0) ||
	    bit_returned >= bits) {
		sp->s_rd_only = 1;
		return;
	}
	start = mfs4_bitmap_start(sp, map);
	block_index = bit_returned / MFS4_BITS_PER_BLOCK;
	within = bit_returned % MFS4_BITS_PER_BLOCK;
	own = mfs4_own_tx();
	if (own && (r = mfs4_tx_begin()) != OK) {
		sp->s_rd_only = 1;
		return;
	}
	if ((r = mfs4_tx_get_meta(start + block_index,
	    mfs4_bitmap_type(map), &image)) != OK) {
		if (own)
			mfs4_tx_abort();
		sp->s_rd_only = 1;
		return;
	}
	mask = (uint8_t)(1U << (within & 7));
	if ((image[within >> 3] & mask) == 0) {
		if (own)
			mfs4_tx_abort();
		sp->s_rd_only = 1;
		return;
	}
	image[within >> 3] &= (uint8_t)~mask;
	if (map == ZMAP && (r = mfs4_tx_note_block_delta(-1)) != OK) {
		if (own)
			mfs4_tx_abort();
		sp->s_rd_only = 1;
		return;
	}
	if (own && (r = mfs4_tx_commit()) != OK) {
		sp->s_rd_only = 1;
		return;
	}
}

bit_t
mfs4_count_free_bits(struct super_block *sp, int map)
{
	uint64_t bits, start, blocks, bit, b, within;
	uint8_t local[MFS4_BLOCK_SIZE], *image;
	uint64_t free_bits = 0;
	uint32_t type;
	const uint8_t *shadow;
	struct buf *bp;
	int r;

	if (sp == NULL || (map != IMAP && map != ZMAP))
		return 0;
	bits = mfs4_bitmap_bits(sp, map);
	start = mfs4_bitmap_start(sp, map);
	blocks = mfs4_bitmap_blocks(sp, map);
	type = mfs4_bitmap_type(map);
	for (b = 0; b < blocks; b++) {
		if (mfs4_tx_is_active() &&
		    mfs4_tx_find_meta(start + b, &shadow) == 0) {
			/* The active shadow image takes precedence over LMFS. */
			image = (uint8_t *)shadow;
		} else {
			if ((r = lmfs_get_block(&bp, sp->s_dev, start + b,
			    NORMAL)) != OK)
				return 0;
			memcpy(local, b_data(bp), sizeof(local));
			put_block(bp);
			image = local;
		}
		for (within = 0; within < MFS4_BITS_PER_BLOCK; within++) {
			bit = b * MFS4_BITS_PER_BLOCK + within;
			if (bit >= bits)
				break;
			if ((image[within >> 3] &
			    (uint8_t)(1U << (within & 7))) == 0)
				free_bits++;
		}
	}
	return (bit_t)free_bits;
}

static void
mfs4_inode_location(const struct super_block *sp, ino_t inum,
    uint64_t *block, uint32_t *slot)
{
	uint64_t index = (uint64_t)inum;

	/*
	 * Inode numbers are also their on-disk slot numbers. Slot zero is
	 * reserved, so inode 1 (the root inode) starts at offset one inode.
	 * Keep this mapping consistent with mkfs and fsck.
	 */
	*block = sp->s_mfs4_inode_table_start +
	    index / MFS4_INODES_PER_BLOCK;
	*slot = (uint32_t)(index % MFS4_INODES_PER_BLOCK);
}

int
mfs4_next_generation(ino_t inum, uint32_t *generation)
{
	struct super_block *sp = &superblock;
	struct buf *bp;
	uint64_t block;
	uint32_t slot, old;
	const uint8_t *shadow;
	int r;

	if (generation == NULL || inum == 0 ||
	    inum > sp->s_mfs4_total_inodes)
		return EINVAL;
	mfs4_inode_location(sp, inum, &block, &slot);
	if (mfs4_tx_is_active() &&
	    mfs4_tx_find_meta(block, &shadow) == 0) {
		old = mfs4_get_le32(shadow + slot * MFS4_INODE_SIZE +
		    MFS4_I_GENERATION);
	} else {
		if ((r = lmfs_get_block(&bp, sp->s_dev, block, NORMAL)) != OK)
			return r;
		old = mfs4_get_le32((uint8_t *)b_data(bp) +
		    slot * MFS4_INODE_SIZE + MFS4_I_GENERATION);
		put_block(bp);
	}
	*generation = old == UINT32_MAX ? 1 : old + 1;
	if (*generation == 0)
		*generation = 1;
	return OK;
}

static int
mfs4_inode_fields_valid(struct inode *rip)
{
	unsigned int i;
	uint64_t ptr;
	dev_t decoded_rdev;

	if (rip->i_size > superblock.s_mfs4_max_file_size ||
	    rip->i_atime_nsec >= 1000000000U ||
	    rip->i_mtime_nsec >= 1000000000U ||
	    rip->i_ctime_nsec >= 1000000000U ||
	    rip->i_generation == 0)
		return EINVAL;
	if ((rip->i_flags & ~(I4_ORPHAN | I4_TRUNCATING)) != 0 ||
	    ((rip->i_flags & I4_TRUNCATING) != 0 &&
	    (rip->i_flags & I4_ORPHAN) == 0) ||
	    ((rip->i_flags & I4_ORPHAN) == 0 &&
	    rip->i_orphan_next != 0))
		return EINVAL;
	if ((rip->i_nlinks == 0 && rip->i_mode != I_NOT_ALLOC &&
	    (rip->i_flags & I4_ORPHAN) == 0) ||
	    (rip->i_mode == I_NOT_ALLOC &&
	    (rip->i_flags != 0 || rip->i_orphan_next != 0)) ||
	    (rip->i_nlinks != 0 && (rip->i_flags & I4_ORPHAN) != 0 &&
	    (rip->i_flags & I4_TRUNCATING) == 0))
		return EIO;
	if (rip->i_orphan_next > superblock.s_mfs4_total_inodes ||
	    rip->i_orphan_next == 1)
		return EIO;
	for (i = 0; i < V2_NR_TZONES; i++) {
		ptr = rip->i_zone[i];
		if (ptr != 0 && (ptr < superblock.s_mfs4_data_start ||
		    ptr >= superblock.s_mfs4_backup_block))
			return EIO;
	}
	if (rip->i_xattr_head != 0 &&
	    (rip->i_xattr_head < superblock.s_mfs4_data_start ||
	    rip->i_xattr_head >= superblock.s_mfs4_backup_block))
		return EIO;
	if ((rip->i_mode & I_TYPE) == I_CHAR_SPECIAL ||
	    (rip->i_mode & I_TYPE) == I_BLOCK_SPECIAL) {
		if (mfs4_decode_rdev(rip->i_rdev, &decoded_rdev) != OK)
			return EIO;
	}
	return OK;
}

static void
mfs4_inode_encode(struct inode *rip, uint8_t raw[MFS4_INODE_SIZE])
{
	unsigned int i;

	memset(raw, 0, MFS4_INODE_SIZE);
	mfs4_put_le16(raw + MFS4_I_MODE, rip->i_mode);
	mfs4_put_le16(raw + MFS4_I_FLAGS, rip->i_flags);
	mfs4_put_le32(raw + MFS4_I_NLINK, rip->i_nlinks);
	mfs4_put_le32(raw + MFS4_I_UID, rip->i_uid);
	mfs4_put_le32(raw + MFS4_I_GID, rip->i_gid);
	mfs4_put_le64(raw + MFS4_I_SIZE, rip->i_size);
	mfs4_put_le64(raw + MFS4_I_ATIME_SEC, (uint64_t)rip->i_atime);
	mfs4_put_le64(raw + MFS4_I_MTIME_SEC, (uint64_t)rip->i_mtime);
	mfs4_put_le64(raw + MFS4_I_CTIME_SEC, (uint64_t)rip->i_ctime);
	mfs4_put_le32(raw + MFS4_I_ATIME_NSEC, rip->i_atime_nsec);
	mfs4_put_le32(raw + MFS4_I_MTIME_NSEC, rip->i_mtime_nsec);
	mfs4_put_le32(raw + MFS4_I_CTIME_NSEC, rip->i_ctime_nsec);
	mfs4_put_le32(raw + MFS4_I_GENERATION, rip->i_generation);
	for (i = 0; i < V2_NR_TZONES; i++)
		mfs4_put_le32(raw + MFS4_I_DIRECT + i * 4, rip->i_zone[i]);
	mfs4_put_le32(raw + MFS4_I_XATTR_HEAD, rip->i_xattr_head);
	mfs4_put_le32(raw + MFS4_I_ORPHAN_NEXT, rip->i_orphan_next);
	mfs4_put_le64(raw + MFS4_I_RDEV, rip->i_rdev);
	mfs4_put_le32(raw + MFS4_I_PROJECT_ID, rip->i_project_id);
}

static int
mfs4_inode_decode(struct inode *rip, const uint8_t raw[MFS4_INODE_SIZE])
{
	unsigned int i;

	if (mfs4_get_le32(raw + MFS4_I_RESERVED) != 0)
		return EIO;
	rip->i_mode = mfs4_get_le16(raw + MFS4_I_MODE);
	rip->i_flags = mfs4_get_le16(raw + MFS4_I_FLAGS);
	rip->i_nlinks = mfs4_get_le32(raw + MFS4_I_NLINK);
	rip->i_uid = mfs4_get_le32(raw + MFS4_I_UID);
	rip->i_gid = mfs4_get_le32(raw + MFS4_I_GID);
	rip->i_size = mfs4_get_le64(raw + MFS4_I_SIZE);
	rip->i_atime = (i64_t)mfs4_get_le64(raw + MFS4_I_ATIME_SEC);
	rip->i_mtime = (i64_t)mfs4_get_le64(raw + MFS4_I_MTIME_SEC);
	rip->i_ctime = (i64_t)mfs4_get_le64(raw + MFS4_I_CTIME_SEC);
	rip->i_atime_nsec = mfs4_get_le32(raw + MFS4_I_ATIME_NSEC);
	rip->i_mtime_nsec = mfs4_get_le32(raw + MFS4_I_MTIME_NSEC);
	rip->i_ctime_nsec = mfs4_get_le32(raw + MFS4_I_CTIME_NSEC);
	rip->i_generation = mfs4_get_le32(raw + MFS4_I_GENERATION);
	for (i = 0; i < V2_NR_TZONES; i++)
		rip->i_zone[i] = mfs4_get_le32(raw + MFS4_I_DIRECT + i * 4);
	rip->i_xattr_head = mfs4_get_le32(raw + MFS4_I_XATTR_HEAD);
	rip->i_orphan_next = mfs4_get_le32(raw + MFS4_I_ORPHAN_NEXT);
	rip->i_rdev = mfs4_get_le64(raw + MFS4_I_RDEV);
	rip->i_project_id = mfs4_get_le32(raw + MFS4_I_PROJECT_ID);
	return mfs4_inode_fields_valid(rip);
}

int
mfs4_rw_inode(struct inode *rip, int rw_flag)
{
	struct super_block *sp = &superblock;
	uint64_t block;
	uint32_t slot;
	uint8_t *image;
	const uint8_t *shadow;
	int own, r;

	if (rip == NULL || rip->i_num == 0 ||
	    rip->i_num > sp->s_mfs4_total_inodes)
		return EINVAL;
	mfs4_inode_location(sp, rip->i_num, &block, &slot);

	if (rw_flag == READING) {
		if (mfs4_tx_is_active() &&
		    mfs4_tx_find_meta(block, &shadow) == 0) {
			r = mfs4_inode_decode(rip,
			    shadow + slot * MFS4_INODE_SIZE);
		} else {
			struct buf *bp;
			if ((r = lmfs_get_block(&bp, rip->i_dev, block,
			    NORMAL)) != OK)
				return r;
			r = mfs4_inode_decode(rip, (uint8_t *)b_data(bp) +
			    slot * MFS4_INODE_SIZE);
			put_block(bp);
		}
		if (r == OK)
			IN_MARKCLEAN(rip);
		return r;
	}

	if (rw_flag != WRITING)
		return EINVAL;
	if ((r = mfs4_inode_fields_valid(rip)) != OK)
		return r;
	own = mfs4_own_tx();
	if (own && (r = mfs4_tx_begin()) != OK)
		return r;
	/*
	 * Register before requesting the shadow image: even a shadow-pool or I/O
	 * failure must leave enough information for abort to restore this cache
	 * object from its still-authoritative home inode.
	 */
	if ((r = mfs4_tx_note_inode(rip)) != OK) {
		if (own)
			mfs4_tx_abort();
		return r;
	}
	if ((r = mfs4_tx_get_meta(block, MFS4_JMETA_INODE, &image)) != OK) {
		if (own)
			mfs4_tx_abort();
		return r;
	}
	mfs4_inode_encode(rip, image + slot * MFS4_INODE_SIZE);
	if (own) {
		if ((r = mfs4_tx_commit()) != OK)
			return r;
		IN_MARKCLEAN(rip);
	} else {
		/* Keep the in-core object dirty until its enclosing VFS tx commits. */
	}
	return OK;
}
