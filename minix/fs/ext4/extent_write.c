/*
 * Limited ext4 extent-tree updates.
 *
 * This writer supports inline roots and in-place updates to depth-one external
 * leaf nodes.  It does not allocate/split external nodes or rebalance deeper
 * trees; operations requiring those changes are rejected.
 */

#include "fs.h"
#include <string.h>
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
 * Test physical block ranges without forming end addresses.  The callers
 * validate the ranges against s_blocks_count before invoking this helper.
 */
static int
extent_ranges_overlap(block64_t first, u32_t first_len,
	block64_t second, u32_t second_len)
{
	if (first <= second)
		return(second - first < first_len);
	return(first - second < second_len);
}

/*
 * Return TRUE if two validated leaf nodes claim any common physical block.
 * The caller validates every range against the filesystem before comparing.
 */
static int
extent_leaf_ranges_overlap(const unsigned char *first_entries,
	u16_t first_count, const unsigned char *second_entries,
	u16_t second_count)
{
	struct ext4_extent first, second;
	unsigned int i, j;

	for (i = 0; i < first_count; i++) {
		extent_get(first_entries + i * 12, &first);
		for (j = 0; j < second_count; j++) {
			extent_get(second_entries + j * 12, &second);
			if (extent_ranges_overlap(first.physical, first.length,
			    second.physical, second.length))
				return(TRUE);
		}
	}

	return(FALSE);
}

/* Return TRUE if a candidate physical block is already mapped in this node. */
static int
extent_block_is_mapped(const unsigned char *entries, u16_t count,
	block64_t block)
{
	struct ext4_extent ex;
	unsigned int i;

	for (i = 0; i < count; i++) {
		extent_get(entries + i * 12, &ex);
		if (block >= ex.physical && block - ex.physical < ex.length)
			return(TRUE);
	}
	return(FALSE);
}

/* Validate physical mappings before a write can free or replace them. */
static int extent_is_group_metadata(struct super_block *sp, block64_t block);
static int extent_overlaps_group_metadata(struct super_block *sp,
	block64_t first, u32_t length);

/*
 * Return the inode's inline extent root.  Depth-one trees are handled by the
 * separate in-place leaf updater; this helper accepts only depth zero.
 */
static int
extent_root(struct inode *rip, unsigned char **rootp, u16_t *entriesp,
	u16_t *maxp)
{
	unsigned char *root;
	u16_t magic, entries, max_entries, depth;
	u64_t previous_end = 0;
	unsigned int i;

	root = (unsigned char *)rip->i_block;
	magic = extent_get16(root);
	entries = extent_get16(root + 2);
	max_entries = extent_get16(root + 4);
	depth = extent_get16(root + 6);

	if (magic != EXT4_EXTENT_MAGIC ||
	    depth > EXT4_EXTENT_MAX_DEPTH ||
	    max_entries == 0 ||
	    max_entries > (sizeof(rip->i_block) - 12) / 12 ||
	    entries > max_entries)
		return(EIO);
	if (depth != 0)
		return(EOPNOTSUPP);

	for (i = 0; i < entries; i++) {
		struct ext4_extent ex, prior;
		unsigned int j;

		extent_get(root + 12 + i * 12, &ex);
		/*
		 * Do not let a damaged inline extent alias filesystem metadata.
		 * WMAP_FREE releases every block in the extent, so accepting a
		 * bitmap or inode-table block here would corrupt the filesystem.
		 */
		if (ex.length == 0 ||
		    ex.physical <= rip->i_sp->s_first_data_block ||
		    ex.physical >= rip->i_sp->s_blocks_count ||
		    ex.length > rip->i_sp->s_blocks_count - ex.physical ||
		    extent_overlaps_group_metadata(rip->i_sp, ex.physical,
		    ex.length) ||
		    (u64_t)ex.logical + ex.length > ((u64_t)UINT32_MAX + 1) ||
		    (i != 0 && ex.logical < previous_end))
			return(EIO);

		/* Two logical extents must never own the same physical blocks. */
		for (j = 0; j < i; j++) {
			extent_get(root + 12 + j * 12, &prior);
			if (extent_ranges_overlap(ex.physical, ex.length,
			    prior.physical, prior.length))
				return(EIO);
		}
		previous_end = (u64_t)ex.logical + ex.length;
	}

	*rootp = root;
	*entriesp = entries;
	*maxp = max_entries;
	return(OK);
}

/*
 * Keep the write-side allocation check in sync with ext4_encode_inode_blocks()
 * in inode.c.  i_blocks is maintained in 512-byte sectors in memory.  Without
 * HUGE_FILE the on-disk field is 32 bits; with HUGE_FILE it is 48 bits, and
 * values above that can be represented as filesystem blocks when aligned.
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
 * Update a depth-one extent tree in place.  The inode contains the index
 * entries; each referenced external block contains a depth-zero leaf.
 * Splitting/rebalancing the tree is deliberately out of scope: operations
 * that need another leaf entry are rejected before either node is changed.
 */
