/*
 * ext4 extent-tree updates.
 *
 * The inode-resident root is promoted to one external leaf when its four
 * entries are exhausted.  That leaf is edited in place; splitting a full
 * external leaf, growing internal index levels, and general tree rebalancing
 * remain unsupported.
 */

#include "fs.h"
#include <string.h>
#include <stdlib.h>
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
	struct buf **bpp, u16_t *root_depthp, u16_t *root_indexp)
{
	unsigned char *node;
	struct buf *bp;
	size_t node_size;
	int expected_depth;
	u16_t magic, entries, max_entries, depth, root_depth;
	u64_t previous_end;
	u32_t expected_key;
	int have_expected_key;
	unsigned int i;

	node = (unsigned char *)rip->i_block;
	node_size = sizeof(rip->i_block);
	root_depth = extent_get16(node + 6);
	*root_depthp = root_depth;
	*root_indexp = 0;
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
			if (!found && node == (unsigned char *)rip->i_block &&
			    depth == 1) {
				/*
				 * The inode root has one level of index entries.  A
				 * logical block before its first key still belongs in
				 * the first leaf; the root key can be lowered on insert.
				 */
				selected_index = 0;
				selected_key = extent_get32(node + 12);
				child = ((u64_t)extent_get16(node + 20) << 32) |
				    extent_get32(node + 16);
				selected_child = child;
				found = TRUE;
			}
			if (!found) {
				/* Earlier keys in deeper trees need ancestor updates. */
				if (bp != NULL)
					put_block(bp);
				return(EOPNOTSUPP);
			}

			if (node == (unsigned char *)rip->i_block)
				*root_indexp = (u16_t)selected_index;

