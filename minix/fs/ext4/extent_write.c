/*
 * Limited ext4 extent-tree updates.
 *
 * This writer supports inline-root promotion, depth-one leaf allocation,
 * splitting and compaction, plus a guarded depth-two layout whose inode root
 * has one external depth-one index node. It does not split full external
 * index nodes, support multi-entry depth-two roots, or mutate deeper trees.
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
static block64_t extent_inode_xattr_block(struct inode *rip);
static int extent_validate_xattr_block(struct inode *rip, block64_t block);

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
 * Update the depth-zero leaves referenced by a depth-one index node. For
 * ordinary depth-one trees the index is in the inode; for the supported
 * single-root depth-two case it is an external block. The caller supplies
 * the index buffer and its physical block number so alias checks and index
 * persistence work for both layouts.
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
	int check_candidate, block64_t candidate, block64_t root_block)
{
	struct ext4_extent ex, selected_ex, prior;
	struct buf *bp;
	const unsigned char *index_entry, *node;
	block64_t leaf_block;
	u64_t previous_end, next_key;
	u32_t root_key;
	u16_t leaf_entries, leaf_max;
	block64_t xattr_block;
	unsigned int i, j, k;

	xattr_block = extent_inode_xattr_block(rip);
	if (extent_validate_xattr_block(rip, xattr_block) != OK)
		return(EIO);
	index_entry = root + 12 + selected * 12;
	leaf_block = ((u64_t)extent_get16(index_entry + 8) << 32) |
	    extent_get32(index_entry + 4);
	if (leaf_block == root_block || leaf_block == xattr_block ||
	    (xattr_block != 0 && extent_block_is_mapped(selected_node + 12,
	    selected_entries, xattr_block)))
		return(EIO);

	for (i = 0; i < root_entries; i++) {
		if (i == selected)
			continue;

		index_entry = root + 12 + i * 12;
		root_key = extent_get32(index_entry);
		leaf_block = ((u64_t)extent_get16(index_entry + 8) << 32) |
		    extent_get32(index_entry + 4);
		if (leaf_block == root_block || leaf_block == xattr_block)
			return(EIO);
		{
			unsigned int prior_i;
			for (prior_i = 0; prior_i < i; prior_i++) {
				const unsigned char *prior_index =
				    root + 12 + prior_i * 12;
				block64_t prior_leaf =
				    ((u64_t)extent_get16(prior_index + 8) << 32) |
				    extent_get32(prior_index + 4);
				if (prior_leaf == leaf_block)
					return(EIO);
			}
		}
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
		if (leaf_max < 4 ||
		    leaf_max > (rip->i_sp->s_block_size - 12) / 12 ||
		    leaf_entries == 0 || leaf_entries > leaf_max ||
		    (xattr_block != 0 &&
		    extent_block_is_mapped(node + 12, leaf_entries, xattr_block))) {
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
			    (root_block != NO_BLOCK &&
			    extent_ranges_overlap(ex.physical, ex.length,
			    root_block, 1)) ||
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
		if (leaf_block == root_block || leaf_block == xattr_block)
			return(EIO);
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
		if (leaf_max < 4 ||
		    leaf_max > (rip->i_sp->s_block_size - 12) / 12 ||
		    leaf_entries == 0 || leaf_entries > leaf_max ||
		    (xattr_block != 0 &&
		    extent_block_is_mapped(node + 12, leaf_entries, xattr_block))) {
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
			if (other_max < 4 ||
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
 * Promote a full inline root into a depth-one tree.  The existing inline
 * records become the first external leaf; the inode root becomes a one-entry
 * index.  This is the first growth step for files that outgrow the four
 * extents stored in i_block.
 */
static int
extent_promote_root(struct inode *rip, unsigned char *root, u16_t entries,
	u16_t root_max)
{
	struct super_block *sp = rip->i_sp;
	struct buf *bp;
	unsigned char *node;
	block64_t block, xattr_block;
	u16_t leaf_max;
	u32_t first_logical;

	if (entries == 0 || entries > root_max || sp->s_block_size <= 12)
		return(EIO);
	leaf_max = (u16_t)((sp->s_block_size - 12) / 12);
	if (leaf_max < entries || sp->s_sectors_in_block == 0 ||
	    !extent_i_blocks_can_add(rip, sp->s_sectors_in_block))
		return(EFBIG);

	xattr_block = extent_inode_xattr_block(rip);
	if (extent_validate_xattr_block(rip, xattr_block) != OK ||
	    (xattr_block != 0 &&
	    extent_block_is_mapped(root + 12, entries, xattr_block)))
		return(EIO);

	first_logical = extent_get32(root + 12);
	block = alloc_block(rip, rip->i_bsearch);
	if (block == NO_BLOCK)
		return(ENOSPC);
	if (block <= sp->s_first_data_block ||
	    block >= sp->s_blocks_count ||
	    block > (((u64_t)1 << 48) - 1) ||
	    extent_is_group_metadata(sp, block) || block == xattr_block ||
	    extent_block_is_mapped(root + 12, entries, block)) {
		free_block(sp, block);
		return(EIO);
	}

	bp = get_block(rip->i_dev, block, NO_READ);
	if (bp == NULL) {
		free_block(sp, block);
		return(EIO);
	}
	node = (unsigned char *)b_data(bp);
	memset(node, 0, sp->s_block_size);
	extent_put16(node, EXT4_EXTENT_MAGIC);
	extent_put16(node + 2, entries);
	extent_put16(node + 4, leaf_max);
	extent_put16(node + 6, 0);
	memcpy(node + 12, root + 12, entries * 12);
	lmfs_markdirty(bp);
	put_block(bp);

	/* Replace the inline extent array with a one-entry external index. */
	memset(root, 0, sizeof(rip->i_block));
	extent_put16(root, EXT4_EXTENT_MAGIC);
	extent_put16(root + 2, 1);
	extent_put16(root + 4, root_max);
	extent_put16(root + 6, 1);
	extent_put32(root + 12, first_logical);
	extent_put32(root + 16, (u32_t)block);
	extent_put16(root + 20, (u16_t)(block >> 32));

	rip->i_blocks += sp->s_sectors_in_block;
	rip->i_dirt = IN_DIRTY;
	return(OK);
}

/*
 * Split one depth-zero leaf when an edit needs more records than it can hold.
 * The new leaf receives the upper half, and a new inode-root index entry is
 * inserted immediately after the old one.  Deeper index-node splitting is
 * intentionally left to a later change.
 */
