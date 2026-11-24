/*
 * MFS4 orphan-list manager.
 *
 * Unlink and truncate first commit the inode's orphan state. Reclamation then
 * runs as a sequence of independent redo transactions, each touching at most
 * 32 logical blocks. The state-list head advances only in the final
 * transaction, after the inode no longer owns any blocks being reclaimed.
 */
#include "fs.h"
#include <string.h>
#include <stdint.h>

#include <minix/libminixfs.h>
#include <minix/mfs4_disk.h>

#include "buf.h"
#include "inode.h"
#include "super.h"
#include "mfs4_journal.h"
#include "mfs4_orphan_core.h"
#include "mfs4_xattr.h"

#define MFS4_ORPHAN_CHUNK_BLOCKS 32U

static int
mfs4_orphan_read_state(uint8_t state[MFS4_BLOCK_SIZE], uint32_t *head)
{
	struct buf *bp;
	int r;

	if (superblock.s_version != V4 || fs_dev == NO_DEV)
		return EINVAL;
	if ((r = lmfs_get_block(&bp, fs_dev, superblock.s_mfs4_state_block,
	    NORMAL)) != OK)
		return r;
	memcpy(state, b_data(bp), MFS4_BLOCK_SIZE);
	put_block(bp);
	return mfs4_orphan_state_validate(state,
	    superblock.s_mfs4_volume_generation,
	    superblock.s_mfs4_total_inodes, head);
}

static int
mfs4_orphan_get_state_image(uint8_t **state, uint32_t *head)
{
	int r;

	if (!mfs4_tx_is_active() || state == NULL || head == NULL)
		return EINVAL;
	if ((r = mfs4_tx_get_meta(superblock.s_mfs4_state_block,
	    MFS4_JMETA_STATE, state)) != OK)
		return r;
	return mfs4_orphan_state_validate(*state,
	    superblock.s_mfs4_volume_generation,
	    superblock.s_mfs4_total_inodes, head);
}

static int
mfs4_orphan_store_head(uint8_t *state, uint32_t head)
{
	return mfs4_orphan_state_set_head(state,
	    superblock.s_mfs4_volume_generation,
	    superblock.s_mfs4_total_inodes, head);
}

int
mfs4_orphan_add(struct inode *rip, int truncating)
{
	uint8_t *state;
	uint32_t head;
	int r;

	if (rip == NULL || rip->i_sp == NULL ||
	    rip->i_sp->s_version != V4 || !mfs4_tx_is_active())
		return EINVAL;
	if (rip->i_num <= ROOT_INODE ||
	    rip->i_num > superblock.s_mfs4_total_inodes)
		return EIO;

	if ((r = mfs4_orphan_get_state_image(&state, &head)) != OK)
		return r;

	if (rip->i_flags & I4_ORPHAN) {
		if (head == 0 || rip->i_orphan_next == rip->i_num)
			return EIO;
		if (truncating)
			rip->i_flags |= I4_TRUNCATING;
		return rw_inode(rip, WRITING);
	}

	if (rip->i_orphan_next != 0 || head == rip->i_num)
		return EIO;
	rip->i_orphan_next = head;
	rip->i_flags |= I4_ORPHAN;
	if (truncating)
		rip->i_flags |= I4_TRUNCATING;
	if ((r = mfs4_orphan_store_head(state, (uint32_t)rip->i_num)) != OK)
		return r;
	return rw_inode(rip, WRITING);
}

int
mfs4_orphan_check_empty(void)
{
	uint8_t state[MFS4_BLOCK_SIZE];
	uint32_t head;
	int r;

	if ((r = mfs4_orphan_read_state(state, &head)) != OK)
		return r;
	return head == 0 ? OK : EBUSY;
}

/*
 * Reclaim one orphan at a time. The list head remains in place while data
 * blocks are released, so a crash can repeat the search from the same inode.
 * A successful 32-block free transaction is durable before the next chunk is
 * attempted. The orphan is unlinked only after all data blocks are gone.
 */
