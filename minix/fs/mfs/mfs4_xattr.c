/*
 * MFS4 XAT4 extended attributes.
 *
 * Every update builds an entirely new, journaled XAT4 chain and switches the
 * inode xattr_head in the same transaction. The previous chain is freed in
 * that transaction. No home metadata block is modified directly.
 */
#include "fs.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/xattr.h>

#include <minix/libminixfs.h>
#include <minix/mfs4_disk.h>

#include "buf.h"
#include "inode.h"
#include "super.h"
#include "mfs4_journal.h"
#include "mfs4_xattr_core.h"
#include "mfs4_xattr.h"

struct mfs4_xattr_chain {
	uint32_t blocks[MFS4_XATTR_MAX_BLOCKS];
	uint8_t *data;
	size_t data_len;
	uint32_t entry_count;
	unsigned int block_count;
};

static int
mfs4_xattr_pointer_valid(uint32_t block)
{
	return block >= superblock.s_mfs4_data_start &&
	    block < superblock.s_mfs4_backup_block;
}

static void
mfs4_xattr_chain_clear(struct mfs4_xattr_chain *chain)
{
	if (chain->data != NULL)
		free(chain->data);
	memset(chain, 0, sizeof(*chain));
}

static int
mfs4_xattr_read_block(uint32_t block, uint8_t image[MFS4_BLOCK_SIZE])
{
	const uint8_t *shadow;
	struct buf *bp;
	int r;

	if (!mfs4_xattr_pointer_valid(block))
		return EIO;
	if (mfs4_tx_is_active()) {
		r = mfs4_tx_find_meta(block, &shadow);
		if (r == OK) {
			memcpy(image, shadow, MFS4_BLOCK_SIZE);
			return OK;
		}
		if (r != ENOENT)
			return r;
	}
	if ((r = lmfs_get_block(&bp, fs_dev, block, NORMAL)) != OK)
		return r;
	memcpy(image, b_data(bp), MFS4_BLOCK_SIZE);
	put_block(bp);
	return OK;
}

/*
 * Read and validate the complete owner/generation-bound chain. The flattened
 * stream contains complete TLVs only; entries never cross XAT4 block bounds.
 */
static int
mfs4_xattr_collect(struct inode *rip, struct mfs4_xattr_chain *chain)
{
	uint8_t image[MFS4_BLOCK_SIZE];
	uint32_t block, next, count;
	uint16_t used, entries;
	size_t total = 0;
	unsigned int i, j;
	int r;

	memset(chain, 0, sizeof(*chain));
	if (rip == NULL || rip->i_sp == NULL ||
	    rip->i_sp->s_version != V4)
		return EOPNOTSUPP;
	if (rip->i_xattr_head == 0)
		return OK;

	chain->data = malloc(MFS4_XATTR_MAX_BYTES);
	if (chain->data == NULL)
		return ENOMEM;

	block = rip->i_xattr_head;
	while (block != 0) {
		if (chain->block_count >= MFS4_XATTR_MAX_BLOCKS ||
		    !mfs4_xattr_pointer_valid(block)) {
			r = EIO;
			goto fail;
		}
		for (i = 0; i < chain->block_count; i++) {
			if (chain->blocks[i] == block) {
				r = ELOOP;
				goto fail;
			}
		}
		if ((r = mfs4_xattr_read_block(block, image)) != OK)
			goto fail;
		if ((r = mfs4_xattr_validate_block(image,
		    (uint32_t)rip->i_num, rip->i_generation, &next, &used,
		    &entries)) != OK)
			goto fail;
		/* Allocated chain blocks must contain at least one TLV. */
		if (used == 0 || entries == 0) {
			r = EIO;
			goto fail;
		}
		if (total > MFS4_XATTR_MAX_BYTES - used) {
			r = EIO;
			goto fail;
		}
		chain->blocks[chain->block_count++] = block;
		memcpy(chain->data + total, image + MFS4_XA_ENTRIES, used);
		total += used;
		if (chain->entry_count > UINT32_MAX - entries) {
			r = EOVERFLOW;
			goto fail;
		}
		chain->entry_count += entries;
		block = next;
	}

	chain->data_len = total;
	count = chain->entry_count;
	if ((r = mfs4_xattr_validate_entries(chain->data, chain->data_len,
	    count, 1)) != OK)
		goto fail;
	return OK;

fail:
	mfs4_xattr_chain_clear(chain);
	return r;
}