/*
 * External extent-node pointers are disk metadata, not ordinary file data.
 * Reject aliases of the group allocation bitmaps and inode-table ranges
 * before acquiring a writable buffer.  The scan intentionally covers all
 * groups because flex_bg may place metadata outside its nominal group.
 */
static int
extent_is_group_metadata(struct super_block *sp, block64_t block)
{
	struct group_desc *gd;
	unsigned int group;

	for (group = 0; group < sp->s_groups_count; group++) {
		gd = get_group_desc(group);
		if (gd == NULL)
			return(TRUE);
		if (block == gd->block_bitmap ||
		    block == gd->inode_bitmap ||
		    (block >= gd->inode_table &&
		    block - gd->inode_table < sp->s_itb_per_group))
			return(TRUE);
	}
	return(FALSE);
}

static int
extent_overlaps_group_metadata(struct super_block *sp, block64_t first,
	u32_t length)
{
	struct group_desc *gd;
	block64_t end, table_end;
	unsigned int group;

	end = first + length;
	for (group = 0; group < sp->s_groups_count; group++) {
		gd = get_group_desc(group);
		if (gd == NULL)
			return(TRUE);
		if ((gd->block_bitmap >= first && gd->block_bitmap < end) ||
		    (gd->inode_bitmap >= first && gd->inode_bitmap < end))
			return(TRUE);
		table_end = gd->inode_table + sp->s_itb_per_group;
		if (first < table_end && gd->inode_table < end)
			return(TRUE);
	}
	return(FALSE);
}

static int
extent_overlaps_tree_node(const unsigned char *root,
	u16_t root_entries, block64_t first, u32_t length)
{
	block64_t end, child;
	unsigned int i;

	end = first + length;
	for (i = 0; i < root_entries; i++) {
		const unsigned char *index_entry = root + 12 + i * 12;

		child = ((u64_t)extent_get16(index_entry + 8) << 32) |
		    extent_get32(index_entry + 4);
		if (child >= first && child < end)
			return(TRUE);
	}
	return(FALSE);
}

/*
 * Validate every sibling leaf against the leaf being modified. A physical
 * range duplicated in another leaf must not be freed by this operation.
 * Also check a speculative allocation against sibling data before it can be
 * inserted or discarded during unwritten-extent conversion.
 */