static int
extent_depth1_split_leaf(struct inode *rip, unsigned char *root,
	u16_t *root_entriesp, u16_t root_max, unsigned int *leaf_indexp,
	u32_t logical, struct buf **bpp, unsigned char **nodep,
	u16_t *leaf_entriesp, u16_t leaf_max,
	unsigned char **index_entryp, block64_t *leaf_blockp,
	block64_t root_block)
{
	struct super_block *sp = rip->i_sp;
	struct buf *new_bp, *old_bp;
	unsigned char *node, *new_node, *index_entry, *new_index;
	block64_t new_block, xattr_block;
	u16_t root_entries, leaf_entries, split_entries, right_entries;
	u32_t split_key;
	unsigned int leaf_index;
	int r, use_right;

	root_entries = *root_entriesp;
	leaf_entries = *leaf_entriesp;
	leaf_index = *leaf_indexp;
	old_bp = *bpp;
	node = *nodep;

	if (root_entries >= root_max || leaf_entries < 4 || leaf_max < 4 ||
	    leaf_entries > leaf_max || sp->s_sectors_in_block == 0)
		return(EFBIG);
	if (!extent_i_blocks_can_add(rip, sp->s_sectors_in_block))
		return(EFBIG);

	xattr_block = extent_inode_xattr_block(rip);
	if (extent_validate_xattr_block(rip, xattr_block) != OK ||
	    (xattr_block != 0 &&
	    extent_block_is_mapped(node + 12, leaf_entries, xattr_block)))
		return(EIO);

	new_block = alloc_block(rip, *leaf_blockp + 1);
	if (new_block == NO_BLOCK)
		return(ENOSPC);
	if (new_block <= sp->s_first_data_block ||
	    new_block >= sp->s_blocks_count ||
	    new_block > (((u64_t)1 << 48) - 1) ||
	    new_block > (((u64_t)~0 >> 1) / sp->s_block_size) ||
	    extent_is_group_metadata(sp, new_block) ||
	    new_block == root_block ||
	    extent_overlaps_tree_node(root, root_entries, new_block, 1) ||
	    new_block == xattr_block ||
	    extent_block_is_mapped(node + 12, leaf_entries, new_block)) {
		free_block(sp, new_block);
		return(EIO);
	}

	r = extent_depth1_check_siblings(rip, root, root_entries, leaf_index,
	    node, leaf_entries, TRUE, new_block, root_block);
	if (r != OK) {
		free_block(sp, new_block);
		return(EIO);
	}

	new_bp = get_block(rip->i_dev, new_block, NO_READ);
	if (new_bp == NULL) {
		free_block(sp, new_block);
		return(EIO);
	}
	new_node = (unsigned char *)b_data(new_bp);
	split_entries = leaf_entries / 2;
	right_entries = leaf_entries - split_entries;
	split_key = extent_get32(node + 12 + split_entries * 12);

	memset(new_node, 0, sp->s_block_size);
	extent_put16(new_node, EXT4_EXTENT_MAGIC);
	extent_put16(new_node + 2, right_entries);
	extent_put16(new_node + 4, leaf_max);
	extent_put16(new_node + 6, 0);
	memcpy(new_node + 12, node + 12 + split_entries * 12,
	    right_entries * 12);

	/* Insert the right-hand leaf in the inode's sorted index array. */
	memmove(root + 12 + (leaf_index + 2) * 12,
	    root + 12 + (leaf_index + 1) * 12,
	    (root_entries - leaf_index - 1) * 12);
	new_index = root + 12 + (leaf_index + 1) * 12;
	extent_put32(new_index, split_key);
	extent_put32(new_index + 4, (u32_t)new_block);
	extent_put16(new_index + 8, (u16_t)(new_block >> 32));
	extent_put16(root + 2, root_entries + 1);

	memset(node + 12 + split_entries * 12, 0,
	    right_entries * 12);
	extent_put16(node + 2, split_entries);
	lmfs_markdirty(old_bp);
	lmfs_markdirty(new_bp);

	rip->i_blocks += sp->s_sectors_in_block;
	rip->i_dirt = IN_DIRTY;
	*root_entriesp = root_entries + 1;

	use_right = (logical >= split_key);
	if (use_right) {
		put_block(old_bp);
		*bpp = new_bp;
		*nodep = new_node;
		*leaf_indexp = leaf_index + 1;
		*leaf_blockp = new_block;
		*leaf_entriesp = right_entries;
	} else {
		put_block(new_bp);
		*leaf_entriesp = split_entries;
	}
	index_entry = root + 12 + (*leaf_indexp) * 12;
	*index_entryp = index_entry;
	return(OK);
}


/*
 * Merge two adjacent depth-zero leaves when their records fit in one block.
 * selected_is_right means the caller's current leaf is the right-hand leaf;
 * in that case the surviving left buffer replaces the caller's buffer.
 *
 * Returns one when a merge was performed, zero when the pair does not fit,
 * and a negative errno on malformed metadata or I/O failure.
 */
