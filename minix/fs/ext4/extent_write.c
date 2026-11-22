/*
 * ext4 extent-tree updates.
 *
 * Inline roots and existing external leaves are edited in place. Full
 * external leaves can also be split when their immediate parent index node
 * has room, at any supported tree depth. Index-node splitting, deeper-tree
 * growth, and root rebalancing remain unsupported.
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
 * Locate the extent leaf which owns logical. Existing external nodes may be
 * edited in place, but this routine does not allocate, split, or rebalance
 * nodes. Propagate the selected child's exclusive upper bound through every
 * index level so a later leaf split cannot cross an ancestor boundary. A key
 * before the first index entry would require changing the tree root, so
 * callers get EOPNOTSUPP in that case.
 */
static int
extent_leaf(struct inode *rip, u32_t logical, unsigned char **nodep,
	size_t *node_sizep, u16_t *entriesp, u16_t *maxp,
	struct buf **bpp, unsigned char **root_nodep,
	u16_t *root_entriesp, u16_t *root_maxp, u16_t *root_indexp,
	u16_t *root_depthp, block64_t *parent_blockp,
	u16_t *parent_entriesp, u16_t *parent_maxp,
	u16_t *parent_indexp, u32_t *parent_keyp,
	block64_t *grandparent_blockp, u16_t *grandparent_entriesp,
	u16_t *grandparent_maxp, u16_t *grandparent_indexp,
	u32_t *grandparent_keyp,
	u64_t *leaf_upper_boundp, block64_t *leaf_blockp)
{
	unsigned char *node;
	struct buf *bp;
	size_t node_size;
	int expected_depth;
	u16_t magic, entries, max_entries, depth;
	u64_t previous_end, upper_bound;
	u32_t expected_key;
	block64_t current_block;
	int have_expected_key;
	unsigned int i;

	node = (unsigned char *)rip->i_block;
	node_size = sizeof(rip->i_block);
	bp = NULL;
	current_block = NO_BLOCK;
	upper_bound = (u64_t)UINT32_MAX + 1;
	*leaf_upper_boundp = upper_bound;
	*parent_blockp = NO_BLOCK;
	*parent_entriesp = 0;
	*parent_maxp = 0;
	*parent_indexp = 0;
	*parent_keyp = 0;
	*grandparent_blockp = NO_BLOCK;
	*grandparent_entriesp = 0;
	*grandparent_maxp = 0;
	*grandparent_indexp = 0;
	*grandparent_keyp = 0;
	*leaf_blockp = NO_BLOCK;
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
			if (*root_depthp == 1) {
				*parent_entriesp = *root_entriesp;
				*parent_maxp = *root_maxp;
				*parent_indexp = *root_indexp;
				*parent_keyp = extent_get32(*root_nodep + 12 +
				    (size_t)*root_indexp * 12);
			} else if (*root_depthp > 1 && *parent_blockp == NO_BLOCK) {
				goto corrupt;
			}
			previous_end = 0;
			for (i = 0; i < entries; i++) {
				struct ext4_extent ex;

				extent_get(node + 12 + i * 12, &ex);
				if (ex.length == 0 || ex.physical == 0 ||
				    ex.physical >= rip->i_sp->s_blocks_count ||
				    ex.length >
				    rip->i_sp->s_blocks_count - ex.physical ||
				    (u64_t)ex.logical + ex.length > upper_bound ||
				    (i != 0 && ex.logical < previous_end))
					goto corrupt;
				previous_end = (u64_t)ex.logical + ex.length;
			}
			*leaf_blockp = current_block;
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
			u64_t child_upper_bound;
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
				    (u64_t)key >= upper_bound ||
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

			child_upper_bound = selected_index + 1 < entries ?
			    extent_get32(node + 12 +
			    (size_t)(selected_index + 1) * 12) : upper_bound;
			if (child_upper_bound <= selected_key ||
			    child_upper_bound > upper_bound)
				goto corrupt;
			if (depth == 1)
				*leaf_upper_boundp = child_upper_bound;
			if (expected_depth == -1)
				*root_indexp = (u16_t)selected_index;
			/*
			 * Remember the immediate external index node when it owns
			 * the leaf being returned. Its buffer is reacquired only if
			 * the caller needs to split that leaf.
			 */
			if (depth == 1 && bp != NULL) {
				*parent_blockp = current_block;
				*parent_entriesp = entries;
				*parent_maxp = max_entries;
				*parent_indexp = (u16_t)selected_index;
				*parent_keyp = selected_key;
			}
			/*
			 * For trees deeper than two, retain the depth-two index node
			 * that owns the immediate depth-one parent. It can safely lose
			 * a non-first child without changing any ancestor key.
			 */
			if (depth == 2 && bp != NULL) {
				*grandparent_blockp = current_block;
				*grandparent_entriesp = entries;
				*grandparent_maxp = max_entries;
				*grandparent_indexp = (u16_t)selected_index;
				*grandparent_keyp = selected_key;
			}
			upper_bound = child_upper_bound;
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
			current_block = selected_child;
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
 * Reacquire a depth-two ancestor before removing its selected depth-one child.
 * Only a non-first child can be detached without propagating a new first key
 * into the ancestor above it.
 */
static int
extent_prepare_grandparent_index(struct inode *rip,
	block64_t grandparent_block, block64_t parent_block,
	u16_t grandparent_entries, u16_t grandparent_max,
	u16_t grandparent_index, u32_t grandparent_key,
	struct buf **grandparent_bpp, unsigned char **grandparent_nodep)
{
	struct buf *bp;
	unsigned char *node, *entry;
	block64_t selected_block;
	size_t tail_offset;

	*grandparent_bpp = NULL;
	*grandparent_nodep = NULL;
	if (grandparent_block == NO_BLOCK ||
	    grandparent_block >= rip->i_sp->s_blocks_count ||
	    parent_block == NO_BLOCK ||
	    parent_block >= rip->i_sp->s_blocks_count ||
	    parent_block == grandparent_block ||
	    grandparent_entries <= 1 ||
	    grandparent_index == 0 ||
	    grandparent_index >= grandparent_entries ||
	    grandparent_max < grandparent_entries ||
	    grandparent_max == 0 ||
	    grandparent_max > (rip->i_sp->s_block_size - 12) / 12)
		return(EOPNOTSUPP);

	bp = get_block(rip->i_dev, grandparent_block, NORMAL);
	if (bp == NULL)
		return(EIO);
	node = (unsigned char *)b_data(bp);
	if (extent_get16(node) != EXT4_EXTENT_MAGIC ||
	    extent_get16(node + 2) != grandparent_entries ||
	    extent_get16(node + 4) != grandparent_max ||
	    extent_get16(node + 6) != 2) {
		extent_release_leaf(rip, bp, FALSE);
		return(EIO);
	}
	if (HAS_RO_COMPAT_FEATURE(rip->i_sp,
	    RO_COMPAT_METADATA_CSUM)) {
		u32_t stored, calculated;

		tail_offset = 12 + (size_t)grandparent_max * 12;
		if (tail_offset > rip->i_sp->s_block_size - sizeof(stored)) {
			extent_release_leaf(rip, bp, FALSE);
			return(EIO);
		}
		stored = extent_get32(node + tail_offset);
		calculated = ext4_crc32c(ext4_inode_csum_seed(rip),
		    node, tail_offset);
		if (stored != calculated) {
			extent_release_leaf(rip, bp, FALSE);
			return(EIO);
		}
	}

	entry = node + 12 + (size_t)grandparent_index * 12;
	selected_block = ((u64_t)extent_get16(entry + 8) << 32) |
	    extent_get32(entry + 4);
	if (selected_block != parent_block ||
	    extent_get32(entry) != grandparent_key) {
		extent_release_leaf(rip, bp, FALSE);
		return(EIO);
	}

	*grandparent_bpp = bp;
	*grandparent_nodep = node;
	return(OK);
}

/*
 * A depth-two tree can detach a one-child intermediate node from the inode
 * root when its only leaf is removed. Revalidate the root's child pointer and
 * key before allowing that special case.
 */
static int
extent_root_parent_matches(struct inode *rip, unsigned char *root_node,
	u16_t root_entries, u16_t root_max, u16_t root_index,
	block64_t parent_block, u32_t parent_key)
{
	const unsigned char *entry;
	block64_t selected_block;

	if (root_node == NULL || parent_block == NO_BLOCK ||
	    parent_block >= rip->i_sp->s_blocks_count || root_entries == 0 ||
	    root_index >= root_entries || root_max < root_entries ||
	    root_max == 0 ||
	    root_max > (sizeof(rip->i_block) - 12) / 12)
		return(FALSE);
	if (extent_get16(root_node) != EXT4_EXTENT_MAGIC ||
	    extent_get16(root_node + 2) != root_entries ||
	    extent_get16(root_node + 4) != root_max ||
	    extent_get16(root_node + 6) != 2)
		return(FALSE);

	entry = root_node + 12 + (size_t)root_index * 12;
	selected_block = ((u64_t)extent_get16(entry + 8) << 32) |
	    extent_get32(entry + 4);
	return(selected_block == parent_block &&
	    extent_get32(entry) == parent_key);
}

/*
 * Reacquire and validate the immediate parent index node before changing
 * its selected child entry. The inode root has no ancestor, so depth-one
 * roots can update or remove any entry. At greater depths, in-place changes
 * require a non-first entry because changing entry zero alters an ancestor
 * key. The depth-two sole-child detach is safe because it removes the entire
 * intermediate node from the inode root instead of changing its first key.
 */
static int
extent_prepare_parent_index(struct inode *rip,
	unsigned char *root_node, u16_t root_depth, block64_t parent_block,
	block64_t leaf_block, u16_t parent_entries, u16_t parent_max,
	u16_t parent_index, u32_t parent_key, int allow_single_child_detach,
	struct buf **parent_bpp, unsigned char **parent_nodep)
{
	struct buf *bp;
	unsigned char *node, *entry;
	block64_t selected_block;
	size_t tail_offset;

	*parent_bpp = NULL;
	*parent_nodep = NULL;
	if (root_node == NULL || root_depth == 0 ||
	    root_depth > EXT4_EXTENT_MAX_DEPTH || leaf_block == NO_BLOCK ||
	    parent_entries == 0 || parent_index >= parent_entries ||
	    parent_max < parent_entries ||
	    (root_depth > 1 && parent_index == 0 &&
	    !(allow_single_child_detach && parent_entries == 1)))
		return(EOPNOTSUPP);

	bp = NULL;
	if (root_depth == 1) {
		if (parent_block != NO_BLOCK ||
		    parent_max > (sizeof(rip->i_block) - 12) / 12)
			return(EIO);
		node = root_node;
	} else {
		if (parent_block == NO_BLOCK ||
		    parent_block >= rip->i_sp->s_blocks_count ||
		    parent_max > (rip->i_sp->s_block_size - 12) / 12)
			return(EIO);
		bp = get_block(rip->i_dev, parent_block, NORMAL);
		if (bp == NULL)
			return(EIO);
		node = (unsigned char *)b_data(bp);
		if (extent_get16(node) != EXT4_EXTENT_MAGIC ||
		    extent_get16(node + 2) != parent_entries ||
		    extent_get16(node + 4) != parent_max ||
		    extent_get16(node + 6) != 1) {
			extent_release_leaf(rip, bp, FALSE);
			return(EIO);
		}
		if (HAS_RO_COMPAT_FEATURE(rip->i_sp,
		    RO_COMPAT_METADATA_CSUM)) {
			u32_t stored, calculated;

			tail_offset = 12 + (size_t)parent_max * 12;
			if (tail_offset > rip->i_sp->s_block_size -
			    sizeof(stored)) {
				extent_release_leaf(rip, bp, FALSE);
				return(EIO);
			}
			stored = extent_get32(node + tail_offset);
			calculated = ext4_crc32c(ext4_inode_csum_seed(rip),
			    node, tail_offset);
			if (stored != calculated) {
				extent_release_leaf(rip, bp, FALSE);
				return(EIO);
			}
		}
	}

	if (extent_get16(node) != EXT4_EXTENT_MAGIC ||
	    extent_get16(node + 2) != parent_entries ||
	    extent_get16(node + 4) != parent_max ||
	    extent_get16(node + 6) != 1) {
		extent_release_leaf(rip, bp, FALSE);
		return(EIO);
	}
	entry = node + 12 + (size_t)parent_index * 12;
	selected_block = ((u64_t)extent_get16(entry + 8) << 32) |
	    extent_get32(entry + 4);
	if (selected_block != leaf_block ||
	    extent_get32(entry) != parent_key) {
		extent_release_leaf(rip, bp, FALSE);
		return(EIO);
	}

	*parent_bpp = bp;
	*parent_nodep = node;
	return(OK);
}

/*
 * Split a full external leaf when its immediate parent index node has room.
 * The inherited exclusive upper bound comes from the complete selected path,
 * so the new leaf remains inside every ancestor's logical-block interval.
 * On failure, the original inode and leaf remain unchanged.
 */
static int
extent_split_full_leaf(struct inode *rip, unsigned char *leaf,
	struct buf *leaf_bp, unsigned char *root_node, u16_t root_depth,
	block64_t parent_block, block64_t leaf_block,
	u16_t parent_entries, u16_t parent_max,
	u16_t parent_index, u32_t parent_key, u64_t leaf_upper_bound,
	u16_t leaf_entries, u16_t leaf_max, unsigned int insert,
	u32_t logical, u64_t physical)
{
	struct buf *new_bp, *parent_bp;
	unsigned char *new_node, *new_index, *parent_node;
	unsigned char *records;
	u64_t upper, sectors, end_key;
	block64_t new_block, selected_block;
	u32_t first_key, right_key;
	u16_t left_entries, right_entries;
	size_t records_size, tail_offset;
	unsigned int i;
	int parent_is_external;

	parent_bp = NULL;
	parent_is_external = FALSE;
	if (leaf_bp == NULL || root_node == NULL || leaf_block == NO_BLOCK ||
	    root_depth == 0 || root_depth > EXT4_EXTENT_MAX_DEPTH ||
	    leaf_entries != leaf_max || insert > leaf_entries ||
	    parent_entries == 0 || parent_index >= parent_entries ||
	    parent_entries >= parent_max) {
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(EFBIG);
	}

	if (root_depth == 1) {
		parent_node = root_node;
		if (parent_block != NO_BLOCK) {
			extent_release_leaf(rip, leaf_bp, FALSE);
			return(EIO);
		}
	} else {
		if (parent_block == NO_BLOCK ||
		    parent_block >= rip->i_sp->s_blocks_count) {
			extent_release_leaf(rip, leaf_bp, FALSE);
			return(EIO);
		}
		parent_bp = get_block(rip->i_dev, parent_block, NORMAL);
		if (parent_bp == NULL) {
			extent_release_leaf(rip, leaf_bp, FALSE);
			return(EIO);
		}
		parent_is_external = TRUE;
		parent_node = (unsigned char *)b_data(parent_bp);
		if (extent_get16(parent_node) != EXT4_EXTENT_MAGIC ||
		    extent_get16(parent_node + 2) != parent_entries ||
		    extent_get16(parent_node + 4) != parent_max ||
		    extent_get16(parent_node + 6) != 1 ||
		    parent_max > (rip->i_sp->s_block_size - 12) / 12)
			goto corrupt;
		if (HAS_RO_COMPAT_FEATURE(rip->i_sp, RO_COMPAT_METADATA_CSUM)) {
			u32_t stored, calculated;

			tail_offset = 12 + (size_t)parent_max * 12;
			if (tail_offset > rip->i_sp->s_block_size -
			    sizeof(stored))
				goto corrupt;
			stored = extent_get32(parent_node + tail_offset);
			calculated = ext4_crc32c(ext4_inode_csum_seed(rip),
			    parent_node, tail_offset);
			if (stored != calculated)
				goto corrupt;
		}
	}

	first_key = extent_get32(leaf + 12);
	new_index = parent_node + 12 + parent_index * 12;
	selected_block = ((u64_t)extent_get16(new_index + 8) << 32) |
	    extent_get32(new_index + 4);
	if (selected_block != leaf_block ||
	    extent_get32(new_index) != parent_key ||
	    parent_key != first_key || logical < first_key) {
		goto corrupt;
	}

	upper = leaf_upper_bound;
	if (upper <= parent_key ||
	    upper > (u64_t)UINT32_MAX + 1 ||
	    (parent_index + 1 < parent_entries &&
	    upper != extent_get32(parent_node + 12 +
	    (parent_index + 1) * 12)))
		goto corrupt;
	if ((u64_t)logical + 1 > upper)
		goto corrupt;
	for (i = 0; i < leaf_entries; i++) {
		struct ext4_extent ex;

		extent_get(leaf + 12 + i * 12, &ex);
		end_key = (u64_t)ex.logical + ex.length;
		if (end_key > upper)
			goto corrupt;
	}

	sectors = rip->i_sp->s_sectors_in_block;
	if (sectors == 0 || sectors > ~(u64_t)0 / 2 ||
	    !extent_i_blocks_can_add(rip, sectors * 2)) {
		extent_release_leaf(rip, parent_bp, FALSE);
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(EFBIG);
	}

	records_size = (size_t)(leaf_entries + 1) * 12;
	records = malloc(records_size);
	if (records == NULL) {
		extent_release_leaf(rip, parent_bp, FALSE);
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
		extent_release_leaf(rip, parent_bp, FALSE);
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(EIO);
	}

	/* Allocate and fully construct the new leaf before publishing its key. */
	new_block = alloc_block(rip, NO_BLOCK);
	if (new_block == NO_BLOCK) {
		free(records);
		extent_release_leaf(rip, parent_bp, FALSE);
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(ENOSPC);
	}
	if (new_block < rip->i_sp->s_first_data_block ||
	    new_block >= rip->i_sp->s_blocks_count)
		panic("ext4: allocator returned invalid extent-tree block");
	if (new_block > 0xffffffffffffULL) {
		free_block(rip->i_sp, new_block);
		free(records);
		extent_release_leaf(rip, parent_bp, FALSE);
		extent_release_leaf(rip, leaf_bp, FALSE);
		return(EFBIG);
	}
	new_bp = get_block(rip->i_dev, new_block, NO_READ);
	if (new_bp == NULL) {
		free_block(rip->i_sp, new_block);
		free(records);
		extent_release_leaf(rip, parent_bp, FALSE);
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

	/* The existing leaf keeps its key; the new leaf gets the upper half. */
	memcpy(leaf + 12, records, (size_t)left_entries * 12);
	memset(leaf + 12 + (size_t)left_entries * 12, 0,
	    (size_t)(leaf_max - left_entries) * 12);
	extent_put16(leaf + 2, left_entries);

	/* Add the new leaf key immediately after the original child. */
	new_index = parent_node + 12 + (parent_index + 1) * 12;
	memmove(new_index + 12, new_index,
	    (size_t)(parent_entries - parent_index - 1) * 12);
	extent_put32(new_index, right_key);
	extent_put32(new_index + 4, (u32_t)new_block);
	extent_put16(new_index + 8, (u16_t)(new_block >> 32));
	extent_put16(new_index + 10, 0);
	extent_put16(parent_node + 2, parent_entries + 1);

	/* Checksums cover the external parent index and both child leaves. */
	extent_release_leaf(rip, new_bp, TRUE);
	extent_release_leaf(rip, leaf_bp, TRUE);
	if (parent_is_external)
		extent_release_leaf(rip, parent_bp, TRUE);
	free(records);

	rip->i_blocks += sectors * 2; /* one data block plus one extent-tree node */
	rip->i_dirt = IN_DIRTY;
	return(OK);

corrupt:
	extent_release_leaf(rip, parent_bp, FALSE);
	extent_release_leaf(rip, leaf_bp, FALSE);
	return(EIO);
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
	u16_t root_index, root_depth, parent_entries, parent_max;
	u16_t parent_index, grandparent_entries, grandparent_max;
	u16_t grandparent_index;
	block64_t parent_block, grandparent_block, leaf_block;
	u32_t parent_key, grandparent_key;
	u64_t logical64, physical, leaf_upper_bound;
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
	    &root_index, &root_depth, &parent_block, &parent_entries,
	    &parent_max, &parent_index, &parent_key, &grandparent_block,
	    &grandparent_entries, &grandparent_max, &grandparent_index,
	    &grandparent_key, &leaf_upper_bound, &leaf_block)) != OK)
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
		struct buf *parent_key_bp, *grandparent_bp;
		unsigned char *parent_key_node, *parent_key_entry;
		unsigned char *grandparent_node, *grandparent_entry;
		u64_t block_to_free, sectors, blocks_to_free;
		u32_t new_parent_key;
		int update_parent_key, remove_empty_leaf, remove_empty_parent;
		int detach_from_root, detach_from_grandparent;

		parent_key_bp = NULL;
		parent_key_node = NULL;
		grandparent_bp = NULL;
		grandparent_node = NULL;
		update_parent_key = FALSE;
		remove_empty_leaf = FALSE;
		remove_empty_parent = FALSE;
		detach_from_root = FALSE;
		detach_from_grandparent = FALSE;
		if (i == entries) {
			extent_release_leaf(rip, leaf_bp, FALSE);
			return(OK);	/* A hole needs no work. */
		}

		offset = logical - ex.logical;
		block_to_free = ex.physical + offset;
		remove_empty_leaf = leaf_bp != NULL && entries == 1 &&
		    ex.length == 1 && offset == 0;
		if (remove_empty_leaf) {
			sectors = rip->i_sp->s_sectors_in_block;
			detach_from_root = root_depth == 2 &&
			    parent_entries == 1 && parent_index == 0;
			detach_from_grandparent = root_depth > 2 &&
			    parent_entries == 1 && parent_index == 0 &&
			    grandparent_entries > 1 && grandparent_index > 0;
			remove_empty_parent = detach_from_root ||
			    detach_from_grandparent;
			blocks_to_free = remove_empty_parent ? 3 : 2;
			if ((parent_entries <= 1 && root_depth != 1 &&
			    !remove_empty_parent) ||
			    sectors == 0 || sectors > ~(u64_t)0 / blocks_to_free ||
			    rip->i_blocks < sectors * blocks_to_free ||
			    block_to_free == leaf_block ||
			    block_to_free == parent_block ||
			    leaf_block == parent_block ||
			    block_to_free == grandparent_block ||
			    leaf_block == grandparent_block ||
			    parent_block == grandparent_block ||
			    (detach_from_root &&
			    !extent_root_parent_matches(rip, root_node, root_entries,
			    root_max, root_index, parent_block, parent_key)) ||
			    (root_depth == 1 &&
			    (root_entries != 1 || parent_index != 0 ||
			    parent_block != NO_BLOCK))) {
				extent_release_leaf(rip, leaf_bp, FALSE);
				return(EOPNOTSUPP);
			}
			r = extent_prepare_parent_index(rip, root_node,
			    root_depth, parent_block, leaf_block, parent_entries,
			    parent_max, parent_index, parent_key, remove_empty_parent,
			    &parent_key_bp, &parent_key_node);
			if (r != OK) {
				extent_release_leaf(rip, leaf_bp, FALSE);
				return(r);
			}
			if (detach_from_grandparent) {
				r = extent_prepare_grandparent_index(rip,
				    grandparent_block, parent_block,
				    grandparent_entries, grandparent_max,
				    grandparent_index, grandparent_key,
				    &grandparent_bp, &grandparent_node);
				if (r != OK) {
					extent_release_leaf(rip, parent_key_bp, FALSE);
					extent_release_leaf(rip, leaf_bp, FALSE);
					return(r);
				}
			}

			if (root_depth == 1 && parent_entries == 1) {
				/*
				 * The inode root has no parent to preserve. Collapse
				 * its sole external leaf back to an empty inline root.
				 */
				extent_put16(parent_key_node + 2, 0);
				extent_put16(parent_key_node + 6, 0);
				memset(parent_key_node + 12, 0,
				    sizeof(rip->i_block) - 12);
			} else if (detach_from_root) {
				/*
				 * Detach the now-empty depth-one index node from the
				 * inode root. The root has no ancestor whose key changes.
				 */
				parent_key_entry = root_node + 12 +
				    (size_t)root_index * 12;
				memmove(parent_key_entry, parent_key_entry + 12,
				    (size_t)(root_entries - root_index - 1) * 12);
				memset(root_node + 12 +
				    (size_t)(root_entries - 1) * 12, 0, 12);
				extent_put16(root_node + 2, root_entries - 1);
				if (root_entries == 1) {
					extent_put16(root_node + 6, 0);
					memset(root_node + 12, 0,
					    sizeof(rip->i_block) - 12);
				}
			} else if (detach_from_grandparent) {
				/*
				 * Remove the now-empty depth-one node from its
				 * non-first slot in the depth-two index node.
				 */
				grandparent_entry = grandparent_node + 12 +
				    (size_t)grandparent_index * 12;
				memmove(grandparent_entry, grandparent_entry + 12,
				    (size_t)(grandparent_entries -
				    grandparent_index - 1) * 12);
				memset(grandparent_node + 12 +
				    (size_t)(grandparent_entries - 1) * 12, 0, 12);
				extent_put16(grandparent_node + 2,
				    grandparent_entries - 1);
			} else {
				/* Remove this child; keep its parent nonempty. */
				parent_key_entry = parent_key_node + 12 +
				    (size_t)parent_index * 12;
				memmove(parent_key_entry, parent_key_entry + 12,
				    (size_t)(parent_entries -
				    parent_index - 1) * 12);
				memset(parent_key_node + 12 +
				    (size_t)(parent_entries - 1) * 12, 0, 12);
				extent_put16(parent_key_node + 2,
				    parent_entries - 1);
			}

			/* Release buffers before returning their metadata blocks. */
			extent_release_leaf(rip, leaf_bp, FALSE);
			if (remove_empty_parent)
				extent_release_leaf(rip, parent_key_bp, FALSE);
			if (detach_from_grandparent)
				extent_release_leaf(rip, grandparent_bp, TRUE);
			free_block(rip->i_sp, block_to_free);
			free_block(rip->i_sp, leaf_block);
			if (remove_empty_parent)
				free_block(rip->i_sp, parent_block);
			rip->i_blocks -= sectors * blocks_to_free;
			if (!remove_empty_parent)
				extent_release_leaf(rip, parent_key_bp, TRUE);
			rip->i_dirt = IN_DIRTY;
			return(OK);
		}

		if (rip->i_blocks < rip->i_sp->s_sectors_in_block) {
			extent_release_leaf(rip, leaf_bp, FALSE);
			return(EIO);
		}
		/*
		 * Changing the first extent changes its parent index key. The
		 * parent key can be updated when the resulting leaf stays
		 * nonempty and no ancestor key needs to change.
		 */
		if (leaf_bp != NULL && i == 0 && offset == 0) {
			if (ex.length == 1) {
				if (entries == 1) {
					extent_release_leaf(rip, leaf_bp, FALSE);
					return(EOPNOTSUPP);
				}
				new_parent_key = extent_get32(entry + 12);
			} else {
				new_parent_key = ex.logical + 1;
			}
			r = extent_prepare_parent_index(rip, root_node,
			    root_depth, parent_block, leaf_block, parent_entries,
			    parent_max, parent_index, parent_key, FALSE,
			    &parent_key_bp, &parent_key_node);
			if (r != OK) {
				extent_release_leaf(rip, leaf_bp, FALSE);
				return(r);
			}
			update_parent_key = TRUE;
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
		if (update_parent_key) {
			parent_key_entry = parent_key_node + 12 +
			    (size_t)parent_index * 12;
			extent_put32(parent_key_entry, new_parent_key);
		}
		free_block(rip->i_sp, block_to_free);
		rip->i_blocks -= rip->i_sp->s_sectors_in_block;
		extent_release_leaf(rip, leaf_bp, TRUE);
		if (update_parent_key)
			extent_release_leaf(rip, parent_key_bp, TRUE);
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
		 * The selected external leaf can grow by one record when its
		 * immediate parent index node has an unused slot. The inherited
		 * upper bound keeps the split inside all ancestor subtrees. A full
		 * immediate parent still fails without changing the mapping.
		 */
		r = extent_split_full_leaf(rip, root, leaf_bp, root_node,
		    root_depth, parent_block, leaf_block, parent_entries,
		    parent_max, parent_index, parent_key, leaf_upper_bound,
		    entries, max_entries, insert, logical, physical);
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