static int
extent_depth1_check_siblings(struct inode *rip, const unsigned char *root,
	u16_t root_entries, unsigned int selected,
	const unsigned char *selected_node, u16_t selected_entries,
	int check_candidate, block64_t candidate)
{
	struct ext4_extent ex, selected_ex, prior;
	struct buf *bp;
	const unsigned char *index_entry, *node;
	block64_t leaf_block;
	u64_t previous_end, next_key;
	u32_t root_key;
	u16_t leaf_entries, leaf_max;
	unsigned int i, j, k;

	for (i = 0; i < root_entries; i++) {
		if (i == selected)
			continue;

		index_entry = root + 12 + i * 12;
		root_key = extent_get32(index_entry);
		leaf_block = ((u64_t)extent_get16(index_entry + 8) << 32) |
		    extent_get32(index_entry + 4);
		bp = get_block(rip->i_dev, leaf_block, NORMAL);
		if (bp == NULL)
			return(EIO);
		node = (const unsigned char *)b_data(bp);

		if (extent_get16(node) != EXT4_EXTENT_MAGIC ||
		    extent_get16(node + 6) != 0) {
			put_block(bp);
			return(EIO);
		}
		leaf_entries = extent_get16(node + 2);
		leaf_max = extent_get16(node + 4);
		if (leaf_max == 0 ||
		    leaf_max > (rip->i_sp->s_block_size - 12) / 12 ||
		    leaf_entries == 0 || leaf_entries > leaf_max) {
			put_block(bp);
			return(EIO);
		}

		previous_end = 0;
		next_key = (u64_t)UINT32_MAX + 1;
		if (i + 1 < root_entries)
			next_key = extent_get32(root + 12 + (i + 1) * 12);

		for (j = 0; j < leaf_entries; j++) {
			extent_get(node + 12 + j * 12, &ex);
			if (ex.length == 0 ||
			    ex.physical <= rip->i_sp->s_first_data_block ||
			    ex.physical >= rip->i_sp->s_blocks_count ||
			    ex.length > rip->i_sp->s_blocks_count - ex.physical ||
			    extent_overlaps_group_metadata(rip->i_sp, ex.physical,
			    ex.length) ||
			    extent_overlaps_tree_node(root, root_entries, ex.physical,
			    ex.length) ||
			    (u64_t)ex.logical + ex.length >
			    ((u64_t)UINT32_MAX + 1) ||
			    (j != 0 && ex.logical < previous_end) ||
			    (j == 0 && ex.logical != root_key)) {
				put_block(bp);
				return(EIO);
			}

			/* Reject overlaps within this sibling leaf as well. */
			for (k = 0; k < j; k++) {
				extent_get(node + 12 + k * 12, &prior);
				if (extent_ranges_overlap(ex.physical, ex.length,
				    prior.physical, prior.length)) {
					put_block(bp);
					return(EIO);
				}
			}

			/* The selected and sibling leaves must own disjoint blocks. */
			for (k = 0; k < selected_entries; k++) {
				extent_get(selected_node + 12 + k * 12, &selected_ex);
				if (extent_ranges_overlap(ex.physical, ex.length,
				    selected_ex.physical, selected_ex.length)) {
					put_block(bp);
					return(EIO);
				}
			}

			if (check_candidate && candidate >= ex.physical &&
			    candidate - ex.physical < ex.length) {
				put_block(bp);
				return(EIO);
			}
			previous_end = (u64_t)ex.logical + ex.length;
		}

		if (previous_end > next_key) {
			put_block(bp);
			return(EIO);
		}
		put_block(bp);
	}

	/*
	 * The selected leaf was checked against every sibling above.  Also
	 * compare the remaining sibling pairs so a write never proceeds on a
	 * depth-one tree whose untouched leaves alias one another.
	 */
	for (i = 0; i < root_entries; i++) {
		if (i == selected)
			continue;

		index_entry = root + 12 + i * 12;
		leaf_block = ((u64_t)extent_get16(index_entry + 8) << 32) |
		    extent_get32(index_entry + 4);
		bp = get_block(rip->i_dev, leaf_block, NORMAL);
		if (bp == NULL)
			return(EIO);
		node = (const unsigned char *)b_data(bp);
		if (extent_get16(node) != EXT4_EXTENT_MAGIC ||
		    extent_get16(node + 6) != 0) {
			put_block(bp);
			return(EIO);
		}
		leaf_entries = extent_get16(node + 2);
		leaf_max = extent_get16(node + 4);
		if (leaf_max == 0 ||
		    leaf_max > (rip->i_sp->s_block_size - 12) / 12 ||
		    leaf_entries == 0 || leaf_entries > leaf_max) {
			put_block(bp);
			return(EIO);
		}

		for (j = i + 1; j < root_entries; j++) {
			struct buf *other_bp;
			const unsigned char *other_index, *other_node;
			block64_t other_block;
			u16_t other_entries, other_max;

			if (j == selected)
				continue;
			other_index = root + 12 + j * 12;
			other_block =
			    ((u64_t)extent_get16(other_index + 8) << 32) |
			    extent_get32(other_index + 4);
			other_bp = get_block(rip->i_dev, other_block, NORMAL);
			if (other_bp == NULL) {
				put_block(bp);
				return(EIO);
			}
			other_node = (const unsigned char *)b_data(other_bp);
			if (extent_get16(other_node) != EXT4_EXTENT_MAGIC ||
			    extent_get16(other_node + 6) != 0) {
				put_block(other_bp);
				put_block(bp);
				return(EIO);
			}
			other_entries = extent_get16(other_node + 2);
			other_max = extent_get16(other_node + 4);
			if (other_max == 0 ||
			    other_max > (rip->i_sp->s_block_size - 12) / 12 ||
			    other_entries == 0 || other_entries > other_max) {
				put_block(other_bp);
				put_block(bp);
				return(EIO);
			}
			if (extent_leaf_ranges_overlap(node + 12, leaf_entries,
			    other_node + 12, other_entries)) {
				put_block(other_bp);
				put_block(bp);
				return(EIO);
			}
			put_block(other_bp);
		}
		put_block(bp);
	}

	return(OK);
}

/*
 * Preflight final inode reclamation for extent trees. truncate_inode() walks
 * logical blocks only up to i_size, so reject depth-one trees with mappings
 * beyond EOF rather than freeing the inode while leaving those blocks behind.
 * The existing depth-one writer removes each external leaf when its final
 * extent is freed.
 */