static int
mfs4_xattr_user_name(const char *name)
{
	int r;

	r = mfs4_xattr_name_valid(name, 1);
	if (r != OK)
		return r;
	if (strncmp(name, "user.", 5) != 0)
		return EOPNOTSUPP;
	return OK;
}

static int
mfs4_xattr_commit_stream(struct inode *rip, const uint8_t *data,
    size_t data_len, uint32_t entry_count,
    const struct mfs4_xattr_chain *old_chain)
{
	uint32_t new_blocks[MFS4_XATTR_MAX_BLOCKS];
	uint16_t used[MFS4_XATTR_MAX_BLOCKS];
	uint16_t per_block_count[MFS4_XATTR_MAX_BLOCKS];
	size_t block_start[MFS4_XATTR_MAX_BLOCKS];
	size_t offset, start;
	uint32_t new_head = 0;
	unsigned int block_count = 0, i;
	zone_t near;
	int r;

	if (data_len > MFS4_XATTR_MAX_BYTES ||
	    (data_len != 0 && data == NULL))
		return E2BIG;
	if ((r = mfs4_xattr_validate_entries(data, data_len,
	    entry_count, 1)) != OK)
		return r;
	memset(new_blocks, 0, sizeof(new_blocks));
	memset(used, 0, sizeof(used));
	memset(per_block_count, 0, sizeof(per_block_count));
	memset(block_start, 0, sizeof(block_start));

	/* Greedily pack complete aligned TLVs; no entry crosses a block. */
	offset = 0;
	while (offset < data_len) {
		size_t in_block = 0;
		uint32_t in_count = 0;

		if (block_count >= MFS4_XATTR_MAX_BLOCKS)
			return E2BIG;
		start = offset;
		block_start[block_count] = start;
		while (offset < data_len) {
			struct mfs4_xattr_entry entry;
			size_t next_offset = offset;

			if ((r = mfs4_xattr_entry_next(data, data_len,
			    &next_offset, &entry)) != OK)
				return r;
			if (entry.encoded_len > MFS4_XATTR_DATA_SIZE - in_block)
				break;
			in_block += entry.encoded_len;
			offset = next_offset;
			in_count++;
		}
		if (in_block == 0 || in_count == 0)
			return E2BIG;
		used[block_count] = (uint16_t)in_block;
		per_block_count[block_count] = (uint16_t)in_count;
		block_count++;
	}

	if (block_count != 0) {
		near = (zone_t)rip->i_zone[0];
		for (i = 0; i < block_count; i++) {
			zone_t allocated = alloc_zone(fs_dev, near);

			if (allocated == NO_ZONE) {
				r = err_code != OK ? err_code : ENOSPC;
				goto alloc_fail;
			}
			if (!mfs4_xattr_pointer_valid((uint32_t)allocated)) {
				r = EIO;
				goto alloc_fail;
			}
			new_blocks[i] = (uint32_t)allocated;
			near = allocated;
		}
		new_head = new_blocks[0];
	}

	for (i = 0; i < block_count; i++) {
		uint8_t *image;
		uint32_t next = (i + 1 < block_count) ? new_blocks[i + 1] : 0;

		if ((r = mfs4_tx_get_meta(new_blocks[i], MFS4_JMETA_XATTR,
		    &image)) != OK)
			goto alloc_fail;
		memset(image, 0, MFS4_BLOCK_SIZE);
		memcpy(image + MFS4_XA_MAGIC, MFS4_XATTR_MAGIC, 4);
		mfs4_put_le32(image + MFS4_XA_OWNER, (uint32_t)rip->i_num);
		mfs4_put_le32(image + MFS4_XA_GENERATION, rip->i_generation);
		mfs4_put_le32(image + MFS4_XA_NEXT, next);
		mfs4_put_le16(image + MFS4_XA_USED_BYTES, used[i]);
		mfs4_put_le16(image + MFS4_XA_ENTRY_COUNT,
		    per_block_count[i]);
		mfs4_put_le32(image + MFS4_XA_FLAGS, 0);
		mfs4_put_le32(image + MFS4_XA_CRC32C, 0);
		mfs4_put_le32(image + MFS4_XA_RESERVED, 0);
		memcpy(image + MFS4_XA_ENTRIES, data + block_start[i],
		    used[i]);
		mfs4_put_le32(image + MFS4_XA_CRC32C,
		    mfs4_crc32c(image, MFS4_BLOCK_SIZE));
	}

	/* Switch the owner only after every new image exists in the shadow set. */
	rip->i_xattr_head = new_head;
	IN_MARKDIRTY(rip);
	if ((r = rw_inode(rip, WRITING)) != OK)
		goto alloc_fail;

	/* Old XAT4 blocks become free in the same atomic transaction. */
	for (i = 0; i < old_chain->block_count; i++) {
		free_zone(fs_dev, (zone_t)old_chain->blocks[i]);
		if (superblock.s_rd_only) {
			r = EIO;
			goto alloc_fail;
		}
	}
	return OK;

alloc_fail:
	rip->i_xattr_head = old_chain->block_count != 0 ?
	    (uint32_t)old_chain->blocks[0] : 0;
	return r;
}

