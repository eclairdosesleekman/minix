/*
 * Limited ext4 extent-tree updates.
 *
 * The implementation deliberately updates only extent trees stored entirely
 * in the inode (depth zero).  External extent nodes need splitting, checksums,
 * and transaction handling; those operations are rejected rather than being
 * rewritten as ext2 indirect blocks.
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
 * Return the inode's inline extent root.  A non-zero depth is a valid ext4
 * tree, but this writer cannot rebalance its external nodes.
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
		struct ext4_extent ex;

		extent_get(root + 12 + i * 12, &ex);
		if (ex.length == 0 || ex.physical == 0 ||
		    ex.physical + ex.length > rip->i_sp->s_blocks_count ||
		    (u64_t)ex.logical + ex.length > ((u64_t)UINT32_MAX + 1) ||
		    (i != 0 && ex.logical < previous_end))
			return(EIO);
		previous_end = (u64_t)ex.logical + ex.length;
	}

	*rootp = root;
	*entriesp = entries;
	*maxp = max_entries;
	return(OK);
}

int
ext4_extent_write_map(struct inode *rip, off_t position,
	block_t new_wblock, int op)
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

	if ((r = extent_root(rip, &root, &entries, &max_entries)) != OK)
		return(r);

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
		free_block(rip->i_sp, (block_t)block_to_free);
		rip->i_blocks -= rip->i_sp->s_sectors_in_block;
		rip->i_dirt = IN_DIRTY;
		return(OK);
	}

	/* Normal allocation must not overlap an initialized or unwritten extent. */
	if (i != entries)
		return(EEXIST);

	physical = (u64_t)new_wblock;
	if (physical == 0 || physical >= rip->i_sp->s_blocks_count ||
	    physical > (((u64_t)1 << 48) - 1))
		return(EIO);
	if (rip->i_blocks > UINT32_MAX - rip->i_sp->s_sectors_in_block)
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