int
ext4_extent_can_truncate(struct inode *rip)
{
	unsigned char *root, *index_entry, *node;
	struct ext4_extent ex, prior;
	struct super_block *sp;
	struct buf *bp;
	block64_t child, leaf_block, prior_child;
	u64_t file_blocks, previous_end, next_key, logical_end, owned_blocks;
	u32_t root_key, previous_key;
	u16_t root_entries, root_max, depth, leaf_entries, leaf_max;
	unsigned int i, j, k;

	sp = rip->i_sp;
	if (!sp->s_is_ext4 || !(rip->i_flags & EXT4_EXTENTS_FL))
		return(OK);

	root = (unsigned char *)rip->i_block;
	if (extent_get16(root) != EXT4_EXTENT_MAGIC)
		return(EIO);
	depth = extent_get16(root + 6);
	if (depth == 0)
		return(OK);
	if (depth != 1)
		return(EOPNOTSUPP);

	root_entries = extent_get16(root + 2);
	root_max = extent_get16(root + 4);
	if (root_max == 0 ||
	    root_max > (sizeof(rip->i_block) - 12) / 12 ||
	    root_entries == 0 || root_entries > root_max ||
	    sp->s_block_size == 0 || rip->i_size < 0)
		return(EIO);

	file_blocks = (u64_t)rip->i_size / sp->s_block_size;
	if ((u64_t)rip->i_size % sp->s_block_size != 0)
		file_blocks++;
	if (file_blocks > (u64_t)UINT32_MAX + 1)
		return(EOPNOTSUPP);

	/* Validate all root keys and external-node pointers before reading them. */
	previous_key = 0;
	for (i = 0; i < root_entries; i++) {
		index_entry = root + 12 + i * 12;
		root_key = extent_get32(index_entry);
		child = ((u64_t)extent_get16(index_entry + 8) << 32) |
		    extent_get32(index_entry + 4);
		if (child <= sp->s_first_data_block ||
		    child >= sp->s_blocks_count ||
		    extent_is_group_metadata(sp, child) ||
		    (i != 0 && root_key <= previous_key))
			return(EIO);
		for (j = 0; j < i; j++) {
			const unsigned char *prior_index = root + 12 + j * 12;
			prior_child =
			    ((u64_t)extent_get16(prior_index + 8) << 32) |
			    extent_get32(prior_index + 4);
			if (prior_child == child)
				return(EIO);
		}
		previous_key = root_key;
	}

	owned_blocks = 0;
	for (i = 0; i < root_entries; i++) {
		index_entry = root + 12 + i * 12;
		root_key = extent_get32(index_entry);
		leaf_block = ((u64_t)extent_get16(index_entry + 8) << 32) |
		    extent_get32(index_entry + 4);
		bp = get_block(rip->i_dev, leaf_block, NORMAL);
		if (bp == NULL)
			return(EIO);
		node = (unsigned char *)b_data(bp);

		if (extent_get16(node) != EXT4_EXTENT_MAGIC ||
		    extent_get16(node + 6) != 0) {
			put_block(bp);
			return(EIO);
		}
		leaf_entries = extent_get16(node + 2);
		leaf_max = extent_get16(node + 4);
		if (leaf_max == 0 ||
		    leaf_max > (sp->s_block_size - 12) / 12 ||
		    leaf_entries == 0 || leaf_entries > leaf_max) {
			put_block(bp);
			return(EIO);
		}

		previous_end = 0;
		next_key = (u64_t)UINT32_MAX + 1;
		if (i + 1 < root_entries)
			next_key = extent_get32(root + 12 + (i + 1) * 12);
		for (j = 0; j < leaf_entries; j++) {
			extent_get(node + 12 + j * 12, &ex);
			logical_end = (u64_t)ex.logical + ex.length;
			if (ex.length == 0 ||
			    ex.physical <= sp->s_first_data_block ||
			    ex.physical >= sp->s_blocks_count ||
			    ex.length > sp->s_blocks_count - ex.physical ||
			    extent_overlaps_group_metadata(sp, ex.physical,
			    ex.length) ||
			    extent_overlaps_tree_node(root, root_entries,
			    ex.physical, ex.length) ||
			    logical_end > ((u64_t)UINT32_MAX + 1) ||
			    (j != 0 && ex.logical < previous_end) ||
			    (j == 0 && ex.logical != root_key)) {
				put_block(bp);
				return(EIO);
			}
			/* This truncate implementation never visits blocks beyond i_size. */
			if (logical_end > file_blocks) {
				put_block(bp);
				return(EOPNOTSUPP);
			}

			for (k = 0; k < j; k++) {
				extent_get(node + 12 + k * 12, &prior);
				if (extent_ranges_overlap(ex.physical, ex.length,
				    prior.physical, prior.length)) {
					put_block(bp);
					return(EIO);
				}
			}
			if (owned_blocks > ~(u64_t)0 - ex.length) {
				put_block(bp);
				return(EIO);
			}
			owned_blocks += ex.length;
			previous_end = logical_end;
		}
		if (previous_end > next_key) {
			put_block(bp);
			return(EIO);
		}
		if (owned_blocks == ~(u64_t)0) {
			put_block(bp);
			return(EIO);
		}
		owned_blocks++; /* One external leaf node. */

		/*
		 * This also checks physical aliases between the selected leaf and
		 * siblings, and between every pair of siblings.
		 */
		if (i == 0 && extent_depth1_check_siblings(rip, root,
		    root_entries, 0, node, leaf_entries, FALSE, 0) != OK) {
			put_block(bp);
			return(EIO);
		}
		put_block(bp);
	}

	/* i_blocks includes the data blocks and the external leaf nodes. */
	if (sp->s_sectors_in_block == 0 ||
	    owned_blocks > ~(u64_t)0 / sp->s_sectors_in_block ||
	    owned_blocks * sp->s_sectors_in_block > rip->i_blocks)
		return(EIO);

	return(OK);
}