ssize_t
mfs4_getxattr(struct inode *rip, const char *name,
    struct fsdriver_data *data, size_t size)
{
	struct mfs4_xattr_chain chain;
	struct mfs4_xattr_entry entry;
	int r;

	if ((r = mfs4_xattr_user_name(name)) != OK)
		return -r;
	if (size > XATTR_SIZE_MAX)
		return -ERANGE;
	if ((r = mfs4_xattr_collect(rip, &chain)) != OK)
		return -r;
	r = mfs4_xattr_find(chain.data, chain.data_len, name, NULL, &entry);
	if (r != OK) {
		mfs4_xattr_chain_clear(&chain);
		return -r;
	}
	if (size == 0) {
		ssize_t result = entry.value_len;
		mfs4_xattr_chain_clear(&chain);
		return result;
	}
	if (size < entry.value_len) {
		mfs4_xattr_chain_clear(&chain);
		return -ERANGE;
	}
	if (data == NULL || data->size < entry.value_len) {
		mfs4_xattr_chain_clear(&chain);
		return -EINVAL;
	}
	r = fsdriver_copyout(data, 0, entry.value, entry.value_len);
	mfs4_xattr_chain_clear(&chain);
	return r == OK ? (ssize_t)entry.value_len : -r;
}

int
mfs4_setxattr(struct inode *rip, const char *name,
    struct fsdriver_data *data, size_t size, int flags)
{
	struct mfs4_xattr_chain old_chain;
	uint8_t *value = NULL, *new_data = NULL;
	size_t new_len;
	uint32_t new_count;
	int r, own = 0, exists;

	if ((r = mfs4_xattr_user_name(name)) != OK)
		return r;
	if ((flags & ~(XATTR_CREATE | XATTR_REPLACE)) != 0 ||
	    (flags & XATTR_CREATE && flags & XATTR_REPLACE))
		return EINVAL;
	if (size > MFS4_XATTR_MAX_VALUE || size > XATTR_SIZE_MAX)
		return ERANGE;
	if (size != 0) {
		if (data == NULL || data->size < size)
			return EINVAL;
		value = malloc(size);
		if (value == NULL)
			return ENOMEM;
		if ((r = fsdriver_copyin(data, 0, value, size)) != OK)
			goto out;
	}

	if ((r = mfs4_xattr_collect(rip, &old_chain)) != OK)
		goto out;
	exists = mfs4_xattr_find(old_chain.data, old_chain.data_len,
	    name, NULL, NULL) == OK;
	if ((flags & XATTR_CREATE) && exists) {
		r = EEXIST;
		goto chain_out;
	}
	if ((flags & XATTR_REPLACE) && !exists) {
		r = ENODATA;
		goto chain_out;
	}

	new_data = malloc(MFS4_XATTR_MAX_BYTES);
	if (new_data == NULL) {
		r = ENOMEM;
		goto chain_out;
	}
	if (old_chain.data_len != 0)
		memcpy(new_data, old_chain.data, old_chain.data_len);
	new_len = old_chain.data_len;
	new_count = old_chain.entry_count;
	if (exists) {
		r = mfs4_xattr_remove(new_data, &new_len, &new_count, name);
		if (r != OK)
			goto chain_out;
	}
	r = mfs4_xattr_append(new_data, MFS4_XATTR_MAX_BYTES, &new_len,
	    &new_count, name, value, size);
	if (r != OK)
		goto chain_out;
	if ((r = mfs4_tx_begin()) != OK)
		goto chain_out;
	own = 1;
	r = mfs4_xattr_commit_stream(rip, new_data, new_len, new_count,
	    &old_chain);
	if (r == OK) {
		own = 0;
		r = mfs4_tx_commit();
	} else {
		mfs4_tx_abort();
		own = 0;
	}
	goto chain_out;

chain_out:
	if (own)
		mfs4_tx_abort();
	mfs4_xattr_chain_clear(&old_chain);
out:
	if (new_data != NULL)
		free(new_data);
	if (value != NULL)
		free(value);
	return r;
}