int
mfs4_orphan_process(int mount_recovery)
{
	uint8_t state[MFS4_BLOCK_SIZE];
	uint8_t *state_image;
	uint32_t head, current_head;
	uint64_t first_block, last_block, start_block, end_block, search_end;
	ino_t search_ino = NO_ENTRY;
	uint32_t search_generation = 0;
	unsigned int walked = 0;
	int found, r;

	if (superblock.s_version != V4)
		return EINVAL;
	if (superblock.s_rd_only)
		return EROFS;

	for (;;) {
		struct inode *rip;
		uint16_t flags;
		uint32_t next;

		if ((r = mfs4_orphan_read_state(state, &head)) != OK)
			return r;
		if (head == 0)
			return OK;
		if (walked >= superblock.s_mfs4_total_inodes)
			return ELOOP;
		if (head <= ROOT_INODE ||
		    head > superblock.s_mfs4_total_inodes)
			return EIO;

		rip = get_inode(fs_dev, (ino_t)head);
		if (rip == NULL)
			return err_code != OK ? err_code : EIO;
		flags = rip->i_flags;
		next = rip->i_orphan_next;
		if (!(flags & I4_ORPHAN) || next == head ||
		    next > superblock.s_mfs4_total_inodes ||
		    next == ROOT_INODE) {
			put_inode(rip);
			return EIO;
		}
		if (flags & I4_TRUNCATING) {
			first_block = rip->i_size / MFS4_BLOCK_SIZE;
			if (rip->i_size % MFS4_BLOCK_SIZE != 0)
				first_block++;
		} else if (rip->i_nlinks == 0) {
			first_block = 0;
		} else {
			put_inode(rip);
			return EIO;
		}

		if (!mfs4_orphan_can_reclaim(flags, rip->i_nlinks,
		    (unsigned int)rip->i_count, mount_recovery)) {
			/* This inode is still open. Leave it at the list head; the
			 * last close will retry after fs_putnode().
			 */
			put_inode(rip);
			return OK;
		}

		if (search_ino != rip->i_num ||
		    search_generation != rip->i_generation) {
			search_ino = rip->i_num;
			search_generation = rip->i_generation;
			search_end = rip->i_sp->s_mfs4_max_file_size /
			    MFS4_BLOCK_SIZE;
		}
		if (search_end < first_block)
			search_end = first_block;
		if ((r = mfs4_find_last_allocated_block(rip, first_block,
		    search_end, &found, &last_block)) != OK) {
			put_inode(rip);
			return r;
		}
		if (found) {
			start_block = last_block + 1 > MFS4_ORPHAN_CHUNK_BLOCKS ?
			    last_block + 1 - MFS4_ORPHAN_CHUNK_BLOCKS : 0;
			if (start_block < first_block)
				start_block = first_block;
			end_block = last_block + 1;
			r = mfs4_free_map_range(rip, start_block, end_block);
			put_inode(rip);
			if (r != OK)
				return r;
			/* Continue below the just-committed range. */
			search_end = start_block;
			continue;
		}

		/* An unlinked-but-open file may be truncated while its fd is
		 * still live. First finish the post-EOF cleanup, then clear only
		 * TRUNCATING and keep the zero-link inode on the orphan list until
		 * its final close. On recovery or after the last close, continue
		 * with whole-inode reclamation from block zero.
		 */
		if (rip->i_nlinks == 0 && (flags & I4_TRUNCATING)) {
			int still_open = !mount_recovery && rip->i_count > 1;

			if ((r = mfs4_tx_begin()) != OK) {
				put_inode(rip);
				return r;
			}
			rip->i_flags &= (uint16_t)~I4_TRUNCATING;
			IN_MARKDIRTY(rip);
			if ((r = rw_inode(rip, WRITING)) != OK) {
				mfs4_tx_abort();
				put_inode(rip);
				return r;
			}
			if ((r = mfs4_tx_commit()) != OK) {
				put_inode(rip);
				return r;
			}
			put_inode(rip);
			search_ino = NO_ENTRY;
			search_end = 0;
			if (still_open)
				return OK;
			continue;
		}


		if ((r = mfs4_tx_begin()) != OK) {
			put_inode(rip);
			return r;
		}
		if ((r = mfs4_orphan_get_state_image(&state_image,
		    &current_head)) != OK || current_head != rip->i_num) {
			if (r == OK)
				r = EAGAIN;
			mfs4_tx_abort();
			put_inode(rip);
			return r;
		}
		if ((r = mfs4_orphan_store_head(state_image, next)) != OK) {
			mfs4_tx_abort();
			put_inode(rip);
			return r;
		}

		rip->i_flags &= (uint16_t)~(I4_ORPHAN | I4_TRUNCATING);
		rip->i_orphan_next = 0;
		if (rip->i_nlinks == 0) {
			/* Release the validated xattr chain within the final orphan
			 * transaction, before clearing and freeing the inode.
			 */
			if ((r = mfs4_xattr_free_chain(rip)) != OK) {
				mfs4_tx_abort();
				put_inode(rip);
				return r;
			}
			uint32_t generation = rip->i_generation;
			unsigned int i;

			rip->i_mode = I_NOT_ALLOC;
			rip->i_flags = 0;
			rip->i_nlinks = 0;
			rip->i_uid = 0;
			rip->i_gid = 0;
			rip->i_size = 0;
			rip->i_atime = 0;
			rip->i_mtime = 0;
			rip->i_ctime = 0;
			rip->i_atime_nsec = 0;
			rip->i_mtime_nsec = 0;
			rip->i_ctime_nsec = 0;
			rip->i_generation = generation;
			for (i = 0; i < V2_NR_TZONES; i++)
				rip->i_zone[i] = 0;
			rip->i_xattr_head = 0;
			rip->i_orphan_next = 0;
			rip->i_rdev = 0;
			rip->i_project_id = 0;
			rip->i_update = 0;
		}
		IN_MARKDIRTY(rip);
		if ((r = rw_inode(rip, WRITING)) != OK) {
			mfs4_tx_abort();
			put_inode(rip);
			return r;
		}
		if (rip->i_nlinks == 0) {
			mfs4_free_bit(&superblock, IMAP, (bit_t)rip->i_num);
			if ((bit_t)rip->i_num < superblock.s_isearch)
				superblock.s_isearch = (bit_t)rip->i_num;
			if (superblock.s_rd_only) {
				mfs4_tx_abort();
				put_inode(rip);
				return EIO;
			}
		}
		if ((r = mfs4_tx_commit()) != OK) {
			put_inode(rip);
			return r;
		}
		put_inode(rip);
		search_ino = NO_ENTRY;
		search_end = 0;
		walked++;
	}

}

int
mfs4_orphan_recover(void)
{
	return mfs4_orphan_process(1);
}