static int
extent_depth1_write_map(struct inode *rip, off_t position,
	block64_t new_wblock, int op)
{
	unsigned char *root, *index_entry, *node, *entry;
	struct ext4_extent ex, previous, next;
	struct buf *bp;
	block64_t leaf_block, child;
	u64_t logical64, previous_end, leaf_end, next_key;
	u32_t logical, offset, new_length, root_key, child_lo;
	u16_t root_entries, root_max, depth, leaf_entries, leaf_max;
	u16_t child_hi;
	unsigned int leaf_index, i, insert;
	int free_leaf;

	if (rip->i_sp->s_rd_only)
		return(EROFS);
	if (position < 0)
		return(EINVAL);

	logical64 = (u64_t)position / rip->i_sp->s_block_size;
	if (logical64 > UINT32_MAX)
		return(EFBIG);
	logical = (u32_t)logical64;
	root = (unsigned char *)rip->i_block;

	if (extent_get16(root) != EXT4_EXTENT_MAGIC)
		return(EIO);
	root_entries = extent_get16(root + 2);
	root_max = extent_get16(root + 4);
	depth = extent_get16(root + 6);
	if (depth != 1 || root_max == 0 ||
	    root_max > (sizeof(rip->i_block) - 12) / 12 ||
	    root_entries == 0 || root_entries > root_max)
		return(EIO);

	/* Check the inode index array and choose the child for this key. */
	leaf_index = 0;
	previous_end = 0;
	for (i = 0; i < root_entries; i++) {
		index_entry = root + 12 + i * 12;
		root_key = extent_get32(index_entry);
		child_lo = extent_get32(index_entry + 4);
		child_hi = extent_get16(index_entry + 8);
		child = ((u64_t)child_hi << 32) | child_lo;
		if (child <= rip->i_sp->s_first_data_block ||
		    child >= rip->i_sp->s_blocks_count ||
		    extent_is_group_metadata(rip->i_sp, child) ||
		    (i != 0 && root_key <= previous_end))
			return(EIO);
		if (i == 0 || root_key <= logical)
			leaf_index = i;
		previous_end = root_key;

		/* Two index records must never alias the same external node. */
		{
			unsigned int j;
			for (j = 0; j < i; j++) {
				const unsigned char *prior = root + 12 + j * 12;
				u64_t prior_block =
				    ((u64_t)extent_get16(prior + 8) << 32) |
				    extent_get32(prior + 4);
				if (prior_block == child)
					return(EIO);
			}
		}
	}

	index_entry = root + 12 + leaf_index * 12;
	root_key = extent_get32(index_entry);
	child = ((u64_t)extent_get16(index_entry + 8) << 32) |
	    extent_get32(index_entry + 4);
	leaf_block = child;
	bp = get_block(rip->i_dev, leaf_block, NORMAL);
	if (bp == NULL)
		return(EIO);
	node = (unsigned char *)b_data(bp);

	if (extent_get16(node) != EXT4_EXTENT_MAGIC ||
	    extent_get16(node + 6) != 0) {
		put_block(bp);
		return(EIO);
	}
	leaf_entries = extent_get16(node + 2);
	leaf_max = extent_get16(node + 4);
	if (leaf_max == 0 ||
	    leaf_max > (rip->i_sp->s_block_size - 12) / 12 ||
	    leaf_entries == 0 || leaf_entries > leaf_max) {
		put_block(bp);
		return(EIO);
	}

	/*
	 * Validate the complete leaf before mutation.  The root index key must
	 * name its first extent, and a leaf may not overlap the next index key.
	 */
	previous_end = 0;
	for (i = 0; i < leaf_entries; i++) {
		extent_get(node + 12 + i * 12, &ex);
		if (ex.length == 0 ||
		    ex.physical <= rip->i_sp->s_first_data_block ||
		    ex.physical >= rip->i_sp->s_blocks_count ||
		    ex.length > rip->i_sp->s_blocks_count - ex.physical ||
		    extent_overlaps_group_metadata(rip->i_sp, ex.physical,
		    ex.length) ||
		    extent_overlaps_tree_node(root, root_entries, ex.physical,
		    ex.length) ||
		    (u64_t)ex.logical + ex.length >
		    ((u64_t)UINT32_MAX + 1) ||
		    (i != 0 && ex.logical < previous_end)) {
			put_block(bp);
			return(EIO);
		}
		if (i == 0 && ex.logical != root_key) {
			put_block(bp);
			return(EIO);
		}

		/* Reject duplicate physical mappings within this external leaf. */
		{
			struct ext4_extent prior;
			unsigned int j;

			for (j = 0; j < i; j++) {
				extent_get(node + 12 + j * 12, &prior);
				if (extent_ranges_overlap(ex.physical, ex.length,
				    prior.physical, prior.length)) {
					put_block(bp);
					return(EIO);
				}
			}
		}
		previous_end = (u64_t)ex.logical + ex.length;
	}

	/*
	 * new_wblock is allocated before this callback, even when it merely
	 * replaces an unwritten extent. Never free or insert it if allocator
	 * metadata points at a block already owned by the selected leaf or its tree.
	 */
	if (!(op & WMAP_FREE) &&
	    (new_wblock <= rip->i_sp->s_first_data_block ||
	    new_wblock >= rip->i_sp->s_blocks_count ||
	    new_wblock > (((u64_t)1 << 48) - 1) ||
	    extent_is_group_metadata(rip->i_sp, new_wblock) ||
	    extent_overlaps_tree_node(root, root_entries, new_wblock, 1) ||
	    extent_block_is_mapped(node + 12, leaf_entries, new_wblock))) {
		put_block(bp);
		return(EIO);
	}

	/* Verify cross-leaf physical ownership before any mutation or free. */
	if (extent_depth1_check_siblings(rip, root, root_entries, leaf_index,
	    node, leaf_entries, !(op & WMAP_FREE), new_wblock) != OK) {
		put_block(bp);
		return(EIO);
	}

	leaf_end = previous_end;
	next_key = (u64_t)UINT32_MAX + 1;
	if (leaf_index + 1 < root_entries)
		next_key = extent_get32(root + 12 +
		    (leaf_index + 1) * 12);
	if (leaf_end > next_key) {
		put_block(bp);
		return(EIO);
	}
	if (logical < root_key && leaf_index != 0) {
		put_block(bp);
		return(EIO);
	}
	if ((u64_t)logical >= next_key) {
		put_block(bp);
		return(EIO);
	}

	/* Find an existing extent containing this logical block. */
	for (i = 0; i < leaf_entries; i++) {
		entry = node + 12 + i * 12;
		extent_get(entry, &ex);
		if ((u64_t)logical >= ex.logical &&
		    (u64_t)logical < (u64_t)ex.logical + ex.length)
			break;
	}

	if (op & WMAP_FREE) {
		block64_t block_to_free;

		if (i == leaf_entries) {
			put_block(bp);
			return(OK);
		}
		offset = logical - ex.logical;
		block_to_free = ex.physical + offset;
		if (rip->i_blocks < rip->i_sp->s_sectors_in_block ||
		    (leaf_entries == 1 && ex.length == 1 &&
		    rip->i_blocks - rip->i_sp->s_sectors_in_block <
		    rip->i_sp->s_sectors_in_block)) {
			put_block(bp);
			return(EIO);
		}
		if (offset != 0 && offset != ex.length - 1 &&
		    leaf_entries >= leaf_max) {
			put_block(bp);
			return(EFBIG);
		}

		if (ex.length == 1) {
			memmove(entry, entry + 12,
			    (leaf_entries - i - 1) * 12);
			leaf_entries--;
		} else if (offset == 0) {
			extent_put(entry, ex.logical + 1, ex.physical + 1,
			    ex.length - 1, ex.unwritten);
		} else if (offset == ex.length - 1) {
			extent_put(entry, ex.logical, ex.physical,
			    ex.length - 1, ex.unwritten);
		} else {
			memmove(entry + 24, entry + 12,
			    (leaf_entries - i - 1) * 12);
			extent_put(entry, ex.logical, ex.physical, offset,
			    ex.unwritten);
			extent_put(entry + 12, logical + 1, block_to_free + 1,
			    ex.length - offset - 1, ex.unwritten);
			leaf_entries++;
		}
		extent_put16(node + 2, leaf_entries);
		free_leaf = (leaf_entries == 0);
		if (!free_leaf && i == 0) {
			extent_get(node + 12, &ex);
			extent_put32(index_entry, ex.logical);
		}
		if (!free_leaf)
			lmfs_markdirty(bp);
		put_block(bp);

		free_block(rip->i_sp, block_to_free);
		rip->i_blocks -= rip->i_sp->s_sectors_in_block;
		if (free_leaf) {
			if (rip->i_blocks < rip->i_sp->s_sectors_in_block)
				return(EIO);
			memmove(index_entry, index_entry + 12,
			    (root_entries - leaf_index - 1) * 12);
			root_entries--;
			extent_put16(root + 2, root_entries);
			if (root_entries == 0)
				extent_put16(root + 6, 0);
			free_block(rip->i_sp, leaf_block);
			rip->i_blocks -= rip->i_sp->s_sectors_in_block;
		}
		rip->i_dirt = IN_DIRTY;
		return(OK);
	}

	/* Convert one block of an existing unwritten extent to written. */
	if (i != leaf_entries) {
		block64_t old_physical;

		if (!ex.unwritten) {
			put_block(bp);
			return(EEXIST);
		}
		offset = logical - ex.logical;
		old_physical = ex.physical + offset;
		if (offset == 0 && ex.length == 1) {
			extent_put(entry, logical, old_physical, 1, FALSE);
		} else if (offset == 0) {
			if (leaf_entries >= leaf_max) {
				put_block(bp);
				return(EFBIG);
			}
			memmove(entry + 12, entry,
			    (leaf_entries - i) * 12);
			extent_put(entry, logical, old_physical, 1, FALSE);
			extent_put(entry + 12, logical + 1, old_physical + 1,
			    ex.length - 1, TRUE);
			leaf_entries++;
		} else if (offset == ex.length - 1) {
			if (leaf_entries >= leaf_max) {
				put_block(bp);
				return(EFBIG);
			}
			extent_put(entry, ex.logical, ex.physical,
			    ex.length - 1, TRUE);
			memmove(entry + 24, entry + 12,
			    (leaf_entries - i - 1) * 12);
			extent_put(entry + 12, logical, old_physical, 1, FALSE);
			leaf_entries++;
		} else {
			if (leaf_entries > leaf_max - 2) {
				put_block(bp);
				return(EFBIG);
			}
			memmove(entry + 36, entry + 12,
			    (leaf_entries - i - 1) * 12);
			extent_put(entry, ex.logical, ex.physical, offset, TRUE);
			extent_put(entry + 12, logical, old_physical, 1, FALSE);
			extent_put(entry + 24, logical + 1, old_physical + 1,
			    ex.length - offset - 1, TRUE);
			leaf_entries += 2;
		}
		extent_put16(node + 2, leaf_entries);
		lmfs_markdirty(bp);
		put_block(bp);
		free_block(rip->i_sp, new_wblock);
		rip->i_dirt = IN_DIRTY;
		return(OK);
	}

	/* Insert a newly allocated block into a hole in this leaf. */

	if (!extent_i_blocks_can_add(rip,
	    rip->i_sp->s_sectors_in_block)) {
		put_block(bp);
		return(EFBIG);
	}
	for (insert = 0; insert < leaf_entries; insert++) {
		extent_get(node + 12 + insert * 12, &next);
		if (next.logical > logical)
			break;
	}
	if ((u64_t)logical + 1 > next_key) {
		put_block(bp);
		return(EOPNOTSUPP);
	}

	/* Extend the preceding initialized extent when physically adjacent. */
	if (insert > 0) {
		entry = node + 12 + (insert - 1) * 12;
		extent_get(entry, &previous);
		if (!previous.unwritten &&
		    (u64_t)previous.logical + previous.length == logical &&
		    previous.physical + previous.length == new_wblock &&
		    previous.length < 0x8000) {
			new_length = previous.length + 1;
			if (insert < leaf_entries) {
				extent_get(node + 12 + insert * 12, &next);
				if (!next.unwritten &&
				    (u64_t)logical + 1 == next.logical &&
				    new_wblock + 1 == next.physical &&
				    (u64_t)new_length + next.length <= 0x8000) {
					new_length += next.length;
					memmove(node + 12 + insert * 12,
					    node + 12 + (insert + 1) * 12,
					    (leaf_entries - insert - 1) * 12);
					leaf_entries--;
				}
			}
			extent_put(entry, previous.logical, previous.physical,
			    new_length, FALSE);
			extent_put16(node + 2, leaf_entries);
			lmfs_markdirty(bp);
			put_block(bp);
			rip->i_blocks += rip->i_sp->s_sectors_in_block;
			rip->i_dirt = IN_DIRTY;
			return(OK);
		}
	}

	/* Or prepend the new block to the following initialized extent. */
	if (insert < leaf_entries) {
		entry = node + 12 + insert * 12;
		extent_get(entry, &next);
		if (!next.unwritten &&
		    (u64_t)logical + 1 == next.logical &&
		    new_wblock + 1 == next.physical &&
		    next.length < 0x8000) {
			extent_put(entry, logical, new_wblock,
			    next.length + 1, FALSE);
			if (insert == 0)
				extent_put32(index_entry, logical);
			lmfs_markdirty(bp);
			put_block(bp);
			rip->i_blocks += rip->i_sp->s_sectors_in_block;
			rip->i_dirt = IN_DIRTY;
			return(OK);
		}
	}

	if (leaf_entries >= leaf_max) {
		put_block(bp);
		return(EFBIG);
	}
	entry = node + 12 + insert * 12;
	memmove(entry + 12, entry, (leaf_entries - insert) * 12);
	extent_put(entry, logical, new_wblock, 1, FALSE);
	leaf_entries++;
	extent_put16(node + 2, leaf_entries);
	if (insert == 0)
		extent_put32(index_entry, logical);
	lmfs_markdirty(bp);
	put_block(bp);
	rip->i_blocks += rip->i_sp->s_sectors_in_block;
	rip->i_dirt = IN_DIRTY;
	return(OK);
}

