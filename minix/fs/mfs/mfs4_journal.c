/*
 * MINIX block-cache adapter for the portable MFS4 redo journal.
 *
 * VFS mutation callbacks frame each operation in one transaction. Inode,
 * allocation-map, indirect-map, directory, and state metadata are staged as
 * shadow images. File data remains in LMFS and is forced stable before the
 * journal commit point. After replay, writable mount resumes orphan-list
 * cleanup before exposing the volume to VFS.
 */
#include "fs.h"
#include <string.h>
#include <sys/ioc_disk.h>
#include <minix/bdev.h>
#include <minix/libminixfs.h>

#include "buf.h"
#include "inode.h"
#include "super.h"
#include "mfs4_journal.h"

static struct mfs4_journal_io journal_io;
static struct mfs4_journal_tx journal_tx;
static int journal_bound;
static int journal_open_readonly;
static uint64_t journal_sequence = 1;
static int64_t journal_block_delta;
struct mfs4_journal_inode_ref {
	struct inode *inode;
	dev_t dev;
	ino_t number;
	uint32_t generation;
};
static struct mfs4_journal_inode_ref journal_inode_refs[NR_INODES];
static unsigned int journal_inode_count;

static int
mfs4_jio_read(void *cookie, uint64_t block, void *data)
{
	struct super_block *sp = cookie;
	struct buf *bp;
	int r;

	if (sp->s_dev == NO_DEV || block >= sp->s_mfs4_total_blocks)
		return EINVAL;
	if ((r = lmfs_get_block(&bp, sp->s_dev, block, NORMAL)) != OK)
		return r;
	memcpy(data, b_data(bp), MFS4_BLOCK_SIZE);
	put_block(bp);
	return OK;
}

static int
mfs4_jio_write(void *cookie, uint64_t block, const void *data)
{
	struct super_block *sp = cookie;
	struct buf *bp;
	int r;

	if (sp->s_dev == NO_DEV || sp->s_rd_only ||
	    block >= sp->s_mfs4_total_blocks)
		return EROFS;
	if ((r = lmfs_get_block(&bp, sp->s_dev, block, NO_READ)) != OK)
		return r;
	memcpy(b_data(bp), data, MFS4_BLOCK_SIZE);
	lmfs_markdirty(bp);
	put_block(bp);
	return OK;
}

static int
mfs4_jio_flush(void *cookie)
{
	struct super_block *sp = cookie;
	int r;

	if (sp->s_dev == NO_DEV)
		return EINVAL;

	/* First drain LMFS buffers and propagate asynchronous write errors. */
	if ((r = lmfs_flushdev_err(sp->s_dev)) != OK)
		return r;

	/* Then flush the storage driver's volatile write cache. */
	return bdev_ioctl(sp->s_dev, DIOCFLUSH, NULL, NONE);
}

static void
mfs4_jio_fail_closed(void *cookie)
{
	struct super_block *sp = cookie;

	/* A post-commit-point failure leaves the commit record for replay. */
	sp->s_rd_only = 1;
}

static void
mfs4_journal_bind(struct super_block *sp)
{
	memset(&journal_io, 0, sizeof(journal_io));
	journal_io.cookie = sp;
	journal_io.geometry.total_blocks = sp->s_mfs4_total_blocks;
	journal_io.geometry.imap_start = sp->s_mfs4_imap_start;
	journal_io.geometry.imap_blocks = sp->s_mfs4_imap_blocks;
	journal_io.geometry.zmap_start = sp->s_mfs4_zmap_start;
	journal_io.geometry.zmap_blocks = sp->s_mfs4_zmap_blocks;
	journal_io.geometry.inode_table_start =
	    sp->s_mfs4_inode_table_start;
	journal_io.geometry.inode_table_blocks =
	    sp->s_mfs4_inode_table_blocks;
	journal_io.geometry.state_block = sp->s_mfs4_state_block;
	journal_io.geometry.data_start = sp->s_mfs4_data_start;
	journal_io.geometry.backup_block = sp->s_mfs4_backup_block;
	journal_io.geometry.volume_generation =
	    sp->s_mfs4_volume_generation;
	journal_io.read_block = mfs4_jio_read;
	journal_io.write_block = mfs4_jio_write;
	journal_io.flush = mfs4_jio_flush;
	journal_io.fail_closed = mfs4_jio_fail_closed;
	journal_bound = 1;
}