static int
extent_depth1_try_merge_pair(struct inode *rip, unsigned char *root,
	u16_t *root_entriesp, u16_t root_max, unsigned int *selectedp,
	int selected_is_right, struct buf **bpp, unsigned char **nodep,
	u16_t *entriesp, u16_t leaf_max, unsigned char **index_entryp,
	block64_t *leaf_blockp)
{
	struct super_block *sp = rip->i_sp;
	struct buf *other_bp, *left_bp, *right_bp;
	unsigned char *left_node, *right_node;
	const unsigned char *left_index, *right_index;
	struct ext4_extent first, last, right_first;
	block64_t left_block, right_block, xattr_block;
	u16_t root_entries, left_count, right_count, left_max, right_max;
	u16_t new_count, right_start;
	u32_t left_key, right_key;
	u64_t left_end;
	unsigned int left_index_num, right_index_num, selected;
	int coalesce, limit;

	selected = *selectedp;
	root_entries = *root_entriesp;
	if (root_entries > root_max)
		return(EIO);
	if (root_entries < 2 || selected >= root_entries)
		return(0);
	if (selected_is_right) {
		if (selected == 0)
			return(0);
		left_index_num = selected - 1;
		right_index_num = selected;
	} else {
		if (selected + 1 >= root_entries)
			return(0);
		left_index_num = selected;
		right_index_num = selected + 1;
	}
	left_index = root + 12 + left_index_num * 12;
	right_index = root + 12 + right_index_num * 12;
	left_key = extent_get32(left_index);
	right_key = extent_get32(right_index);
	left_block = ((u64_t)extent_get16(left_index + 8) << 32) |
	    extent_get32(left_index + 4);
	right_block = ((u64_t)extent_get16(right_index + 8) << 32) |
	    extent_get32(right_index + 4);
	xattr_block = extent_inode_xattr_block(rip);
	if (left_block == right_block ||
	    left_block <= sp->s_first_data_block ||
	    right_block <= sp->s_first_data_block ||
	    left_block >= sp->s_blocks_count ||
	    right_block >= sp->s_blocks_count ||
	    extent_is_group_metadata(sp, left_block) ||
	    extent_is_group_metadata(sp, right_block) ||
	    (left_block == xattr_block) ||
	    (right_block == xattr_block))
		return(EIO);

	if (selected_is_right) {
		right_bp = *bpp;
		other_bp = get_block(rip->i_dev, left_block, NORMAL);
		if (other_bp == NULL)
			return(EIO);
		left_bp = other_bp;
	} else {
		left_bp = *bpp;
		other_bp = get_block(rip->i_dev, right_block, NORMAL);
		if (other_bp == NULL)
			return(EIO);
		right_bp = other_bp;
	}
	left_node = (unsigned char *)b_data(left_bp);
	right_node = (unsigned char *)b_data(right_bp);

	if (extent_get16(left_node) != EXT4_EXTENT_MAGIC ||
	    extent_get16(left_node + 6) != 0 ||
	    extent_get16(right_node) != EXT4_EXTENT_MAGIC ||
	    extent_get16(right_node + 6) != 0) {
		put_block(other_bp);
		return(EIO);
	}
	left_count = extent_get16(left_node + 2);
	right_count = extent_get16(right_node + 2);
	left_max = extent_get16(left_node + 4);
	right_max = extent_get16(right_node + 4);
	if (left_max < 4 || right_max < 4 ||
	    left_max > (sp->s_block_size - 12) / 12 ||
	    right_max > (sp->s_block_size - 12) / 12 ||
	    left_count == 0 || right_count == 0 ||
	    left_count > left_max || right_count > right_max) {
		put_block(other_bp);
		return(EIO);
	}
	extent_get(left_node + 12, &first);
	extent_get(right_node + 12, &right_first);
	extent_get(left_node + 12 + (left_count - 1) * 12, &last);
	if (first.logical != left_key || right_first.logical != right_key) {
		put_block(other_bp);
		return(EIO);
	}
	left_end = (u64_t)last.logical + last.length;
	if (left_end > right_first.logical) {
		put_block(other_bp);
		return(EIO);
	}

	coalesce = FALSE;
	right_start = 0;
	if (last.unwritten == right_first.unwritten &&
	    left_end == right_first.logical &&
	    last.physical + last.length == right_first.physical) {
		limit = last.unwritten ? 0x7fff : 0x8000;
		if ((u64_t)last.length + right_first.length <= (u64_t)limit) {
			coalesce = TRUE;
			right_start = 1;
		}
	}
	new_count = left_count + right_count - (coalesce ? 1 : 0);
	if (new_count > leaf_max || new_count > left_max) {
		put_block(other_bp);
		return(0);
	}
	if (sp->s_sectors_in_block == 0 ||
	    rip->i_blocks < sp->s_sectors_in_block) {
		put_block(other_bp);
		return(EIO);
	}

	if (coalesce) {
		extent_put(left_node + 12 + (left_count - 1) * 12,
		    last.logical, last.physical,
		    last.length + right_first.length, last.unwritten);
	}
	if (right_count > right_start) {
		memcpy(left_node + 12 + left_count * 12,
		    right_node + 12 + right_start * 12,
		    (right_count - right_start) * 12);
	}
	extent_put16(left_node + 2, new_count);
	memset(left_node + 12 + new_count * 12, 0,
	    sp->s_block_size - 12 - new_count * 12);
	lmfs_markdirty(left_bp);

	/* Remove the right-hand index record and release its external leaf. */
	memmove(root + 12 + right_index_num * 12,
	    root + 12 + (right_index_num + 1) * 12,
	    (root_entries - right_index_num - 1) * 12);
	memset(root + 12 + (root_entries - 1) * 12, 0, 12);
	root_entries--;
	extent_put16(root + 2, root_entries);
	rip->i_blocks -= sp->s_sectors_in_block;
	rip->i_dirt = IN_DIRTY;

	if (selected_is_right) {
		/* The current buffer was the removed right-hand leaf. */
		put_block(right_bp);
		*bpp = left_bp;
		*nodep = left_node;
		*selectedp = left_index_num;
		*leaf_blockp = left_block;
		*entriesp = new_count;
	} else {
		put_block(other_bp);
		*selectedp = left_index_num;
		*entriesp = new_count;
	}
	free_block(sp, right_block);
	*root_entriesp = root_entries;
	*index_entryp = root + 12 + left_index_num * 12;
	return(1);
}

/*
 * Compact a depth-one tree around the leaf just changed. Adjacent leaves that
 * fit are folded together; a one-leaf tree small enough for the inode is
 * demoted back to an inline extent root.
 */