ssize_t
mfs4_listxattr(struct inode *rip, struct fsdriver_data *data, size_t size)
{
	struct mfs4_xattr_chain chain;
	struct mfs4_xattr_entry entry;
	size_t offset = 0, needed = 0, written = 0;
	int r;

	if (size > XATTR_SIZE_MAX)
		return -ERANGE;
	if ((r = mfs4_xattr_collect(rip, &chain)) != OK)
		return -r;

	while (offset < chain.data_len) {
		if ((r = mfs4_xattr_entry_next(chain.data, chain.data_len,
		    &offset, &entry)) != OK) {
			mfs4_xattr_chain_clear(&chain);
			return -r;
		}
		if (entry.name_len >= 5 &&
		    memcmp(entry.name, "user.", 5) == 0)
			needed += (size_t)entry.name_len + 1;
	}
	if (size == 0) {
		mfs4_xattr_chain_clear(&chain);
		return (ssize_t)needed;
	}
	if (size < needed || data == NULL || data->size < needed) {
		mfs4_xattr_chain_clear(&chain);
		return -ERANGE;
	}
	offset = 0;
	while (offset < chain.data_len) {
		if ((r = mfs4_xattr_entry_next(chain.data, chain.data_len,
		    &offset, &entry)) != OK) {
			mfs4_xattr_chain_clear(&chain);
			return -r;
		}
		if (entry.name_len >= 5 &&
		    memcmp(entry.name, "user.", 5) == 0) {
			r = fsdriver_copyout(data, written, entry.name,
			    entry.name_len);
			if (r != OK) {
				mfs4_xattr_chain_clear(&chain);
				return -r;
			}
			written += entry.name_len;
			if ((r = fsdriver_zero(data, written, 1)) != OK) {
				mfs4_xattr_chain_clear(&chain);
				return -r;
			}
			written++;
		}
	}
	mfs4_xattr_chain_clear(&chain);
	return (ssize_t)written;
}

int
mfs4_removexattr(struct inode *rip, const char *name)
{
	struct mfs4_xattr_chain old_chain;
	uint8_t *new_data = NULL;
	size_t new_len;
	uint32_t new_count;
	int r;

	if ((r = mfs4_xattr_user_name(name)) != OK)
		return r;
	if ((r = mfs4_xattr_collect(rip, &old_chain)) != OK)
		return r;
	new_data = malloc(MFS4_XATTR_MAX_BYTES);
	if (new_data == NULL) {
		mfs4_xattr_chain_clear(&old_chain);
		return ENOMEM;
	}
	if (old_chain.data_len != 0)
		memcpy(new_data, old_chain.data, old_chain.data_len);
	new_len = old_chain.data_len;
	new_count = old_chain.entry_count;
	r = mfs4_xattr_remove(new_data, &new_len, &new_count, name);
	if (r != OK)
		goto out;
	if ((r = mfs4_tx_begin()) != OK)
		goto out;
	r = mfs4_xattr_commit_stream(rip, new_data, new_len, new_count,
	    &old_chain);
	if (r == OK)
		r = mfs4_tx_commit();
	else
		mfs4_tx_abort();

out:
	free(new_data);
	mfs4_xattr_chain_clear(&old_chain);
	return r;
}

int
mfs4_xattr_free_chain(struct inode *rip)
{
	struct mfs4_xattr_chain chain;
	unsigned int i;
	int r;

	if (rip == NULL || !mfs4_tx_is_active())
		return EINVAL;
	if ((r = mfs4_xattr_collect(rip, &chain)) != OK)
		return r;
	for (i = 0; i < chain.block_count; i++) {
		free_zone(fs_dev, (zone_t)chain.blocks[i]);
		if (superblock.s_rd_only) {
			r = EIO;
			goto out;
		}
	}
	rip->i_xattr_head = 0;
	r = OK;

out:
	mfs4_xattr_chain_clear(&chain);
	return r;
}
