/*
 * Bounded ext4 extent-tree updates.
 *
 * Inline roots and existing external leaves are edited in place.  Allocating
 * or splitting nodes, rebalancing the tree, and changing ancestor index keys
 * remain unsupported; those operations are rejected rather than treating
 * extents as ext2 indirect blocks.
 */

#include "fs.h"
#include <string.h>
#include "buf.h"
#include "inode.h"
#include "super.h"

struct ext4_extent {
	u32_t logical;
	u16_t encoded_len;
	u64_t physical;
	u32_t length;
	int unwritten;
};

static u16_t
extent_get16(const unsigned char *p)
{
	u16_t v;

	memcpy(&v, p, sizeof(v));
	return((u16_t)conv2(le_CPU, v));
}

static u32_t
extent_get32(const unsigned char *p)
{
	u32_t v;

	memcpy(&v, p, sizeof(v));
	return((u32_t)conv4(le_CPU, v));
}

static void
extent_put16(unsigned char *p, u16_t v)
{
	v = (u16_t)conv2(le_CPU, v);
	memcpy(p, &v, sizeof(v));
}

static void
extent_put32(unsigned char *p, u32_t v)
{
	v = (u32_t)conv4(le_CPU, v);
	memcpy(p, &v, sizeof(v));
}

static void
extent_get(const unsigned char *p, struct ext4_extent *ex)
{
	ex->logical = extent_get32(p);
	ex->encoded_len = extent_get16(p + 4);
	ex->unwritten = ex->encoded_len > 0x8000;
	ex->length = ex->unwritten ?
	    ex->encoded_len - 0x8000 : ex->encoded_len;
	ex->physical = ((u64_t)extent_get16(p + 6) << 32) |
	    extent_get32(p + 8);
}

static void
extent_put(unsigned char *p, u32_t logical, u64_t physical,
	u32_t length, int unwritten)
{
	u16_t encoded_len;

	encoded_len = (u16_t)(unwritten ? 0x8000 + length : length);
	extent_put32(p, logical);
	extent_put16(p + 4, encoded_len);
	extent_put16(p + 6, (u16_t)(physical >> 32));
	extent_put32(p + 8, (u32_t)physical);
}


/*
 * Locate the extent leaf which owns logical.  Existing external nodes may be
 * edited in place, but this routine does not allocate, split, or rebalance
 * nodes.  A key before the first index entry would require changing the tree
 * root, so callers get EOPNOTSUPP in that case.
 */
static int
extent_leaf(struct inode *rip, u32_t logical, unsigned char **nodep,
	size_t *node_sizep, u16_t *entriesp, u16_t *maxp,
	struct buf **bpp)
{
	unsigned char *node;
	struct buf *bp;
	size_t node_size;
	int expected_depth;
	u16_t magic, entries, max_entries, depth;
	u64_t previous_end;
	unsigned int i;

	node = (unsigned char *)rip->i_block;
	node_size = sizeof(rip->i_block);
	bp = NULL;
	expected_depth = -1;