static int
extent_depth1_compact(struct inode *rip, unsigned char *root,
	u16_t *root_entriesp, u16_t root_max, unsigned int *selectedp,
	struct buf **bpp, unsigned char **nodep, u16_t *entriesp,
	u16_t leaf_max, unsigned char **index_entryp, block64_t *leaf_blockp,
	int allow_demote)
{
	int r;

	while (*root_entriesp > 1) {
		r = extent_depth1_try_merge_pair(rip, root, root_entriesp,
		    root_max, selectedp, FALSE, bpp, nodep, entriesp,
		    leaf_max, index_entryp, leaf_blockp);
		if (r != 0 && r != 1)
			break;
		if (r == 0 && *selectedp > 0) {
			r = extent_depth1_try_merge_pair(rip, root,
			    root_entriesp, root_max, selectedp, TRUE, bpp,
			    nodep, entriesp, leaf_max, index_entryp,
			    leaf_blockp);
			if (r != 0 && r != 1)
				break;
		}
		if (r == 0)
			break;
	}

	if (allow_demote && *root_entriesp == 1 && *selectedp == 0 &&
	    *entriesp != 0 && *entriesp <= root_max) {
		struct super_block *sp = rip->i_sp;
		u16_t count = *entriesp;
		block64_t block = *leaf_blockp;

		if (sp->s_sectors_in_block == 0 ||
		    rip->i_blocks < sp->s_sectors_in_block ||
		    block <= sp->s_first_data_block ||
		    block >= sp->s_blocks_count ||
		    extent_is_group_metadata(sp, block))
			return(OK); /* Keep the valid tree; compaction is optional. */

		memset(root, 0, sizeof(rip->i_block));
		extent_put16(root, EXT4_EXTENT_MAGIC);
		extent_put16(root + 2, count);
		extent_put16(root + 4, root_max);
		extent_put16(root + 6, 0);
		memcpy(root + 12, *nodep + 12, count * 12);
		put_block(*bpp);
		*bpp = NULL;
		free_block(sp, block);
		rip->i_blocks -= sp->s_sectors_in_block;
		rip->i_dirt = IN_DIRTY;
		*root_entriesp = count;
		*entriesp = count;
		*leaf_blockp = NO_BLOCK;
		*index_entryp = NULL;
		*nodep = root;
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
static int extent_depth2_preflight_single_root(struct inode *,
	block64_t *, int);

int
ext4_extent_can_truncate(struct inode *rip)
{
	unsigned char *root, *index_entry, *node;
	struct ext4_extent ex, prior;
	struct super_block *sp;
	struct buf *bp;
	block64_t child, leaf_block, prior_child, xattr_block;
	u64_t file_blocks, previous_end, next_key, logical_end, owned_blocks;
	u32_t root_key, previous_key;
	u16_t root_entries, root_max, depth, leaf_entries, leaf_max;
	unsigned int i, j, k;
	int r;

	sp = rip->i_sp;
	if (!sp->s_is_ext4)
		return(OK);

	xattr_block = extent_inode_xattr_block(rip);
	if (extent_validate_xattr_block(rip, xattr_block) != OK)
		return(EIO);
	if (!(rip->i_flags & EXT4_EXTENTS_FL)) {
		/*
		 * A legacy indirect tree needs a separate ownership walk to prove
		 * that its xattr block does not alias file data or indirect nodes.
		 * Special files have no data mapping to collide with.
		 */
		if (xattr_block != 0 &&
		    (rip->i_mode & I_TYPE) != I_CHAR_SPECIAL &&
		    (rip->i_mode & I_TYPE) != I_BLOCK_SPECIAL)
			return(EOPNOTSUPP);
		return(OK);
	}

	root = (unsigned char *)rip->i_block;
	if (extent_get16(root) != EXT4_EXTENT_MAGIC)
		return(EIO);
	depth = extent_get16(root + 6);
	if (depth == 2) {
		block64_t parent_block;
		return(extent_depth2_preflight_single_root(rip, &parent_block,
		    TRUE));
	}
	if (depth > 2)
		return(EOPNOTSUPP);

	root_entries = extent_get16(root + 2);
	root_max = extent_get16(root + 4);
	if (root_max == 0 ||
	    root_max > (sizeof(rip->i_block) - 12) / 12 ||
	    root_entries > root_max || sp->s_block_size == 0 ||
	    rip->i_size < 0)
		return(EIO);

	file_blocks = (u64_t)rip->i_size / sp->s_block_size;
	if ((u64_t)rip->i_size % sp->s_block_size != 0)
		file_blocks++;
	if (file_blocks > (u64_t)UINT32_MAX + 1)
		return(EOPNOTSUPP);

	/*
	 * Inline roots have no external nodes. Reuse the normal root validator
	 * and confirm every allocation is covered by the range truncate_inode()
	 * will visit.
	 */
	if (depth == 0) {
		r = extent_root(rip, &root, &root_entries, &root_max);
		if (r != OK)
			return(r);
		owned_blocks = 0;
		if (xattr_block != 0 &&
		    extent_block_is_mapped(root + 12, root_entries,
		    xattr_block))
			return(EIO);
		for (i = 0; i < root_entries; i++) {
			extent_get(root + 12 + i * 12, &ex);
			logical_end = (u64_t)ex.logical + ex.length;
			if (logical_end > file_blocks)
				return(EOPNOTSUPP);
			if (owned_blocks > ~(u64_t)0 - ex.length)
				return(EIO);
			owned_blocks += ex.length;
		}
		if (sp->s_sectors_in_block == 0 ||
		    owned_blocks > ~(u64_t)0 / sp->s_sectors_in_block ||
		    owned_blocks * sp->s_sectors_in_block != rip->i_blocks)
			return(EIO);
		return(OK);
	}

	if (root_entries == 0)
		return(EIO);

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
		    child == xattr_block ||
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

		if (xattr_block != 0 &&
		    extent_block_is_mapped(node + 12, leaf_entries, xattr_block)) {
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
		    root_entries, 0, node, leaf_entries, FALSE, 0, NO_BLOCK) != OK) {
			put_block(bp);
			return(EIO);
		}
		put_block(bp);
	}

	/* No unreferenced data or extent-node blocks may be lost on final unlink. */
	if (sp->s_sectors_in_block == 0 ||
	    owned_blocks > ~(u64_t)0 / sp->s_sectors_in_block ||
	    owned_blocks * sp->s_sectors_in_block != rip->i_blocks)
		return(EIO);

	return(OK);
}

/*
 * Return the external xattr block pointer stored in the inode. The high
 * 16 bits occupy l_i_file_acl_high when the 64-bit feature is present.
 */
static block64_t
extent_inode_xattr_block(struct inode *rip)
{
	block64_t block;

	block = (block64_t)(u32_t)rip->i_file_acl;
	if (HAS_INCOMPAT_FEATURE(rip->i_sp, INCOMPAT_64BIT))
		block |= (block64_t)conv2(le_CPU,
		    rip->osd2.linux2.i_pad1) << 32;
	return(block);
}

/*
 * Validate the xattr block header before it is ever detached from an inode.
 * ext4 stores one external xattr block per inode; identical blocks may be
 * shared, so h_refcount must be in the supported range.
 */
static int
extent_validate_xattr_block(struct inode *rip, block64_t block)
{
	struct super_block *sp = rip->i_sp;
	struct buf *bp;
	const unsigned char *data;
	u32_t magic, refcount, blocks;

	if (block == 0)
		return(OK);
	if (!HAS_COMPAT_FEATURE(sp, COMPAT_EXT_ATTR))
		return(EIO);
	if (sp->s_block_size == 0 ||
	    block <= sp->s_first_data_block ||
	    block >= sp->s_blocks_count ||
	    block > (((u64_t)~0 >> 1) / sp->s_block_size) ||
	    extent_is_group_metadata(sp, block))
		return(EIO);

	bp = get_block(rip->i_dev, block, NORMAL);
	if (bp == NULL)
		return(EIO);
	data = (const unsigned char *)b_data(bp);
	magic = extent_get32(data);
	refcount = extent_get32(data + 4);
	blocks = extent_get32(data + 8);
	put_block(bp);

	if (magic != EXT4_XATTR_MAGIC || refcount == 0 ||
	    refcount > EXT4_XATTR_REFCOUNT_MAX || blocks != 1)
		return(EIO);

	if (HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_METADATA_CSUM) &&
	    !ext4_verify_xattr_block_csum(sp, rip->i_dev, block))
		return(EIO);

	return(OK);
}

/*
 * Drop this inode's reference to its external xattr block. Shared blocks are
 * retained with a decremented reference count; an unshared block is returned
 * to the allocation bitmap. Xattr blocks are quota/accounting objects and are
 * not included in the inode's i_blocks extent-data counter.
 */