int
ext4_extent_write_map(struct inode *rip, off_t position,
	block64_t new_wblock, int op)
{
	unsigned char *root, *entry;
	struct ext4_extent ex, previous, next;
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

	/*
	 * The inode root may point to depth-one external leaves.  Support
	 * in-place edits in those leaves; deeper trees still need rebalancing.
	 */
	if (extent_get16((unsigned char *)rip->i_block) ==
	    EXT4_EXTENT_MAGIC &&
	    extent_get16((unsigned char *)rip->i_block + 6) == 1)
		return(extent_depth1_write_map(rip, position, new_wblock, op));

	if ((r = extent_root(rip, &root, &entries, &max_entries)) != OK)
		return(r);

	/*
	 * The allocator may hand us a block even when a damaged bitmap says it
	 * is free.  Do not write it, or free it after unwritten conversion, if
	 * it is already referenced by another inline extent in this inode.
	 */
	physical = (u64_t)new_wblock;
	if (!(op & WMAP_FREE) &&
	    (physical <= rip->i_sp->s_first_data_block ||
	    physical >= rip->i_sp->s_blocks_count ||
	    physical > (((u64_t)1 << 48) - 1) ||
	    extent_is_group_metadata(rip->i_sp, physical) ||
	    extent_block_is_mapped(root + 12, entries, physical)))
		return(EIO);

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

		if (i == entries)
			return(OK);	/* A hole needs no work. */

		offset = logical - ex.logical;
		block_to_free = ex.physical + offset;
		if (rip->i_blocks < rip->i_sp->s_sectors_in_block)
			return(EIO);
		/* Splitting a middle extent consumes one additional root slot. */
		if (offset != 0 && offset != ex.length - 1 &&
		    entries >= max_entries)
			return(EFBIG);

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

		if (!ex.unwritten)
			return(EEXIST);
		offset = logical - ex.logical;
		old_physical = ex.physical + offset;
		if (offset == 0 && ex.length == 1) {
			extent_put(entry, logical, old_physical, 1, FALSE);
		} else if (offset == 0) {
			if (entries >= max_entries)
				return(EFBIG);
			memmove(entry + 12, entry,
			    (entries - i) * 12);
			extent_put(entry, logical, old_physical, 1, FALSE);
			extent_put(entry + 12, logical + 1, old_physical + 1,
			    ex.length - 1, TRUE);
			entries++;
		} else if (offset == ex.length - 1) {
			if (entries >= max_entries)
				return(EFBIG);
			extent_put(entry, ex.logical, ex.physical, ex.length - 1,
			    TRUE);
			memmove(entry + 24, entry + 12,
			    (entries - i - 1) * 12);
			extent_put(entry + 12, logical, old_physical, 1, FALSE);
			entries++;
		} else {
			if (entries > max_entries - 2)
				return(EFBIG);
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
		rip->i_dirt = IN_DIRTY;
		return(OK);
	}

	/*
	 * The in-core counter is 64 bits.  Do not retain the old ext2
	 * 32-bit limit here: HUGE_FILE filesystems can store a 48-bit
	 * sector count (or a 48-bit filesystem-block count).
	 */
	if (!extent_i_blocks_can_add(rip,
	    rip->i_sp->s_sectors_in_block))
		return(EFBIG);

	/* Locate the sorted insertion point. */
	for (insert = 0; insert < entries; insert++) {
		extent_get(root + 12 + insert * 12, &next);
		if (next.logical > logical)
			break;
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
			rip->i_dirt = IN_DIRTY;
			return(OK);
		}
	}

	if (entries >= max_entries)
		return(EFBIG);

	/* Insert a one-block initialized extent in logical-block order. */
	entry = root + 12 + insert * 12;
	memmove(entry + 12, entry, (entries - insert) * 12);
	extent_put(entry, logical, physical, 1, FALSE);
	extent_put16(root + 2, entries + 1);
	rip->i_blocks += rip->i_sp->s_sectors_in_block;
	rip->i_dirt = IN_DIRTY;
	return(OK);
}