	for (;;) {
		if (node_size < 12)
			goto corrupt;
		magic = extent_get16(node);
		entries = extent_get16(node + 2);
		max_entries = extent_get16(node + 4);
		depth = extent_get16(node + 6);

		if (magic != EXT4_EXTENT_MAGIC ||
		    depth > EXT4_EXTENT_MAX_DEPTH ||
		    (expected_depth >= 0 && depth != expected_depth) ||
		    max_entries == 0 ||
		    max_entries > (node_size - 12) / 12 ||
		    entries > max_entries)
			goto corrupt;

		/* The inode-resident root has only 60 bytes for its entries. */
		if (bp == NULL &&
		    max_entries > (sizeof(rip->i_block) - 12) / 12)
			goto corrupt;

		if (depth == 0) {
			if (bp != NULL && entries == 0)
				goto corrupt;
			previous_end = 0;
			for (i = 0; i < entries; i++) {
				struct ext4_extent ex;

				extent_get(node + 12 + i * 12, &ex);
				if (ex.length == 0 || ex.physical == 0 ||
				    ex.physical >= rip->i_sp->s_blocks_count ||
				    ex.length >
				    rip->i_sp->s_blocks_count - ex.physical ||
				    (u64_t)ex.logical + ex.length >
				    ((u64_t)UINT32_MAX + 1) ||
				    (i != 0 && ex.logical < previous_end))
					goto corrupt;
				previous_end = (u64_t)ex.logical + ex.length;
			}
			*nodep = node;
			*node_sizep = node_size;
			*entriesp = entries;
			*maxp = max_entries;
			*bpp = bp;
			return(OK);
		}

		if (entries == 0)
			goto corrupt;

		/*
		 * Extent index keys must be strictly increasing.  Validate every
		 * child address before following the selected one.
		 */
		{
			block64_t child, selected_child;
			u32_t previous_key, key;
			int found;

			selected_child = 0;
			previous_key = 0;
			found = FALSE;
			for (i = 0; i < entries; i++) {
				const unsigned char *entry;
				u32_t child_lo;
				u16_t child_hi;

				entry = node + 12 + i * 12;
				key = extent_get32(entry);
				child_lo = extent_get32(entry + 4);
				child_hi = extent_get16(entry + 8);
				child = ((u64_t)child_hi << 32) | child_lo;

				if (child == 0 ||
				    child >= rip->i_sp->s_blocks_count ||
				    (i != 0 && key <= previous_key))
					goto corrupt;
				previous_key = key;

				if (key > logical)
					break;
				selected_child = child;
				found = TRUE;
			}
			if (!found) {
				/* Inserting before this key needs tree rebalancing. */
				if (bp != NULL)
					put_block(bp);
				return(EOPNOTSUPP);
			}

			if (bp != NULL) {
				put_block(bp);
				bp = NULL;
			}
			bp = get_block(rip->i_dev, selected_child, NORMAL);
			if (bp == NULL)
				goto corrupt;
			node = (unsigned char *)b_data(bp);
			node_size = rip->i_sp->s_block_size;
			expected_depth = depth - 1;
		}
	}

corrupt:
	if (bp != NULL)
		put_block(bp);
	return(EIO);
}

static void
extent_release_leaf(struct buf *bp, int dirty)
{
	if (bp != NULL) {
		if (dirty)
			lmfs_markdirty(bp);
		put_block(bp);
	}
}

/*
 * Keep the write-side allocation check in sync with
 * ext4_encode_inode_blocks() in inode.c.  i_blocks is maintained in 512-byte
 * sectors in memory.  Without HUGE_FILE the on-disk field is 32 bits; with
 * HUGE_FILE it is 48 bits, and values above that can be represented as
 * filesystem blocks when aligned.
 */
static int
extent_i_blocks_can_add(struct inode *rip, u64_t sectors)
{
	u64_t new_blocks, sectors_per_block;

	if (sectors > ~(u64_t)0 - rip->i_blocks)
		return(FALSE);
	new_blocks = rip->i_blocks + sectors;

	if (!HAS_RO_COMPAT_FEATURE(rip->i_sp, RO_COMPAT_HUGE_FILE))
		return(new_blocks <= 0xffffffffULL);

	if (new_blocks <= 0xffffffffffffULL)
		return(TRUE);

	sectors_per_block = rip->i_sp->s_sectors_in_block;
	if (sectors_per_block == 0 ||
	    new_blocks % sectors_per_block != 0)
		return(FALSE);

	return(new_blocks / sectors_per_block <= 0xffffffffffffULL);
}