int
ext4_release_xattr_block(struct inode *rip)
{
	struct super_block *sp = rip->i_sp;
	struct buf *bp;
	unsigned char *data;
	block64_t block;
	u32_t magic, refcount, blocks;

	if (!sp->s_is_ext4)
		return(OK);
	block = extent_inode_xattr_block(rip);
	if (block == 0)
		return(OK);
	if (!HAS_COMPAT_FEATURE(sp, COMPAT_EXT_ATTR))
		return(EIO);
	if (sp->s_rd_only)
		return(EROFS);
	/* Writable metadata_csum is not supported by the mount path. */
	if (HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_METADATA_CSUM))
		return(EOPNOTSUPP);
	if (sp->s_block_size == 0 ||
	    block <= sp->s_first_data_block ||
	    block >= sp->s_blocks_count ||
	    block > (((u64_t)~0 >> 1) / sp->s_block_size) ||
	    extent_is_group_metadata(sp, block))
		return(EIO);

	bp = get_block(rip->i_dev, block, NORMAL);
	if (bp == NULL)
		return(EIO);
	data = (unsigned char *)b_data(bp);
	magic = extent_get32(data);
	refcount = extent_get32(data + 4);
	blocks = extent_get32(data + 8);
	if (magic != EXT4_XATTR_MAGIC || refcount == 0 ||
	    refcount > EXT4_XATTR_REFCOUNT_MAX || blocks != 1) {
		put_block(bp);
		return(EIO);
	}

	/* Clear both pointer halves before the inode is written back. */
	rip->i_file_acl = 0;
	if (HAS_INCOMPAT_FEATURE(sp, INCOMPAT_64BIT))
		rip->osd2.linux2.i_pad1 = 0;
	rip->i_dirt = IN_DIRTY;

	if (refcount > 1) {
		extent_put32(data + 4, refcount - 1);
		lmfs_markdirty(bp);
		put_block(bp);
		return(OK);
	}

	put_block(bp);
	free_block(sp, block);
	return(OK);
}

