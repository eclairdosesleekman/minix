/*
 * ext4 extent-tree updates.
 *
 * Inline roots and existing external leaves are edited in place. Full
 * external leaves can also be split when a depth-one inode root has room for
 * another index entry. Deeper-tree growth and root rebalancing remain
 * unsupported and are rejected before modifying the mapping.
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
	struct buf **bpp, unsigned char **root_nodep,
	u16_t *root_entriesp, u16_t *root_maxp, u16_t *root_indexp,
	u16_t *root_depthp)
{
	unsigned char *node;
	struct buf *bp;
	size_t node_size;
	int expected_depth;
	u16_t magic, entries, max_entries, depth;
	u64_t previous_end;
	u32_t expected_key;
	int have_expected_key;
	unsigned int i;

	node = (unsigned char *)rip->i_block;
	node_size = sizeof(rip->i_block);
	bp = NULL;
	expected_depth = -1;
	have_expected_key = FALSE;

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
		if (bp == NULL) {
			*root_nodep = node;
			*root_entriesp = entries;
			*root_maxp = max_entries;
			*root_indexp = 0;
			*root_depthp = depth;
		}

		/*
		 * External extent nodes store their checksum immediately after the
		 * maximum-size entry array, not after the used entries. Validate
		 * every node in the path before allowing an in-place edit.
		 */
		if (bp != NULL &&
		    HAS_RO_COMPAT_FEATURE(rip->i_sp, RO_COMPAT_METADATA_CSUM)) {
			size_t tail_offset;
			u32_t stored, calculated;

			tail_offset = 12 + (size_t)max_entries * 12;
			if (tail_offset > node_size - sizeof(stored))
				goto corrupt;
			stored = extent_get32(node + tail_offset);
			calculated = ext4_crc32c(ext4_inode_csum_seed(rip),
			    node, tail_offset);
			if (stored != calculated)
				goto corrupt;
		}
		/*
		 * Each index entry records the first logical block in its child.
		 * Check that the selected child really starts at that key.
		 */
		if (have_expected_key &&
		    (entries == 0 || extent_get32(node + 12) != expected_key))
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
			u32_t previous_key, key, selected_key;
			unsigned int selected_index;
			int found;

			selected_child = 0;
			selected_key = 0;
			selected_index = 0;
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

				if (key <= logical) {
					selected_child = child;
					selected_key = key;
					selected_index = i;
					found = TRUE;
				}
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
			if (expected_depth == -1)
				*root_indexp = (u16_t)selected_index;
			expected_key = selected_key;
			have_expected_key = TRUE;
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
extent_release_leaf(struct inode *rip, struct buf *bp, int dirty)
{
	if (bp != NULL) {
		if (dirty &&
		    HAS_RO_COMPAT_FEATURE(rip->i_sp, RO_COMPAT_METADATA_CSUM)) {
			unsigned char *node;
			size_t tail_offset;
			u16_t max_entries;
			u32_t checksum;

			node = (unsigned char *)b_data(bp);
			max_entries = extent_get16(node + 4);
			tail_offset = 12 + (size_t)max_entries * 12;
			if (tail_offset > rip->i_sp->s_block_size -
			    sizeof(checksum))
				panic("ext4: invalid extent checksum tail");
			checksum = ext4_crc32c(ext4_inode_csum_seed(rip),
			    node, tail_offset);
			extent_put32(node + tail_offset, checksum);
		}
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

/*
 * Split a full external leaf under a depth-one inode root. The new leaf and
 * both resulting index keys are prepared only after every bound and capacity
 * check has passed. On failure, the original inode and leaf remain unchanged.
 */
static int
extent_split_full_leaf(struct inode *rip, unsigned char *leaf,
	struct buf *leaf_bp, unsigned char *root_node, u16_t root_entries,
	u16_t root_max, u16_t root_index, u16_t root_depth,
	u16_t leaf_entries, u16_t leaf_max, unsigned int insert,
	u32_t logical, u64_t physical)
{
	struct buf *new_bp;
	unsigned char *new_node, *new_index;
	unsigned char *records;
	u64_t upper, sectors;
	block64_t new_block;
	u32_t first_key, right_key;
	u64_t end_key;
	u16_t left_entries, right_entries;
	size_t records_size;
	unsigned int i;

	if (leaf_bp == NULL || root_node == NULL || root_depth != 1 ||
	    leaf_entries != leaf_max || insert > leaf_entries ||
	    root_entries == 0 || root_index >= root_entries ||
	    root_entries >= root_max) {
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(EFBIG);
	}

	first_key = extent_get32(leaf + 12);
	if (extent_get32(root_node + 12 + root_index * 12) != first_key ||
	    logical < first_key) {
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(EIO);
	}

	upper = root_index + 1 < root_entries ?
	    extent_get32(root_node + 12 + (root_index + 1) * 12) :
	    (u64_t)UINT32_MAX + 1;
	if ((u64_t)logical + 1 > upper) {
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(EIO);
	}
	for (i = 0; i < leaf_entries; i++) {
		struct ext4_extent ex;

		extent_get(leaf + 12 + i * 12, &ex);
		end_key = (u64_t)ex.logical + ex.length;
		if (end_key > upper) {
			extent_release_leaf(rip, leaf_bp, FALSE);
			return(EIO);
		}
	}

	sectors = rip->i_sp->s_sectors_in_block;
	if (sectors == 0 || sectors > ~(u64_t)0 / 2 ||
	    !extent_i_blocks_can_add(rip, sectors * 2)) {
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(EFBIG);
	}

	if (leaf_entries >= (UINT16_MAX - 1)) {
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(EFBIG);
	}
	records_size = (size_t)(leaf_entries + 1) * 12;
	records = malloc(records_size);
	if (records == NULL) {
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(ENOMEM);
	}

	/* Build a sorted scratch copy without touching the live leaf. */
	memcpy(records, leaf + 12, (size_t)insert * 12);
	extent_put(records + (size_t)insert * 12, logical, physical, 1, FALSE);
	memcpy(records + (size_t)(insert + 1) * 12,
	    leaf + 12 + (size_t)insert * 12,
	    (size_t)(leaf_entries - insert) * 12);

	left_entries = (u16_t)((leaf_entries + 1) / 2);
	right_entries = (u16_t)(leaf_entries + 1 - left_entries);
	right_key = extent_get32(records + (size_t)left_entries * 12);
	if (left_entries == 0 || right_entries == 0 ||
	    right_key <= first_key || (u64_t)right_key >= upper) {
		free(records);
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(EIO);
	}

	/* Allocate and fully construct the new leaf before publishing its key. */
	new_block = alloc_block(rip, NO_BLOCK);
	if (new_block == NO_BLOCK) {
		free(records);
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(ENOSPC);
	}
	if (new_block < rip->i_sp->s_first_data_block ||
	    new_block >= rip->i_sp->s_blocks_count)
		panic("ext4: allocator returned invalid extent-tree block");
	if (new_block > 0xffffffffffffULL) {
		free_block(rip->i_sp, new_block);
		free(records);
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(EFBIG);
	}
	new_bp = get_block(rip->i_dev, new_block, NO_READ);
	if (new_bp == NULL) {
		free_block(rip->i_sp, new_block);
		free(records);
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(EIO);
	}
	new_node = (unsigned char *)b_data(new_bp);
	memset(new_node, 0, rip->i_sp->s_block_size);
	extent_put16(new_node, EXT4_EXTENT_MAGIC);
	extent_put16(new_node + 2, right_entries);
	extent_put16(new_node + 4, leaf_max);
	extent_put16(new_node + 6, 0);
	extent_put32(new_node + 8, 0);
	memcpy(new_node + 12, records + (size_t)left_entries * 12,
	    (size_t)right_entries * 12);

	/* Keep the old node's first key, and move the upper half to new_node. */
	memcpy(leaf + 12, records, (size_t)left_entries * 12);
	memset(leaf + 12 + (size_t)left_entries * 12, 0,
	    (size_t)(leaf_max - left_entries) * 12);
	extent_put16(leaf + 2, left_entries);

	/* Publish a new root index immediately after the split child. */
	new_index = root_node + 12 + (root_index + 1) * 12;
	memmove(new_index + 12, new_index,
	    (size_t)(root_entries - root_index - 1) * 12);
	extent_put32(new_index, right_key);
	extent_put32(new_index + 4, (u32_t)new_block);
	extent_put16(new_index + 8, (u16_t)(new_block >> 32));
	extent_put16(new_index + 10, 0);
	extent_put16(root_node + 2, root_entries + 1);

	/* Both external nodes need fresh checksums when metadata_csum is active. */
	extent_release_leaf(rip, new_bp, TRUE);
	extent_release_leaf(rip, leaf_bp, TRUE);
	free(records);

	rip->i_blocks += sectors * 2; /* one data block plus one extent-tree node */
	rip->i_dirt = IN_DIRTY;
	return(OK);
}

int
ext4_extent_write_map(struct inode *rip, off_t position,
	block64_t new_wblock, int op)
{
	unsigned char *root, *entry, *root_node;
	struct buf *leaf_bp;
	struct ext4_extent ex, previous, next;
	size_t node_size;
	u16_t entries, max_entries, root_entries, root_max;
	u16_t root_index, root_depth;
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
	    &max_entries, &leaf_bp, &root_node, &root_entries, &root_max,
	    &root_index, &root_depth)) != OK)
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
			extent_release_leaf(rip, leaf_bp, FALSE);
			return(OK);	/* A hole needs no work. */
		}

		offset = logical - ex.logical;
		block_to_free = ex.physical + offset;
		if (rip->i_blocks < rip->i_sp->s_sectors_in_block) {
			extent_release_leaf(rip, leaf_bp, FALSE);
			return(EIO);
		}
		/*
		 * Changing or removing the first extent in an external leaf
		 * also changes its parent's index key.  Do not leave a stale key.
		 */
		if (leaf_bp != NULL && i == 0 && offset == 0) {
			extent_release_leaf(rip, leaf_bp, FALSE);
			return(EOPNOTSUPP);
		}
		/* Splitting a middle extent consumes one additional leaf slot. */
		if (offset != 0 && offset != ex.length - 1 &&
		    entries >= max_entries) {
			extent_release_leaf(rip, leaf_bp, FALSE);
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
		extent_release_leaf(rip, leaf_bp, TRUE);
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
			extent_release_leaf(rip, leaf_bp, FALSE);
			return(EEXIST);
		}
		offset = logical - ex.logical;
		old_physical = ex.physical + offset;
		if (offset == 0 && ex.length == 1) {
			extent_put(entry, logical, old_physical, 1, FALSE);
		} else if (offset == 0) {
			if (entries >= max_entries) {
				extent_release_leaf(rip, leaf_bp, FALSE);
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
				extent_release_leaf(rip, leaf_bp, FALSE);
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
				extent_release_leaf(rip, leaf_bp, FALSE);
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
		extent_release_leaf(rip, leaf_bp, TRUE);
		rip->i_dirt = IN_DIRTY;
		return(OK);
	}

	physical = (u64_t)new_wblock;
	if (physical == 0 || physical >= rip->i_sp->s_blocks_count ||
	    physical > (((u64_t)1 << 48) - 1)) {
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(EIO);
	}
	/*
	 * The in-core counter is 64 bits.  Do not retain the old ext2
	 * 32-bit limit here: HUGE_FILE filesystems can store a 48-bit
	 * sector count (or a 48-bit filesystem-block count).
	 */
	if (!extent_i_blocks_can_add(rip,
	    rip->i_sp->s_sectors_in_block)) {
		extent_release_leaf(rip, leaf_bp, FALSE);
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
			extent_release_leaf(rip, leaf_bp, FALSE);
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
			extent_release_leaf(rip, leaf_bp, TRUE);
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
			extent_release_leaf(rip, leaf_bp, TRUE);
			rip->i_dirt = IN_DIRTY;
			return(OK);
		}
	}

	if (entries >= max_entries) {
		/*
		 * The selected depth-one external leaf can grow by one record
		 * when the inode root still has an unused index slot. Deeper trees
		 * and a full root still fail without changing any on-disk mapping.
		 */
		r = extent_split_full_leaf(rip, root, leaf_bp, root_node,
		    root_entries, root_max, root_index, root_depth, entries,
		    max_entries, insert, logical, physical);
		return(r);
	}

	/* Insert a one-block initialized extent in logical-block order. */
	entry = root + 12 + insert * 12;
	memmove(entry + 12, entry, (entries - insert) * 12);
	extent_put(entry, logical, physical, 1, FALSE);
	extent_put16(root + 2, entries + 1);
	rip->i_blocks += rip->i_sp->s_sectors_in_block;
	extent_release_leaf(rip, leaf_bp, TRUE);
	rip->i_dirt = IN_DIRTY;
	return(OK);
}