			if (bp != NULL) {
				put_block(bp);
				bp = NULL;
			}
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

/*
 * Move an inline extent root into one external leaf.  This buys the tree the
 * full block-sized leaf capacity without changing any logical/physical
 * extent mappings.  Read-write mounts currently reject metadata_csum, so
 * the external-node checksum tail is not required on this write path.
 */
static int
extent_promote_root(struct inode *rip, u32_t logical, block64_t goal)
{
	unsigned char *root, *node;
	struct buf *bp;
	block64_t node_block;
	u16_t entries, max_entries, root_max;
	u32_t first_logical, generation;
	u64_t sectors;

	root = (unsigned char *)rip->i_block;
	root_max = (u16_t)((sizeof(rip->i_block) - 12) / 12);
	if (extent_get16(root) != EXT4_EXTENT_MAGIC ||
	    extent_get16(root + 6) != 0)
		return(EOPNOTSUPP);

	entries = extent_get16(root + 2);
	max_entries = extent_get16(root + 4);
	if (max_entries != root_max || entries == 0 ||
	    entries > max_entries)
		return(EIO);

	first_logical = extent_get32(root + 12);
	/* A one-child external tree cannot prepend before its parent key. */
	if (logical < first_logical)
		return(EOPNOTSUPP);

	sectors = rip->i_sp->s_sectors_in_block;
	if (sectors == 0 || !extent_i_blocks_can_add(rip, sectors))
		return(EFBIG);

	node_block = alloc_block(rip, goal);
	if (node_block == NO_BLOCK)
		return(ENOSPC);
	if (node_block == 0 || node_block >= rip->i_sp->s_blocks_count ||
	    node_block > 0xffffffffffffULL) {
		free_block(rip->i_sp, node_block);
		return(EIO);
	}

	bp = get_block(rip->i_dev, node_block, NO_READ);
	if (bp == NULL) {
		free_block(rip->i_sp, node_block);
		return(EIO);
	}
	node = (unsigned char *)b_data(bp);
	memset(node, 0, rip->i_sp->s_block_size);
	generation = extent_get32(root + 8);
	memcpy(node + 12, root + 12, (size_t)entries * 12);
	extent_put16(node, EXT4_EXTENT_MAGIC);
	extent_put16(node + 2, entries);
	extent_put16(node + 4,
	    (u16_t)((rip->i_sp->s_block_size - 12) / 12));
	extent_put16(node + 6, 0);
	extent_put32(node + 8, generation);
	lmfs_markdirty(bp);
	put_block(bp);

	/* Replace the inline leaf with a depth-one root and one index entry. */
	memset(root, 0, sizeof(rip->i_block));
	extent_put16(root, EXT4_EXTENT_MAGIC);
	extent_put16(root + 2, 1);
	extent_put16(root + 4, root_max);
	extent_put16(root + 6, 1);
	extent_put32(root + 8, generation);
	extent_put32(root + 12, first_logical);
	extent_put32(root + 16, (u32_t)node_block);
	extent_put16(root + 20, (u16_t)(node_block >> 32));
	extent_put16(root + 22, 0);

	rip->i_blocks += sectors;
	rip->i_dirt = IN_DIRTY;
	return(OK);
}

/*
 * Update one index key in the inode-resident depth-one root.
 */
static void
extent_update_root_key(struct inode *rip, u16_t root_index, u32_t key)
{
	unsigned char *root;

	root = (unsigned char *)rip->i_block;
	extent_put32(root + 12 + (size_t)root_index * 12, key);
	rip->i_dirt = IN_DIRTY;
}

/*
 * Remove an empty leaf from a depth-one tree.  Since all children are leaves,
 * the inode-resident root can remove the index entry without recursing into
 * another index node.  Removing the last child collapses the tree to an empty
 * inline root.
 */
static int
extent_remove_root_leaf(struct inode *rip, u16_t root_index,
	block64_t data_block)
{
	unsigned char *root, *index;
	block64_t leaf_block;
	u16_t root_entries, root_max;
	u64_t sectors;
	u32_t generation;

	root = (unsigned char *)rip->i_block;
	root_max = (u16_t)((sizeof(rip->i_block) - 12) / 12);
	root_entries = extent_get16(root + 2);
	if (extent_get16(root) != EXT4_EXTENT_MAGIC ||
	    extent_get16(root + 6) != 1 ||
	    extent_get16(root + 4) != root_max ||
	    root_entries == 0 || root_entries > root_max ||
	    root_index >= root_entries)
		return(EOPNOTSUPP);

	index = root + 12 + (size_t)root_index * 12;
	leaf_block = ((u64_t)extent_get16(index + 8) << 32) |
	    extent_get32(index + 4);
	if (leaf_block == 0 || leaf_block >= rip->i_sp->s_blocks_count)
		return(EIO);

	sectors = rip->i_sp->s_sectors_in_block;
	if (sectors == 0 || sectors > ~(u64_t)0 / 2 ||
	    rip->i_blocks < sectors * 2)
		return(EIO);

	generation = extent_get32(root + 8);
	memmove(index, index + 12,
	    (size_t)(root_entries - root_index - 1) * 12);
	root_entries--;
	free_block(rip->i_sp, data_block);
	free_block(rip->i_sp, leaf_block);
	rip->i_blocks -= sectors * 2;

	if (root_entries == 0) {
		memset(root, 0, sizeof(rip->i_block));
		extent_put16(root, EXT4_EXTENT_MAGIC);
		extent_put16(root + 2, 0);
		extent_put16(root + 4, root_max);
		extent_put16(root + 6, 0);
		extent_put32(root + 8, generation);
	} else {
		extent_put16(root + 2, root_entries);
		memset(root + 12 + (size_t)root_entries * 12, 0, 12);
	}
	rip->i_dirt = IN_DIRTY;
	return(OK);
}

/*
 * Split a full external leaf directly below the inode's depth-one root.
 * The root has a fixed four-entry index area; this operation consumes one
 * slot there and one external block for the new sibling leaf.  Deeper trees
 * still require general index-node splitting and are not handled here.
 */
static int
extent_split_root_leaf(struct inode *rip, unsigned char *leaf,
	struct buf *leaf_bp, u16_t entries, u16_t max_entries,
	u16_t root_index, unsigned int insert, u32_t logical,
	u64_t physical, int update_root_key)
{
	unsigned char *root, *index, *temp, *node;
	struct buf *new_bp;
	block64_t new_block;
	u16_t root_entries, root_max, total, left_entries, right_entries;
	u16_t node_max;
	u32_t new_key, generation, next_key;
	u64_t sectors;
	size_t temp_size;

	if (leaf_bp == NULL)
		return(EINVAL);
	root = (unsigned char *)rip->i_block;
	root_max = (u16_t)((sizeof(rip->i_block) - 12) / 12);
	root_entries = extent_get16(root + 2);
	if (extent_get16(root) != EXT4_EXTENT_MAGIC ||
	    extent_get16(root + 6) != 1 ||
	    extent_get16(root + 4) != root_max ||
	    root_entries == 0 || root_entries >= root_max ||
	    root_index >= root_entries)
		return(EFBIG);

	node_max = (u16_t)((rip->i_sp->s_block_size - 12) / 12);
	if (entries != max_entries || max_entries != node_max ||
	    insert > entries || entries == 0)
		return(EOPNOTSUPP);

	sectors = rip->i_sp->s_sectors_in_block;
	if (sectors == 0 || sectors > ~(u64_t)0 / 2 ||
	    !extent_i_blocks_can_add(rip, sectors * 2))
		return(EFBIG);

	total = entries + 1;
	left_entries = total / 2;
	right_entries = total - left_entries;
	temp_size = (size_t)total * 12;
	temp = malloc(temp_size);
	if (temp == NULL)
		return(ENOMEM);

	memcpy(temp, leaf + 12, (size_t)insert * 12);
	extent_put(temp + (size_t)insert * 12, logical, physical, 1, FALSE);
	memcpy(temp + (size_t)(insert + 1) * 12,
	    leaf + 12 + (size_t)insert * 12,
	    (size_t)(entries - insert) * 12);

	new_key = extent_get32(temp + (size_t)left_entries * 12);
	index = root + 12 + (size_t)root_index * 12;
	if ((!update_root_key && extent_get32(index) !=
	    extent_get32(temp)) ||
	    (update_root_key && new_key >= extent_get32(index)) ||
	    (left_entries == 0 || right_entries == 0)) {
		free(temp);
		return(EIO);
	}
	if (root_index + 1 < root_entries) {
		next_key = extent_get32(index + 12);
		if (new_key >= next_key) {
			free(temp);
			return(EIO);
		}
	}

	new_block = alloc_block(rip, physical);
	if (new_block == NO_BLOCK) {
		free(temp);
		return(ENOSPC);
	}
	if (new_block == 0 || new_block >= rip->i_sp->s_blocks_count ||
	    new_block > 0xffffffffffffULL) {
		free_block(rip->i_sp, new_block);
		free(temp);
		return(EIO);
	}
	new_bp = get_block(rip->i_dev, new_block, NO_READ);
	if (new_bp == NULL) {
		free_block(rip->i_sp, new_block);
		free(temp);
		return(EIO);
	}

	generation = extent_get32(leaf + 8);
	node = (unsigned char *)b_data(new_bp);
	memset(node, 0, rip->i_sp->s_block_size);
	memcpy(node + 12, temp + (size_t)left_entries * 12,
	    (size_t)right_entries * 12);
	extent_put16(node, EXT4_EXTENT_MAGIC);
	extent_put16(node + 2, right_entries);
	extent_put16(node + 4, node_max);
	extent_put16(node + 6, 0);
	extent_put32(node + 8, generation);
	lmfs_markdirty(new_bp);
	put_block(new_bp);

	memset(leaf + 12 + (size_t)left_entries * 12, 0,
	    (size_t)(entries - left_entries) * 12);
	memcpy(leaf + 12, temp, (size_t)left_entries * 12);
	extent_put16(leaf + 2, left_entries);

	if (update_root_key)
		extent_put32(index, extent_get32(temp));
	index = root + 12 + (size_t)(root_index + 1) * 12;
	memmove(index + 12, index,
	    (size_t)(root_entries - root_index - 1) * 12);
	extent_put32(index, new_key);
	extent_put32(index + 4, (u32_t)new_block);
	extent_put16(index + 8, (u16_t)(new_block >> 32));
	extent_put16(index + 10, 0);
	extent_put16(root + 2, root_entries + 1);

	rip->i_blocks += sectors * 2; /* new data block and new extent node */
	rip->i_dirt = IN_DIRTY;
	free(temp);
	(void)leaf_bp; /* The caller marks the modified original leaf dirty. */
	return(OK);
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
		int update_root_key = FALSE;

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
		 * Removing the only extent from our one-leaf tree can release
		 * both the data block and the external extent node.  Other
		 * external trees need parent rebalancing and remain unsupported.
		 */
		if (leaf_bp != NULL && entries == 1 && ex.length == 1) {
			extent_release_leaf(leaf_bp, FALSE);
			return(extent_collapse_single_leaf(rip, block_to_free));
		}
		/*
		 * A one-leaf depth-one tree has its parent index in the inode,
		 * so its sole key can be updated when the first extent moves.
		 * Multi-leaf and deeper trees need general parent traversal.
		 */
		if (leaf_bp != NULL && i == 0 && offset == 0) {
			unsigned char *extent_root;

			extent_root = (unsigned char *)rip->i_block;
			if (extent_get16(extent_root) != EXT4_EXTENT_MAGIC ||
			    extent_get16(extent_root + 6) != 1 ||
			    extent_get16(extent_root + 2) != 1 ||
			    extent_get16(extent_root + 4) !=
			    (sizeof(rip->i_block) - 12) / 12) {
				extent_release_leaf(leaf_bp, FALSE);
				return(EOPNOTSUPP);
			}
			update_root_key = TRUE;
		}
		/* Splitting a middle extent consumes one additional leaf slot. */
		if (offset != 0 && offset != ex.length - 1 &&
		    entries >= max_entries) {
			if (leaf_bp == NULL) {
				r = extent_promote_root(rip, logical, block_to_free);
				if (r == OK)
					return(ext4_extent_write_map(rip, position,
					    new_wblock, op));
				extent_release_leaf(leaf_bp, FALSE);
				return(r);
			}
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
		if (update_root_key) {
			/* The root has exactly one child, which is this leaf. */
			extent_put32((unsigned char *)rip->i_block + 12,
			    extent_get32(root + 12));
		}
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
				if (leaf_bp == NULL) {
					r = extent_promote_root(rip, logical,
					    new_wblock);
					if (r == OK)
						return(ext4_extent_write_map(rip,
						    position, new_wblock, op));
					extent_release_leaf(leaf_bp, FALSE);
					return(r);
				}
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
				if (leaf_bp == NULL) {
					r = extent_promote_root(rip, logical,
					    new_wblock);
					if (r == OK)
						return(ext4_extent_write_map(rip,
						    position, new_wblock, op));
					extent_release_leaf(leaf_bp, FALSE);
					return(r);
				}
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
				if (leaf_bp == NULL) {
					r = extent_promote_root(rip, logical,
					    new_wblock);
					if (r == OK)
						return(ext4_extent_write_map(rip,
						    position, new_wblock, op));
					extent_release_leaf(leaf_bp, FALSE);
					return(r);
				}
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
		if (leaf_bp == NULL) {
			r = extent_promote_root(rip, logical, physical);
			if (r == OK)
				return(ext4_extent_write_map(rip, position,
				    new_wblock, op));
			extent_release_leaf(leaf_bp, FALSE);
			return(r);
		}
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