int
ext4_extent_write_map(struct inode *rip, off_t position,
	block64_t new_wblock, int op)
{
	unsigned char *root, *entry;
	struct buf *leaf_bp;
	struct ext4_extent ex, previous, next;
	size_t node_size;
	u16_t entries, max_entries;
	u64_t logical64, physical;
	u32_t logical, offset, new_length;
	unsigned int i, insert;
	int r;

	if (rip->i_sp->s_rd_only)
		return(EROFS);
	if (position < 0)
		return(EINVAL);

	logical64 = (u64_t)position / rip->i_sp->s_block_size;
	if (logical64 > UINT32_MAX)
		return(EFBIG);
	logical = (u32_t)logical64;

	if ((r = extent_leaf(rip, logical, &root, &node_size, &entries,
	    &max_entries, &leaf_bp)) != OK)
		return(r);
	(void)node_size;	/* The selected node has already been bounded. */

	/* Find the extent containing the requested logical block. */
	for (i = 0; i < entries; i++) {
		entry = root + 12 + i * 12;
		extent_get(entry, &ex);
		if ((u64_t)logical >= ex.logical &&
		    (u64_t)logical < (u64_t)ex.logical + ex.length)
			break;
	}

	if (op & WMAP_FREE) {
		u64_t block_to_free;

		if (i == entries) {
			extent_release_leaf(leaf_bp, FALSE);
			return(OK);	/* A hole needs no work. */
		}

		offset = logical - ex.logical;
		block_to_free = ex.physical + offset;
		if (rip->i_blocks < rip->i_sp->s_sectors_in_block) {
			extent_release_leaf(leaf_bp, FALSE);
			return(EIO);
		}
		/*
		 * Changing or removing the first extent in an external leaf
		 * also changes its parent's index key.  Do not leave a stale key.
		 */
		if (leaf_bp != NULL && i == 0 && offset == 0) {
			extent_release_leaf(leaf_bp, FALSE);
			return(EOPNOTSUPP);
		}
		/* Splitting a middle extent consumes one additional leaf slot. */
		if (offset != 0 && offset != ex.length - 1 &&
		    entries >= max_entries) {
			extent_release_leaf(leaf_bp, FALSE);
			return(EFBIG);
		}

		if (ex.length == 1) {
			memmove(entry, entry + 12,
			    (entries - i - 1) * 12);
			entries--;
		} else if (offset == 0) {
			extent_put(entry, ex.logical + 1, ex.physical + 1,
			    ex.length - 1, ex.unwritten);
		} else if (offset == ex.length - 1) {
			extent_put(entry, ex.logical, ex.physical,
			    ex.length - 1, ex.unwritten);
		} else {
			/* Removing a middle block splits one extent into two. */
			memmove(entry + 24, entry + 12,
			    (entries - i - 1) * 12);
			extent_put(entry, ex.logical, ex.physical, offset,
			    ex.unwritten);
			extent_put(entry + 12, logical + 1,
			    block_to_free + 1, ex.length - offset - 1,
			    ex.unwritten);
			entries++;
		}

		extent_put16(root + 2, entries);
		free_block(rip->i_sp, block_to_free);
		rip->i_blocks -= rip->i_sp->s_sectors_in_block;
		extent_release_leaf(leaf_bp, TRUE);
		rip->i_dirt = IN_DIRTY;
		return(OK);
	}

	/*
	 * An unwritten extent already owns its physical blocks.  Convert just
	 * the block being written to an initialized extent and return the
	 * speculative allocation made by new_block().
	 */
	if (i != entries) {
		u64_t old_physical;

		if (!ex.unwritten) {
			extent_release_leaf(leaf_bp, FALSE);
			return(EEXIST);
		}
		offset = logical - ex.logical;
		old_physical = ex.physical + offset;
		if (offset == 0 && ex.length == 1) {
			extent_put(entry, logical, old_physical, 1, FALSE);
		} else if (offset == 0) {
			if (entries >= max_entries) {
				extent_release_leaf(leaf_bp, FALSE);
				return(EFBIG);
			}
			memmove(entry + 12, entry,
			    (entries - i) * 12);
			extent_put(entry, logical, old_physical, 1, FALSE);
			extent_put(entry + 12, logical + 1, old_physical + 1,
			    ex.length - 1, TRUE);
			entries++;
		} else if (offset == ex.length - 1) {
			if (entries >= max_entries) {
				extent_release_leaf(leaf_bp, FALSE);
				return(EFBIG);
			}
			extent_put(entry, ex.logical, ex.physical, ex.length - 1,
			    TRUE);
			memmove(entry + 24, entry + 12,
			    (entries - i - 1) * 12);
			extent_put(entry + 12, logical, old_physical, 1, FALSE);
			entries++;
		} else {
			if (entries > max_entries - 2) {
				extent_release_leaf(leaf_bp, FALSE);
				return(EFBIG);
			}
			memmove(entry + 36, entry + 12,
			    (entries - i - 1) * 12);
			extent_put(entry, ex.logical, ex.physical, offset, TRUE);
			extent_put(entry + 12, logical, old_physical, 1, FALSE);
			extent_put(entry + 24, logical + 1, old_physical + 1,
			    ex.length - offset - 1, TRUE);
			entries += 2;
		}
		extent_put16(root + 2, entries);
		free_block(rip->i_sp, new_wblock);
		extent_release_leaf(leaf_bp, TRUE);
		rip->i_dirt = IN_DIRTY;
		return(OK);
	}

	physical = (u64_t)new_wblock;
	if (physical == 0 || physical >= rip->i_sp->s_blocks_count ||
	    physical > (((u64_t)1 << 48) - 1)) {
		extent_release_leaf(leaf_bp, FALSE);
		return(EIO);
	}
	/*
	 * The in-core counter is 64 bits.  Do not retain the old ext2
	 * 32-bit limit here: HUGE_FILE filesystems can store a 48-bit
	 * sector count (or a 48-bit filesystem-block count).
	 */
	if (!extent_i_blocks_can_add(rip,
	    rip->i_sp->s_sectors_in_block)) {
		extent_release_leaf(leaf_bp, FALSE);
		return(EFBIG);
	}

	/* Locate the sorted insertion point. */
	for (insert = 0; insert < entries; insert++) {
		extent_get(root + 12 + insert * 12, &next);
		if (next.logical > logical)
			break;
	}

	/*
	 * An external leaf's first logical block is also its parent's index
	 * key.  Inserting before it would require updating every ancestor.
	 */
	if (leaf_bp != NULL && insert == 0 &&
	    entries != 0) {
		extent_get(root + 12, &next);
		if (logical < next.logical) {
			extent_release_leaf(leaf_bp, FALSE);
			return(EOPNOTSUPP);
		}
	}

	/* Extend the preceding initialized extent when both sides are adjacent. */
	if (insert > 0) {
		entry = root + 12 + (insert - 1) * 12;
		extent_get(entry, &previous);
		if (!previous.unwritten &&
		    (u64_t)previous.logical + previous.length == logical &&
		    previous.physical + previous.length == physical &&
		    previous.length < 0x8000) {
			new_length = previous.length + 1;
			if (insert < entries) {
				extent_get(root + 12 + insert * 12, &next);
				if (!next.unwritten &&
				    (u64_t)logical + 1 == next.logical &&
				    physical + 1 == next.physical &&
				    (u64_t)new_length + next.length <= 0x8000) {
					new_length += next.length;
					memmove(root + 12 + insert * 12,
					    root + 12 + (insert + 1) * 12,
					    (entries - insert - 1) * 12);
					entries--;
				}
			}
			extent_put(entry, previous.logical, previous.physical,
			    new_length, FALSE);
			extent_put16(root + 2, entries);
			rip->i_blocks += rip->i_sp->s_sectors_in_block;
			extent_release_leaf(leaf_bp, TRUE);
			rip->i_dirt = IN_DIRTY;
			return(OK);
		}
	}

	/* Or prepend the new block to the following initialized extent. */
	if (insert < entries) {
		entry = root + 12 + insert * 12;
		extent_get(entry, &next);
		if (!next.unwritten &&
		    (u64_t)logical + 1 == next.logical &&
		    physical + 1 == next.physical &&
		    next.length < 0x8000) {
			extent_put(entry, logical, physical, next.length + 1,
			    FALSE);
			rip->i_blocks += rip->i_sp->s_sectors_in_block;
			extent_release_leaf(leaf_bp, TRUE);
			rip->i_dirt = IN_DIRTY;
			return(OK);
		}
	}

	if (entries >= max_entries) {
		extent_release_leaf(leaf_bp, FALSE);
		return(EFBIG);
	}

	/* Insert a one-block initialized extent in logical-block order. */
	entry = root + 12 + insert * 12;
	memmove(entry + 12, entry, (entries - insert) * 12);
	extent_put(entry, logical, physical, 1, FALSE);
	extent_put16(root + 2, entries + 1);
	rip->i_blocks += rip->i_sp->s_sectors_in_block;
	extent_release_leaf(leaf_bp, TRUE);
	rip->i_dirt = IN_DIRTY;
	return(OK);
}