static int
mfs4_journal_validate_state(struct super_block *sp, uint32_t *flags_out,
    uint32_t *orphan_out)
{
	uint8_t state[MFS4_BLOCK_SIZE];
	uint32_t i, flags;
	int r;

	if ((r = mfs4_jio_read(sp, sp->s_mfs4_state_block, state)) != OK)
		return r;
	if (memcmp(state, MFS4_STATE_MAGIC, 4) != 0 ||
	    mfs4_get_le32(state + 4) != MFS4_STATE_VERSION ||
	    mfs4_get_le64(state + MFS4_STATE_GENERATION) !=
	    sp->s_mfs4_volume_generation ||
	    mfs4_get_le32(state + MFS4_STATE_CRC32C) !=
	    mfs4_crc32c(state, MFS4_STATE_CRC32C))
		return EIO;
	for (i = 24; i < MFS4_STATE_CRC32C; i++) {
		if (state[i] != 0)
			return EIO;
	}
	flags = mfs4_get_le32(state + MFS4_STATE_FLAGS);
	if ((flags & ~MFS4_STATE_CLEAN) != 0)
		return EIO;
	if (flags_out != NULL)
		*flags_out = flags;
	if (orphan_out != NULL)
		*orphan_out = mfs4_get_le32(state + MFS4_STATE_ORPHAN_HEAD);
	return OK;
}


/* Update the filesystem clean marker through the same redo protocol. */
static int
mfs4_journal_set_clean(int clean)
{
	uint8_t *state;
	uint32_t flags;
	int r;

	if ((r = mfs4_tx_begin()) != OK)
		return r;
	if ((r = mfs4_tx_get_meta(superblock.s_mfs4_state_block,
	    MFS4_JMETA_STATE, &state)) != OK) {
		mfs4_tx_abort();
		return r;
	}
	flags = mfs4_get_le32(state + MFS4_STATE_FLAGS);
	if (clean)
		flags |= MFS4_STATE_CLEAN;
	else
		flags &= ~MFS4_STATE_CLEAN;
	mfs4_put_le32(state + MFS4_STATE_FLAGS, flags);
	mfs4_put_le32(state + MFS4_STATE_CRC32C,
	    mfs4_crc32c(state, MFS4_STATE_CRC32C));
	return mfs4_tx_commit();
}

/*
 * MFS4 mount policy:
 * - RO mount is allowed only when state is clean, orphan list empty, and the
 *   journal has no committed transaction.
 * - RW mount replays the journal, validates state, recovers orphans, initializes
 *   block-usage accounting, and durably clears CLEAN before exposing the
 *   volume to VFS.
 */
