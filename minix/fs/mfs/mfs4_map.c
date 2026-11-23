/*
 * MFS4 4 KiB block mapper with seven direct and triple-indirect roots.
 * MFS3 mapping stays in read.c/write.c and is not changed here.
 */
#include "fs.h"
#include <string.h>
#include <stdint.h>
#include <sys/param.h>
#include <minix/libminixfs.h>
#include <minix/mfs4_disk.h>

#include "buf.h"
#include "inode.h"
#include "super.h"
#include "mfs4_journal.h"
#include "mfs4_map.h"

struct mfs4_map_path {
	uint32_t root_slot;
	uint32_t depth;
	uint32_t index[3];
};

static int
mfs4_make_path(u64_t position, struct mfs4_map_path *path)
{
	u64_t block, excess, square, cube;

	if (path == NULL || position >= MFS4_MAX_FILE_SIZE)
		return EFBIG;
	block = position / MFS4_BLOCK_SIZE;
	if (block < MFS4_MAP_DIRECT_COUNT) {
		path->root_slot = (uint32_t)block;
		path->depth = 0;
		return OK;
	}

	excess = block - MFS4_MAP_DIRECT_COUNT;
	if (excess < MFS4_MAP_POINTERS) {
		path->root_slot = 7;
		path->depth = 1;
		path->index[0] = (uint32_t)excess;
		return OK;
	}
	excess -= MFS4_MAP_POINTERS;
	square = (u64_t)MFS4_MAP_POINTERS * MFS4_MAP_POINTERS;
	if (excess < square) {
		path->root_slot = 8;
		path->depth = 2;
		path->index[0] = (uint32_t)(excess / MFS4_MAP_POINTERS);
		path->index[1] = (uint32_t)(excess % MFS4_MAP_POINTERS);
		return OK;
	}

	excess -= square;
	cube = square * MFS4_MAP_POINTERS;
	if (excess >= cube)
		return EFBIG;
	path->root_slot = 9;
	path->depth = 3;
	path->index[0] = (uint32_t)(excess / square);
	path->index[1] = (uint32_t)((excess / MFS4_MAP_POINTERS) %
	    MFS4_MAP_POINTERS);
	path->index[2] = (uint32_t)(excess % MFS4_MAP_POINTERS);
	return OK;
}

static int
mfs4_pointer_valid(struct super_block *sp, uint32_t pointer)
{
	return pointer == 0 ||
	    ((uint64_t)pointer >= sp->s_mfs4_data_start &&
	    (uint64_t)pointer < sp->s_mfs4_backup_block);
}

static int
mfs4_read_indirect(uint64_t block, uint32_t index, uint32_t *value)
{
	const uint8_t *shadow;
	struct buf *bp;
	int r;

	if (index >= MFS4_MAP_POINTERS || value == NULL)
		return EINVAL;
	if (mfs4_tx_is_active() &&
	    mfs4_tx_find_meta(block, &shadow) == OK) {
		*value = mfs4_get_le32(shadow + index * sizeof(uint32_t));
		return mfs4_pointer_valid(&superblock, *value) ? OK : EIO;
	}
	if ((r = lmfs_get_block(&bp, fs_dev, block, NORMAL)) != OK)
		return r;
	*value = mfs4_get_le32((uint8_t *)b_data(bp) +
	    index * sizeof(uint32_t));
	put_block(bp);
	return mfs4_pointer_valid(&superblock, *value) ? OK : EIO;
}

block_t
mfs4_read_map(struct inode *rip, off_t position)
{
	struct mfs4_map_path path;
	uint64_t block;
	uint32_t pointer;
	uint32_t level;
	int r;

	if (rip == NULL || position < 0 ||
	    (uint64_t)position >= rip->i_sp->s_mfs4_max_file_size)
		return NO_BLOCK;
	if ((r = mfs4_make_path((u64_t)position, &path)) != OK)
		return NO_BLOCK;

	pointer = rip->i_zone[path.root_slot];
	if (!mfs4_pointer_valid(rip->i_sp, pointer))
		return NO_BLOCK;
	if (path.depth == 0)
		return (block_t)pointer;
	if (pointer == 0)
		return NO_BLOCK;

	for (level = 0; level < path.depth; level++) {
		block = pointer;
		if ((r = mfs4_read_indirect(block, path.index[level],
		    &pointer)) != OK)
			return NO_BLOCK;
		if (pointer == 0)
			return NO_BLOCK;
	}
	return (block_t)pointer;
}