static int
extent_depth1_write_map_root(struct inode *rip, off_t position,
	block64_t new_wblock, int op, unsigned char *root,
	int external_root, block64_t root_block)
{
	unsigned char *index_entry, *node, *entry;
	struct ext4_extent ex, previous, next;
	struct buf *bp;
	block64_t leaf_block, child, xattr_block;
	u64_t logical64, previous_end, leaf_end, next_key;
	u32_t logical, offset, new_length, root_key, child_lo;
	u16_t root_entries, root_max, depth, leaf_entries, leaf_max;
	u16_t child_hi;
	unsigned int leaf_index, i, insert;
	int free_leaf, r;

	if (rip->i_sp->s_rd_only)
		return(EROFS);
	if (position < 0)
		return(EINVAL);

	logical64 = (u64_t)position / rip->i_sp->s_block_size;
	if (logical64 > UINT32_MAX)
		return(EFBIG);
	logical = (u32_t)logical64;

	if (extent_get16(root) != EXT4_EXTENT_MAGIC)
		return(EIO);
	root_entries = extent_get16(root + 2);
	root_max = extent_get16(root + 4);
	depth = extent_get16(root + 6);
	if (depth != 1 || root_max == 0 ||
	    (!external_root &&
	    root_max > (sizeof(rip->i_block) - 12) / 12) ||
	    (external_root &&
	    root_max > (rip->i_sp->s_block_size - 12) / 12) ||
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
		    child == root_block ||
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
	xattr_block = extent_inode_xattr_block(rip);
	if (extent_validate_xattr_block(rip, xattr_block) != OK ||
	    leaf_block == xattr_block)
		return(EIO);
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
	if (leaf_max < 4 ||
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
		    (root_block != NO_BLOCK &&
		    extent_ranges_overlap(ex.physical, ex.length, root_block, 1)) ||
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
	if (xattr_block != 0 &&
	    extent_block_is_mapped(node + 12, leaf_entries, xattr_block)) {
		put_block(bp);
		return(EIO);
	}
	if (!(op & WMAP_FREE) &&
	    (new_wblock <= rip->i_sp->s_first_data_block ||
	    new_wblock >= rip->i_sp->s_blocks_count ||
	    new_wblock > (((u64_t)1 << 48) - 1) ||
	    extent_is_group_metadata(rip->i_sp, new_wblock) ||
	    new_wblock == root_block ||
	    extent_overlaps_tree_node(root, root_entries, new_wblock, 1) ||
	    new_wblock == xattr_block ||
	    extent_block_is_mapped(node + 12, leaf_entries, new_wblock))) {
		put_block(bp);
		return(EIO);
	}

	/* Verify cross-leaf physical ownership before any mutation or free. */
	if (extent_depth1_check_siblings(rip, root, root_entries, leaf_index,
	    node, leaf_entries, !(op & WMAP_FREE), new_wblock,
	    root_block) != OK) {
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
			r = extent_depth1_split_leaf(rip, root, &root_entries,
			    root_max, &leaf_index, logical, &bp, &node,
			    &leaf_entries, leaf_max, &index_entry, &leaf_block,
			    root_block);
			if (r != OK) {
				put_block(bp);
				return(r);
			}
			/* The original extent moved intact to one of the leaves. */
			for (i = 0; i < leaf_entries; i++) {
				entry = node + 12 + i * 12;
				extent_get(entry, &ex);
				if ((u64_t)logical >= ex.logical &&
				    (u64_t)logical < (u64_t)ex.logical + ex.length)
					break;
			}
			if (i == leaf_entries) {
				put_block(bp);
				return(EIO);
			}
			offset = logical - ex.logical;
			block_to_free = ex.physical + offset;
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
		if (!free_leaf) {
			r = extent_depth1_compact(rip, root, &root_entries,
			    root_max, &leaf_index, &bp, &node, &leaf_entries,
			    leaf_max, &index_entry, &leaf_block, !external_root);
			if (r != OK) {
				if (bp != NULL)
					put_block(bp);
				return(r);
			}
		}
		if (bp != NULL) {
			lmfs_markdirty(bp);
			put_block(bp);
		}

		free_block(rip->i_sp, block_to_free);
		rip->i_blocks -= rip->i_sp->s_sectors_in_block;
		if (free_leaf) {
			if (rip->i_blocks < rip->i_sp->s_sectors_in_block)
				return(EIO);
			memmove(index_entry, index_entry + 12,
			    (root_entries - leaf_index - 1) * 12);
			root_entries--;
			extent_put16(root + 2, root_entries);
			if (root_entries == 0 && !external_root)
				extent_put16(root + 6, 0);
			free_block(rip->i_sp, leaf_block);
			rip->i_blocks -= rip->i_sp->s_sectors_in_block;

			/*
			 * Removing an empty leaf can leave one small leaf or two
			 * newly adjacent leaves. Re-read a surviving leaf and run
			 * the same optional compaction/demotion pass.
			 */
			if (root_entries != 0) {
				struct buf *keep_bp;
				unsigned char *keep_node;
				u16_t keep_entries, keep_max;
				block64_t keep_block;

				if (leaf_index >= root_entries)
					leaf_index = root_entries - 1;
				index_entry = root + 12 + leaf_index * 12;
				keep_block =
				    ((u64_t)extent_get16(index_entry + 8) << 32) |
				    extent_get32(index_entry + 4);
				if (keep_block > rip->i_sp->s_first_data_block &&
				    keep_block < rip->i_sp->s_blocks_count &&
				    keep_block != extent_inode_xattr_block(rip) &&
				    !extent_is_group_metadata(rip->i_sp, keep_block) &&
				    (keep_bp = get_block(rip->i_dev, keep_block,
				    NORMAL)) != NULL) {
					keep_node = (unsigned char *)b_data(keep_bp);
					keep_entries = extent_get16(keep_node + 2);
					keep_max = extent_get16(keep_node + 4);
					if (extent_get16(keep_node) == EXT4_EXTENT_MAGIC &&
					    extent_get16(keep_node + 6) == 0 &&
					    keep_max >= 4 &&
					    keep_max <= (rip->i_sp->s_block_size - 12) / 12 &&
					    keep_entries != 0 && keep_entries <= keep_max) {
						bp = keep_bp;
						node = keep_node;
						leaf_entries = keep_entries;
						leaf_max = keep_max;
						leaf_block = keep_block;
						r = extent_depth1_compact(rip, root,
						    &root_entries, root_max, &leaf_index,
						    &bp, &node, &leaf_entries, leaf_max,
						    &index_entry, &leaf_block, !external_root);
						if (r == OK && bp != NULL)
							put_block(bp);
						else if (bp != NULL)
							put_block(bp);
					} else {
						put_block(keep_bp);
					}
				}
			}
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
		if (((offset == 0 || offset == ex.length - 1) &&
		    ex.length > 1 && leaf_entries >= leaf_max) ||
		    (offset != 0 && offset != ex.length - 1 &&
		    leaf_entries > leaf_max - 2)) {
			r = extent_depth1_split_leaf(rip, root, &root_entries,
			    root_max, &leaf_index, logical, &bp, &node,
			    &leaf_entries, leaf_max, &index_entry, &leaf_block,
			    root_block);
			if (r != OK) {
				put_block(bp);
				return(r);
			}
			for (i = 0; i < leaf_entries; i++) {
				entry = node + 12 + i * 12;
				extent_get(entry, &ex);
				if ((u64_t)logical >= ex.logical &&
				    (u64_t)logical < (u64_t)ex.logical + ex.length)
					break;
			}
			if (i == leaf_entries) {
				put_block(bp);
				return(EIO);
			}
			offset = logical - ex.logical;
		}
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
		r = extent_depth1_compact(rip, root, &root_entries,
		    root_max, &leaf_index, &bp, &node, &leaf_entries,
		    leaf_max, &index_entry, &leaf_block, !external_root);
		if (r != OK) {
			if (bp != NULL)
				put_block(bp);
			free_block(rip->i_sp, new_wblock);
			return(r);
		}
		if (bp != NULL) {
			lmfs_markdirty(bp);
			put_block(bp);
		}
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
			r = extent_depth1_compact(rip, root, &root_entries,
			    root_max, &leaf_index, &bp, &node, &leaf_entries,
			    leaf_max, &index_entry, &leaf_block, !external_root);
			if (r != OK) {
				if (bp != NULL)
					put_block(bp);
				return(r);
			}
			if (bp != NULL) {
				lmfs_markdirty(bp);
				put_block(bp);
			}
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
			r = extent_depth1_compact(rip, root, &root_entries,
			    root_max, &leaf_index, &bp, &node, &leaf_entries,
			    leaf_max, &index_entry, &leaf_block, !external_root);
			if (r != OK) {
				if (bp != NULL)
					put_block(bp);
				return(r);
			}
			if (bp != NULL) {
				lmfs_markdirty(bp);
				put_block(bp);
			}
			rip->i_blocks += rip->i_sp->s_sectors_in_block;
			rip->i_dirt = IN_DIRTY;
			return(OK);
		}
	}

	if (leaf_entries >= leaf_max) {
		r = extent_depth1_split_leaf(rip, root, &root_entries,
		    root_max, &leaf_index, logical, &bp, &node,
		    &leaf_entries, leaf_max, &index_entry, &leaf_block,
		    root_block);
		if (r != OK) {
			put_block(bp);
			return(r);
		}
		/* Recompute the insertion offset in the selected half. */
		for (insert = 0; insert < leaf_entries; insert++) {
			extent_get(node + 12 + insert * 12, &next);
			if (next.logical > logical)
				break;
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
	r = extent_depth1_compact(rip, root, &root_entries, root_max,
	    &leaf_index, &bp, &node, &leaf_entries, leaf_max,
	    &index_entry, &leaf_block, !external_root);
	if (r != OK) {
		if (bp != NULL)
			put_block(bp);
		return(r);
	}
	if (bp != NULL) {
		lmfs_markdirty(bp);
		put_block(bp);
	}
	rip->i_blocks += rip->i_sp->s_sectors_in_block;
	rip->i_dirt = IN_DIRTY;
	return(OK);
}



/*
 * Validate the currently supported depth-two layout before modifying it.
 * The inode root must have exactly one child index node. This keeps every
 * leaf covered by the existing depth-one sibling/alias validator while
 * allowing that index node to contain the full block-sized fanout.
 */
static int
extent_depth2_preflight_single_root(struct inode *rip,
	block64_t *parent_blockp, int for_truncate)
{
	struct super_block *sp = rip->i_sp;
	unsigned char *root, *parent_node, *leaf_node;
	struct buf *parent_bp, *leaf_bp, *first_bp;
	struct ext4_extent ex, prior;
	const unsigned char *root_index, *leaf_index;
	block64_t parent_block, leaf_block, xattr_block;
	u64_t owned_blocks, previous_end, next_key, file_blocks = 0;
	u32_t root_key, leaf_key;
	u16_t root_entries, root_max, parent_entries, parent_max;
	u16_t leaf_entries, leaf_max;
	unsigned int i, j, k;

	root = (unsigned char *)rip->i_block;
	if (extent_get16(root) != EXT4_EXTENT_MAGIC ||
	    extent_get16(root + 6) != 2)
		return(EIO);
	root_entries = extent_get16(root + 2);
	root_max = extent_get16(root + 4);
	if (root_max == 0 ||
	    root_max > (sizeof(rip->i_block) - 12) / 12 ||
	    root_entries == 0 || root_entries > root_max)
		return(EIO);
	/* Multiple top-level subtrees need cross-parent alias validation. */
	if (root_entries != 1)
		return(EOPNOTSUPP);
	if (sp->s_block_size <= 12 || sp->s_sectors_in_block == 0)
		return(EIO);
	if (for_truncate) {
		if (rip->i_size < 0)
			return(EIO);
		file_blocks = (u64_t)rip->i_size / sp->s_block_size;
		if ((u64_t)rip->i_size % sp->s_block_size != 0)
			file_blocks++;
		if (file_blocks > (u64_t)UINT32_MAX + 1)
			return(EOPNOTSUPP);
	}

	xattr_block = extent_inode_xattr_block(rip);
	if (extent_validate_xattr_block(rip, xattr_block) != OK)
		return(EIO);
	root_index = root + 12;
	root_key = extent_get32(root_index);
	parent_block = ((u64_t)extent_get16(root_index + 8) << 32) |
	    extent_get32(root_index + 4);
	if (parent_block <= sp->s_first_data_block ||
	    parent_block >= sp->s_blocks_count ||
	    parent_block > (((u64_t)~0 >> 1) / sp->s_block_size) ||
	    extent_is_group_metadata(sp, parent_block) ||
	    parent_block == xattr_block)
		return(EIO);

	parent_bp = get_block(rip->i_dev, parent_block, NORMAL);
	if (parent_bp == NULL)
		return(EIO);
	parent_node = (unsigned char *)b_data(parent_bp);
	if (extent_get16(parent_node) != EXT4_EXTENT_MAGIC ||
	    extent_get16(parent_node + 6) != 1) {
		put_block(parent_bp);
		return(EIO);
	}
	parent_entries = extent_get16(parent_node + 2);
	parent_max = extent_get16(parent_node + 4);
	if (parent_max < 4 ||
	    parent_max > (sp->s_block_size - 12) / 12 ||
	    parent_entries == 0 || parent_entries > parent_max) {
		put_block(parent_bp);
		return(EIO);
	}

	owned_blocks = 1; /* The external depth-one index node. */
	previous_end = 0;
	for (i = 0; i < parent_entries; i++) {
		u64_t logical_end;

		leaf_index = parent_node + 12 + i * 12;
		leaf_key = extent_get32(leaf_index);
		leaf_block = ((u64_t)extent_get16(leaf_index + 8) << 32) |
		    extent_get32(leaf_index + 4);
		if (leaf_block <= sp->s_first_data_block ||
		    leaf_block >= sp->s_blocks_count ||
		    leaf_block > (((u64_t)~0 >> 1) / sp->s_block_size) ||
		    leaf_block == parent_block || leaf_block == xattr_block ||
		    extent_is_group_metadata(sp, leaf_block) ||
		    (i != 0 && leaf_key <= extent_get32(
		    parent_node + 12 + (i - 1) * 12))) {
			put_block(parent_bp);
			return(EIO);
		}
		for (j = 0; j < i; j++) {
			const unsigned char *prior_index =
			    parent_node + 12 + j * 12;
			block64_t prior_leaf =
			    ((u64_t)extent_get16(prior_index + 8) << 32) |
			    extent_get32(prior_index + 4);
			if (prior_leaf == leaf_block) {
				put_block(parent_bp);
				return(EIO);
			}
		}

		leaf_bp = get_block(rip->i_dev, leaf_block, NORMAL);
		if (leaf_bp == NULL) {
			put_block(parent_bp);
			return(EIO);
		}
		leaf_node = (unsigned char *)b_data(leaf_bp);
		if (extent_get16(leaf_node) != EXT4_EXTENT_MAGIC ||
		    extent_get16(leaf_node + 6) != 0) {
			put_block(leaf_bp);
			put_block(parent_bp);
			return(EIO);
		}
		leaf_entries = extent_get16(leaf_node + 2);
		leaf_max = extent_get16(leaf_node + 4);
		if (leaf_max < 4 ||
		    leaf_max > (sp->s_block_size - 12) / 12 ||
		    leaf_entries == 0 || leaf_entries > leaf_max) {
			put_block(leaf_bp);
			put_block(parent_bp);
			return(EIO);
		}
		extent_get(leaf_node + 12, &ex);
		if (ex.logical != leaf_key ||
		    (i == 0 && leaf_key != root_key)) {
			put_block(leaf_bp);
			put_block(parent_bp);
			return(EIO);
		}

		previous_end = 0;
		next_key = (u64_t)UINT32_MAX + 1;
		if (i + 1 < parent_entries)
			next_key = extent_get32(parent_node + 12 +
			    (i + 1) * 12);
		for (j = 0; j < leaf_entries; j++) {
			extent_get(leaf_node + 12 + j * 12, &ex);
			logical_end = (u64_t)ex.logical + ex.length;
			if (ex.length == 0 ||
			    ex.physical <= sp->s_first_data_block ||
			    ex.physical >= sp->s_blocks_count ||
			    ex.length > sp->s_blocks_count - ex.physical ||
			    extent_overlaps_group_metadata(sp, ex.physical,
			    ex.length) ||
			    extent_overlaps_tree_node(parent_node, parent_entries,
			    ex.physical, ex.length) ||
			    extent_ranges_overlap(ex.physical, ex.length,
			    parent_block, 1) ||
			    (xattr_block != 0 &&
			    xattr_block >= ex.physical &&
			    xattr_block - ex.physical < ex.length) ||
			    logical_end > ((u64_t)UINT32_MAX + 1) ||
			    (for_truncate && logical_end > file_blocks) ||
			    (j != 0 && ex.logical < previous_end) ||
			    logical_end > next_key) {
				put_block(leaf_bp);
				put_block(parent_bp);
				return(EIO);
			}
			for (k = 0; k < j; k++) {
				extent_get(leaf_node + 12 + k * 12, &prior);
				if (extent_ranges_overlap(ex.physical, ex.length,
				    prior.physical, prior.length)) {
					put_block(leaf_bp);
					put_block(parent_bp);
					return(EIO);
				}
			}
			if (owned_blocks > ~(u64_t)0 - ex.length) {
				put_block(leaf_bp);
				put_block(parent_bp);
				return(EIO);
			}
			owned_blocks += ex.length;
			previous_end = logical_end;
		}
		if (owned_blocks == ~(u64_t)0) {
			put_block(leaf_bp);
			put_block(parent_bp);
			return(EIO);
		}
		owned_blocks++; /* One external depth-zero leaf. */
		put_block(leaf_bp);
	}

	/* Catch aliases between distinct leaves, including untouched siblings. */
	first_bp = get_block(rip->i_dev,
	    ((u64_t)extent_get16(parent_node + 20) << 32) |
	    extent_get32(parent_node + 16), NORMAL);
	if (first_bp == NULL) {
		put_block(parent_bp);
		return(EIO);
	}
	leaf_node = (unsigned char *)b_data(first_bp);
	leaf_entries = extent_get16(leaf_node + 2);
	if (extent_depth1_check_siblings(rip, parent_node, parent_entries, 0,
	    leaf_node, leaf_entries, FALSE, 0, parent_block) != OK) {
		put_block(first_bp);
		put_block(parent_bp);
		return(EIO);
	}
	put_block(first_bp);
	put_block(parent_bp);

	if (owned_blocks > ~(u64_t)0 / sp->s_sectors_in_block ||
	    owned_blocks * sp->s_sectors_in_block != rip->i_blocks)
		return(EIO);

	*parent_blockp = parent_block;
	return(OK);
}

/*
 * Update the leaf set beneath a single depth-two root index. The shared
 * depth-one writer can then split/merge leaves in the external index node.
 * If the external index becomes small enough, copy it back into the inode
 * root and release the now-redundant index block.
 */
static int
extent_depth2_write_map_single(struct inode *rip, off_t position,
	block64_t new_wblock, int op)
{
	unsigned char *root, *parent_node;
	struct buf *parent_bp;
	block64_t parent_block;
	u16_t root_max, parent_entries;
	int r;

	root = (unsigned char *)rip->i_block;
	root_max = extent_get16(root + 4);
	r = extent_depth2_preflight_single_root(rip, &parent_block, FALSE);
	if (r != OK)
		return(r);

	parent_bp = get_block(rip->i_dev, parent_block, NORMAL);
	if (parent_bp == NULL)
		return(EIO);
	parent_node = (unsigned char *)b_data(parent_bp);
	r = extent_depth1_write_map_root(rip, position, new_wblock, op,
	    parent_node, TRUE, parent_block);
	if (r != OK) {
		put_block(parent_bp);
		return(r);
	}

	parent_entries = extent_get16(parent_node + 2);
	if (parent_entries == 0) {
		if (rip->i_blocks < rip->i_sp->s_sectors_in_block) {
			put_block(parent_bp);
			return(EIO);
		}
		memset(root + 12, 0, sizeof(rip->i_block) - 12);
		extent_put16(root + 2, 0);
		extent_put16(root + 6, 0);
		rip->i_blocks -= rip->i_sp->s_sectors_in_block;
		rip->i_dirt = IN_DIRTY;
		put_block(parent_bp);
		free_block(rip->i_sp, parent_block);
		return(OK);
	}

	/* The parent index block is no longer needed after demotion. */
	if (parent_entries <= root_max) {
		if (rip->i_blocks < rip->i_sp->s_sectors_in_block) {
			put_block(parent_bp);
			return(EIO);
		}
		/* Collapse depth two back to depth one in the inode root. */
		memset(root, 0, sizeof(rip->i_block));
		extent_put16(root, EXT4_EXTENT_MAGIC);
		extent_put16(root + 2, parent_entries);
		extent_put16(root + 4, root_max);
		extent_put16(root + 6, 1);
		memcpy(root + 12, parent_node + 12, parent_entries * 12);
		rip->i_blocks -= rip->i_sp->s_sectors_in_block;
		rip->i_dirt = IN_DIRTY;
		put_block(parent_bp);
		free_block(rip->i_sp, parent_block);
		return(OK);
	}

	/* The external index remains attached; persist its modified records. */
	extent_put32(root + 12, extent_get32(parent_node + 12));
	lmfs_markdirty(parent_bp);
	put_block(parent_bp);
	rip->i_dirt = IN_DIRTY;
	return(OK);
}


static int
extent_depth1_write_map(struct inode *rip, off_t position,
	block64_t new_wblock, int op)
{
	return(extent_depth1_write_map_root(rip, position, new_wblock, op,
	    (unsigned char *)rip->i_block, FALSE, NO_BLOCK));
}


int
ext4_extent_write_map(struct inode *rip, off_t position,
	block64_t new_wblock, int op)
{
	unsigned char *root, *entry;
	struct ext4_extent ex, previous, next;
	u16_t entries, max_entries;
	u64_t logical64, physical;
	block64_t xattr_block;
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
	 * The inode root may point to depth-one leaves or to a single
	 * depth-one index node containing depth-zero leaves.
	 */
	if (extent_get16((unsigned char *)rip->i_block) ==
	    EXT4_EXTENT_MAGIC) {
		u16_t depth = extent_get16((unsigned char *)rip->i_block + 6);
		if (depth == 1)
			return(extent_depth1_write_map(rip, position,
			    new_wblock, op));
		if (depth == 2)
			return(extent_depth2_write_map_single(rip, position,
			    new_wblock, op));
		if (depth > 2)
			return(EOPNOTSUPP);
	}

	if ((r = extent_root(rip, &root, &entries, &max_entries)) != OK)
		return(r);

	xattr_block = extent_inode_xattr_block(rip);
	if (extent_validate_xattr_block(rip, xattr_block) != OK ||
	    (xattr_block != 0 &&
	    extent_block_is_mapped(root + 12, entries, xattr_block)))
		return(EIO);

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
	    physical == xattr_block ||
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
		/* Promote a full inline root before splitting an extent. */
		if (offset != 0 && offset != ex.length - 1 &&
		    entries >= max_entries) {
			if ((r = extent_promote_root(rip, root, entries,
			    max_entries)) != OK)
				return(r);
			return(extent_depth1_write_map(rip, position,
			    new_wblock, op));
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
			if (entries >= max_entries) {
				if ((r = extent_promote_root(rip, root, entries,
				    max_entries)) != OK)
					return(r);
				return(extent_depth1_write_map(rip, position,
				    new_wblock, op));
			}
			memmove(entry + 12, entry,
			    (entries - i) * 12);
			extent_put(entry, logical, old_physical, 1, FALSE);
			extent_put(entry + 12, logical + 1, old_physical + 1,
			    ex.length - 1, TRUE);
			entries++;
		} else if (offset == ex.length - 1) {
			if (entries >= max_entries) {
				if ((r = extent_promote_root(rip, root, entries,
				    max_entries)) != OK)
					return(r);
				return(extent_depth1_write_map(rip, position,
				    new_wblock, op));
			}
			extent_put(entry, ex.logical, ex.physical, ex.length - 1,
			    TRUE);
			memmove(entry + 24, entry + 12,
			    (entries - i - 1) * 12);
			extent_put(entry + 12, logical, old_physical, 1, FALSE);
			entries++;
		} else {
			if (entries > max_entries - 2) {
				if ((r = extent_promote_root(rip, root, entries,
				    max_entries)) != OK)
					return(r);
				return(extent_depth1_write_map(rip, position,
				    new_wblock, op));
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

	if (entries >= max_entries) {
		if ((r = extent_promote_root(rip, root, entries,
		    max_entries)) != OK)
			return(r);
		return(extent_depth1_write_map(rip, position,
		    new_wblock, op));
	}

	/* Insert a one-block initialized extent in logical-block order. */
	entry = root + 12 + insert * 12;
	memmove(entry + 12, entry, (entries - insert) * 12);
	extent_put(entry, logical, physical, 1, FALSE);
	extent_put16(root + 2, entries + 1);
	rip->i_blocks += rip->i_sp->s_sectors_in_block;
	rip->i_dirt = IN_DIRTY;
	return(OK);
}