int
mfs4_journal_mount(struct super_block *sp, int readonly)
{
	int pending, r;
	uint32_t state_flags, orphan_head;

	if (sp == NULL || sp->s_version != V4)
		return EINVAL;

	journal_open_readonly = readonly != 0;
	mfs4_journal_bind(sp);
	pending = mfs4_journal_pending(&journal_io);
	if (pending < 0)
		return -pending;

	if (readonly) {
		/* A read-only server has no committed-image shadow overlay. */
		if (pending)
			return EROFS;
		if ((r = mfs4_journal_validate_state(sp, &state_flags,
		    &orphan_head)) != OK)
			return r;
		if (!(state_flags & MFS4_STATE_CLEAN) || orphan_head != 0)
			return EROFS;
		return OK;
	}

	/*
	 * s_mfs4_volume_flags carries the read-only-compatible feature mask.
	 * Unknown bits permit a read-only mount, but must be rejected before
	 * any RW-side flush, journal replay, or other on-disk mutation.
	 */
	if (sp->s_mfs4_volume_flags != 0)
		return EOPNOTSUPP;

	/* Fail closed when the device cannot promise durable journal barriers. */
	if ((r = mfs4_jio_flush(sp)) != OK)
		return r;

	/* Replay committed images before reading journaled state metadata. */
	if ((r = mfs4_journal_replay(&journal_io)) != OK)
		return r;
	if ((r = mfs4_journal_validate_state(sp, &state_flags,
	    &orphan_head)) != OK)
		return r;

	/*
	 * Orphan recovery may free blocks before fs_mount() reaches its usual
	 * accounting setup. Initialize the baseline after journal replay so
	 * mfs4_tx_commit() can safely apply each chunk's block-count delta.
	 */
	used_zones = sp->s_zones - mfs4_count_free_bits(sp, ZMAP);
	lmfs_set_blockusage(sp->s_zones, used_zones);

	/* Recovery is idempotent: the orphan head remains published until each
	 * inode's final cleanup transaction commits.
	 */
	if (orphan_head != 0) {
		if ((r = mfs4_orphan_recover()) != OK)
			return r;
		if ((r = mfs4_journal_validate_state(sp, &state_flags,
		    &orphan_head)) != OK)
			return r;
		if (orphan_head != 0)
			return EIO;
	}

	/* A writable mount is not complete until CLEAN=0 is durable. */
	return mfs4_journal_set_clean(0);
}

int
mfs4_tx_is_active(void)
{
	return journal_tx.active;
}

int
mfs4_tx_note_block_delta(int delta)
{
	if (!journal_tx.active)
		return EINVAL;
	if ((delta > 0 && journal_block_delta > INT64_MAX - delta) ||
	    (delta < 0 && journal_block_delta < INT64_MIN - delta))
		return EOVERFLOW;
	journal_block_delta += delta;
	return OK;
}

int
mfs4_tx_begin(void)
{
	int r;

	if (!journal_bound)
		return EINVAL;
	if (superblock.s_rd_only)
		return EROFS;
	if (journal_tx.active)
		return EBUSY;

	journal_block_delta = 0;
	journal_inode_count = 0;
	r = mfs4_journal_tx_begin(&journal_tx, &journal_io,
	    journal_sequence++);
	return r;
}

int
mfs4_tx_note_inode(struct inode *rip)
{
	unsigned int i;

	if (!journal_tx.active || rip == NULL)
		return EINVAL;
	for (i = 0; i < journal_inode_count; i++) {
		if (journal_inode_refs[i].inode == rip &&
		    journal_inode_refs[i].dev == rip->i_dev &&
		    journal_inode_refs[i].number == rip->i_num &&
		    journal_inode_refs[i].generation == rip->i_generation)
			return OK;
	}
	if (journal_inode_count >= NR_INODES)
		return ENOSPC;
	journal_inode_refs[journal_inode_count].inode = rip;
	journal_inode_refs[journal_inode_count].dev = rip->i_dev;
	journal_inode_refs[journal_inode_count].number = rip->i_num;
	journal_inode_refs[journal_inode_count].generation =
	    rip->i_generation;
	journal_inode_count++;
	return OK;
}

int
mfs4_tx_get_meta(uint64_t home_block, uint32_t metadata_type,
    uint8_t **image_out)
{
	if (!journal_bound)
		return EINVAL;
	return mfs4_journal_tx_get_meta(&journal_tx, home_block,
	    metadata_type, image_out);
}

int
mfs4_tx_find_meta(uint64_t home_block, const uint8_t **image_out)
{
	if (!journal_bound)
		return ENOENT;
	return mfs4_journal_tx_find_meta(&journal_tx, home_block, image_out);
}

/*
 * Explicit transaction aborts have not written shadow metadata to home
 * blocks. Reload every inode touched by the transaction from its home inode
 * table slot so a failed callback cannot leave uncommitted fields visible in
 * the inode cache. A freshly allocated inode may decode as the all-zero free
 * inode (mode/generation both zero); that is the expected pre-transaction
 * state, not a disk error.
 */