static int
mfs4_image_empty(const uint8_t image[MFS4_BLOCK_SIZE])
{
	uint32_t i;

	for (i = 0; i < MFS4_BLOCK_SIZE; i++)
		if (image[i] != 0)
			return 0;
	return 1;
}

static int
mfs4_new_indirect(uint32_t near, uint32_t *block_out, uint8_t **image_out)
{
	zone_t block;
	int r;

	block = alloc_zone(fs_dev, near);
	if (block == NO_ZONE)
		return err_code != OK ? err_code : ENOSPC;
	if ((uint64_t)block < superblock.s_mfs4_data_start ||
	    (uint64_t)block >= superblock.s_mfs4_backup_block) {
		free_zone(fs_dev, block);
		return EIO;
	}
	if ((r = mfs4_tx_get_meta(block, MFS4_JMETA_INDIRECT,
	    image_out)) != OK) {
		free_zone(fs_dev, block);
		return r;
	}
	memset(*image_out, 0, MFS4_BLOCK_SIZE);
	*block_out = (uint32_t)block;
	return OK;
}

static int
mfs4_write_map_active(struct inode *rip, const struct mfs4_map_path *path,
    zone_t new_block, int op)
{
	uint32_t roots[3], indexes[3], child;
	uint8_t *images[3], *child_image;
	uint32_t level, pointer, root_slot, levels_used = 0;
	int freeing = (op & WMAP_FREE) != 0;
	int r;

	root_slot = path->root_slot;
	if (path->depth == 0) {
		pointer = rip->i_zone[root_slot];
		if (freeing) {
			if (pointer != 0)
				free_zone(fs_dev, pointer);
			rip->i_zone[root_slot] = 0;
		} else {
			if (!mfs4_pointer_valid(rip->i_sp, new_block) ||
			    new_block == 0)
				return EINVAL;
			rip->i_zone[root_slot] = new_block;
		}
		return OK;
	}

	pointer = rip->i_zone[root_slot];
	if (pointer == 0 && freeing)
		return OK;
	if (pointer == 0) {
		if ((r = mfs4_new_indirect((uint32_t)rip->i_zone[0],
		    &pointer, &child_image)) != OK)
			return r;
		rip->i_zone[root_slot] = pointer;
	}

	for (level = 0; level < path->depth; level++) {
		roots[level] = pointer;
		indexes[level] = path->index[level];
		levels_used = level + 1;
		if (pointer == 0 ||
		    !mfs4_pointer_valid(rip->i_sp, pointer))
			return EIO;
		if ((r = mfs4_tx_get_meta(pointer, MFS4_JMETA_INDIRECT,
		    &images[level])) != OK)
			return r;
		child = mfs4_get_le32(images[level] +
		    indexes[level] * sizeof(uint32_t));

		if (level + 1 == path->depth) {
			if (freeing) {
				if (child != 0)
					free_zone(fs_dev, child);
				mfs4_put_le32(images[level] +
				    indexes[level] * sizeof(uint32_t), 0);
			} else {
				if (new_block == 0 ||
				    !mfs4_pointer_valid(rip->i_sp, new_block))
					return EINVAL;
				mfs4_put_le32(images[level] +
				    indexes[level] * sizeof(uint32_t), new_block);
			}
			break;
		}

		if (child == 0) {
			if (freeing)
				break;
			if ((r = mfs4_new_indirect(pointer, &child,
			    &child_image)) != OK)
				return r;
			mfs4_put_le32(images[level] +
			    indexes[level] * sizeof(uint32_t), child);
		}
		pointer = child;
	}

	if (freeing) {
		/* Prune empty indirect blocks from the leaf back to the root. */
		for (level = levels_used; level > 0; level--) {
			uint32_t which = level - 1;
			if (!mfs4_image_empty(images[which]))
				break;
			free_zone(fs_dev, roots[which]);
			if (which == 0) {
				rip->i_zone[root_slot] = 0;
			} else {
				mfs4_put_le32(images[which - 1] +
				    indexes[which - 1] * sizeof(uint32_t), 0);
			}
		}
	}
	return OK;
}

