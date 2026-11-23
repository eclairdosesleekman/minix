    uint32_t inode_pointers[MFS4_BM_INODE_POINTERS], uint64_t *file_size,
    uint64_t offset, const void *buffer, size_t length,
    size_t *bytes_written)
{
	const uint8_t *in = (const uint8_t *)buffer;
	uint8_t raw[MFS4_BM_BLOCK_SIZE];
	uint64_t logical_block, end;
	uint32_t physical;
	size_t amount, done = 0;
	unsigned int block_offset;
	int allocated, r;

	if (bytes_written != NULL)
		*bytes_written = 0;
	if (map == NULL || inode_pointers == NULL || file_size == NULL ||
	    (length != 0 && buffer == NULL) || bytes_written == NULL)
		return EINVAL;
	if (*file_size > MFS4_BM_MAX_FILE_SIZE ||
	    offset > MFS4_BM_MAX_FILE_SIZE ||
	    (uint64_t)length > MFS4_BM_MAX_FILE_SIZE - offset)
		return EFBIG;
	if (length == 0)
		return 0;
	end = offset + (uint64_t)length;

	/* A sparse seek must not expose stale bytes after the old EOF. */
	if (offset > *file_size) {
		r = mfs4_zero_tail(map, inode_pointers, *file_size);
		if (r != 0)
			return r;
	}

	while (done < length) {
		logical_block = (offset + done) / MFS4_BM_BLOCK_SIZE;
		block_offset = (unsigned int)((offset + done) % MFS4_BM_BLOCK_SIZE);
		amount = MFS4_BM_BLOCK_SIZE - block_offset;
		if (amount > length - done)
			amount = length - done;
		r = mfs4_blockmap_lookup(map, inode_pointers, logical_block,
		    &physical);
		if (r != 0) {
			*bytes_written = done;
			return r;
		}
		allocated = (physical == 0);
		if (allocated) {
			r = mfs4_blockmap_alloc(map, &physical);
			if (r != 0) {
				*bytes_written = done;
				return r;
			}
			memset(raw, 0, sizeof(raw));
		} else if (block_offset == 0 && amount == MFS4_BM_BLOCK_SIZE) {
			/* A complete overwrite does not need the old data read. */
			memset(raw, 0, sizeof(raw));
		} else {
			r = map->io.read_block(map->io.cookie, physical, raw);
			if (r != 0) {
				*bytes_written = done;
				return r;
			}
		}
		memcpy(raw + block_offset, in + done, amount);
		r = map->io.write_block(map->io.cookie, physical, raw);
		if (r != 0) {
			if (allocated)
				(void)mfs4_blockmap_free(map, physical);
			*bytes_written = done;
			return r;
		}
		/* Ordered-data rule: make new/updated data durable before linking it. */
		r = mfs4_flush(map);
		if (r != 0) {
			if (allocated)
				(void)mfs4_blockmap_free(map, physical);
			*bytes_written = done;
			return r;
		}
		if (allocated) {
			r = mfs4_blockmap_set(map, inode_pointers, logical_block,
			    physical);
			if (r != 0) {
				/* A failed metadata write may have linked the pointer.
				 * Prefer a leaked allocation to a dangling pointer. */
				*bytes_written = done;
				return r;
			}
			r = mfs4_flush(map);
			if (r != 0) {
				*bytes_written = done;
				return r;
			}
		}
		done += amount;
		if (offset + done > *file_size)
			*file_size = offset + done;
	}
	if (end > *file_size)
		*file_size = end;
	*bytes_written = done;
	return 0;
}