static int
mfs4_restore_inode_refs(void)
{
	unsigned int i;
	int r, result = OK;

	for (i = 0; i < journal_inode_count; i++) {
		struct mfs4_journal_inode_ref *ref = &journal_inode_refs[i];
		struct inode *rip = ref->inode;

		if (rip == NULL || rip->i_dev != ref->dev ||
		    rip->i_num != ref->number ||
		    rip->i_generation != ref->generation)
			continue;

		r = mfs4_rw_inode(rip, READING);
		if (r != OK &&
		    !(r == EINVAL && rip->i_mode == I_NOT_ALLOC &&
		    rip->i_generation == 0)) {
			if (result == OK)
				result = r;
			continue;
		}

		/* i_update is in-core-only; pending atime may be lost on abort. */
		rip->i_update = 0;
		rip->i_zsearch = NO_ZONE;
		IN_MARKCLEAN(rip);
	}
	journal_inode_count = 0;
	return result;
}

int
mfs4_tx_commit(void)
{
	int r;

	if (!journal_tx.active)
		return EINVAL;
	r = mfs4_journal_tx_commit(&journal_tx);
	if (r != OK) {
		/*
		 * The journal core sets s_rd_only through fail_closed only when the
		 * commit point may have been reached. Before that point, home inode
		 * blocks are still authoritative and the cache can be restored.
		 */
		if (!superblock.s_rd_only) {
			if (mfs4_restore_inode_refs() != OK)
				superblock.s_rd_only = 1;
		} else {
			journal_inode_count = 0;
		}
		journal_block_delta = 0;
		/* A commit I/O error still fails the mounted volume closed. */
		superblock.s_rd_only = 1;
		return r;
	}
	/* The cached inode now matches the durable home image. */
	{
		unsigned int i;
		for (i = 0; i < journal_inode_count; i++) {
			struct mfs4_journal_inode_ref *ref =
			    &journal_inode_refs[i];
			if (ref->inode->i_dev == ref->dev &&
			    ref->inode->i_num == ref->number &&
			    ref->inode->i_generation == ref->generation)
				IN_MARKCLEAN(ref->inode);
		}
		journal_inode_count = 0;
	}
	if (journal_block_delta != 0) {
		if (journal_block_delta > 0)
			used_zones += (zone_t)journal_block_delta;
		else if (used_zones >= (zone_t)-journal_block_delta)
			used_zones -= (zone_t)-journal_block_delta;
		else {
			superblock.s_rd_only = 1;
			journal_block_delta = 0;
			return EIO;
		}
		lmfs_change_blockusage((int)journal_block_delta);
	}
	journal_block_delta = 0;
	return OK;
}

void
mfs4_tx_abort(void)
{
	int r;

	mfs4_journal_tx_abort(&journal_tx);
	journal_block_delta = 0;
	r = mfs4_restore_inode_refs();
	if (r != OK)
		superblock.s_rd_only = 1;
}

int
mfs4_journal_sync(void)
{
	if (superblock.s_version != V4)
		return EINVAL;
	if (journal_tx.active)
		return EBUSY;
	/* A true RO mount cannot have dirtied data; fail-closed RW mounts can. */
	if (superblock.s_rd_only && journal_open_readonly)
		return OK;
	return mfs4_jio_flush(&superblock);
}

int
mfs4_journal_unmount(void)
{
	int r;

	if (superblock.s_version != V4 || superblock.s_rd_only)
		return EINVAL;
	if ((r = mfs4_orphan_check_empty()) != OK)
		return r;
	if ((r = mfs4_journal_sync()) != OK)
		return r;
	return mfs4_journal_set_clean(1);
}

int
mfs4_journal_mark_dirty(void)
{
	if (superblock.s_version != V4 || superblock.s_rd_only)
		return EINVAL;
	return mfs4_journal_set_clean(0);
}
