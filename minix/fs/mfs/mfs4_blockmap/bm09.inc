
int
mfs4_blockmap_truncate(struct mfs4_blockmap *map,
    uint32_t inode_pointers[MFS4_BM_INODE_POINTERS], uint64_t *file_size,
    uint64_t new_size)
{
	uint64_t first_free, end_block;
	unsigned int i;
	int r;

	if (map == NULL || inode_pointers == NULL || file_size == NULL)
		return EINVAL;
	if (*file_size > MFS4_BM_MAX_FILE_SIZE ||
	    new_size > MFS4_BM_MAX_FILE_SIZE)
		return EFBIG;
	if (new_size == *file_size)
		return 0;

	/* Zero the retained tail both on shrink and on subsequent extension. */
	if (new_size < *file_size) {
		r = mfs4_zero_tail(map, inode_pointers, new_size);
		if (r != 0)
			return r;
		first_free = new_size / MFS4_BM_BLOCK_SIZE +
		    (new_size % MFS4_BM_BLOCK_SIZE != 0);
		end_block = *file_size / MFS4_BM_BLOCK_SIZE +
		    (*file_size % MFS4_BM_BLOCK_SIZE != 0);
		if (first_free < end_block) {
			/* Direct slots are removed first. */
			for (i = 0; i < MFS4_BM_DIRECT; i++) {
				if ((uint64_t)i < first_free || (uint64_t)i >= end_block ||
				    inode_pointers[i] == 0)
					continue;
				{
					uint32_t old = inode_pointers[i];
					inode_pointers[i] = 0;
					r = mfs4_blockmap_free(map, old);
					if (r != 0)
						return r;
				}
			}
			r = mfs4_prune_root(map, inode_pointers,
			    MFS4_ROOT_SINGLE, 1, MFS4_BM_DIRECT,
			    first_free, end_block);
			if (r != 0)
				return r;
			r = mfs4_prune_root(map, inode_pointers,
			    MFS4_ROOT_DOUBLE, 2,
			    MFS4_BM_DIRECT + MFS4_BM_FANOUT,
			    first_free, end_block);
			if (r != 0)
				return r;
			r = mfs4_prune_root(map, inode_pointers,
			    MFS4_ROOT_TRIPLE, 3,
			    MFS4_BM_DIRECT + MFS4_BM_FANOUT +
			    (uint64_t)MFS4_BM_FANOUT * MFS4_BM_FANOUT,
			    first_free, end_block);
			if (r != 0)
				return r;
		}
	} else {
		r = mfs4_zero_tail(map, inode_pointers, *file_size);
		if (r != 0)
			return r;
	}
	*file_size = new_size;
	return mfs4_flush(map);
}