int
mfs4_write_map(struct inode *rip, off_t position, zone_t new_block, int op)
{
	struct mfs4_map_path path;
	uint32_t old_zones[V2_NR_TZONES];
	int own, r;

	if (rip == NULL || position < 0 ||
	    (uint64_t)position >= rip->i_sp->s_mfs4_max_file_size)
		return EFBIG;
	if (rip->i_sp->s_rd_only)
		return EROFS;
	if ((r = mfs4_make_path((u64_t)position, &path)) != OK)
		return r;

	memcpy(old_zones, rip->i_zone, sizeof(old_zones));
	own = !mfs4_tx_is_active();
	if (own && (r = mfs4_tx_begin()) != OK)
		return r;

	r = mfs4_write_map_active(rip, &path, new_block, op);
	if (r == OK)
		r = mfs4_rw_inode(rip, WRITING);
	if (r != OK) {
		memcpy(rip->i_zone, old_zones, sizeof(old_zones));
		if (own)
			mfs4_tx_abort();
		return r;
	}
	if (own)
		r = mfs4_tx_commit();
	if (r != OK)
		memcpy(rip->i_zone, old_zones, sizeof(old_zones));
	return r;
}

int
mfs4_free_map_range(struct inode *rip, u64_t first_block, u64_t end_block)
{
	uint64_t block, max_blocks;
	uint32_t old_zones[V2_NR_TZONES];
	int own, r = OK;

	if (rip == NULL || end_block < first_block)
		return EINVAL;
	max_blocks = rip->i_sp->s_mfs4_max_file_size / MFS4_BLOCK_SIZE;
	if (end_block > max_blocks)
		return EFBIG;

	memcpy(old_zones, rip->i_zone, sizeof(old_zones));
	own = !mfs4_tx_is_active();
	if (own && (r = mfs4_tx_begin()) != OK)
		return r;
	for (block = first_block; block < end_block; block++) {
		u64_t pos = block * MFS4_BLOCK_SIZE;
		struct mfs4_map_path path;

		r = mfs4_make_path(pos, &path);
		if (r == OK)
			r = mfs4_write_map_active(rip, &path, 0, WMAP_FREE);
		if (r != OK)
			break;
	}
	if (r == OK)
		r = mfs4_rw_inode(rip, WRITING);
	if (r != OK) {
		memcpy(rip->i_zone, old_zones, sizeof(old_zones));
		if (own)
			mfs4_tx_abort();
		return r;
	}
	if (own)
		r = mfs4_tx_commit();
	if (r != OK)
		memcpy(rip->i_zone, old_zones, sizeof(old_zones));
	return r;
}

int
mfs4_zero_eof_tail(struct inode *rip, u64_t old_eof, u64_t new_eof)
{
	struct buf *bp;
	block_t block;
	u64_t block_start;
	size_t offset, length;
	int r;

	if (rip == NULL || new_eof <= old_eof ||
	    old_eof >= rip->i_sp->s_mfs4_max_file_size)
		return EINVAL;
	offset = (size_t)(old_eof % MFS4_BLOCK_SIZE);
	if (offset == 0)
		return OK;
	block_start = old_eof - offset;
	block = mfs4_read_map(rip, (off_t)block_start);
	if (block == NO_BLOCK)
		return OK;
	length = MFS4_BLOCK_SIZE - offset;
	if (new_eof - old_eof < length)
		length = (size_t)(new_eof - old_eof);
	if ((r = lmfs_get_block_ino(&bp, rip->i_dev, block, NORMAL,
	    rip->i_num, block_start)) != OK)
		return r;
	memset((uint8_t *)b_data(bp) + offset, 0, length);
	MARKDIRTY(bp);
	put_block(bp);
	return OK;
}
