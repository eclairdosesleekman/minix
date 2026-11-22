		r = mfs4_read_ptrs(map, current, pointers);
		if (r != 0)
			return r;
		current = pointers[path->index[level]];
		if (current == 0) {
			*physical = 0;
			return 0;
		}
		if (!mfs4_valid_block(map, current))
			return EIO;
	}
	*physical = current;
	return 0;
}

int
mfs4_blockmap_lookup(struct mfs4_blockmap *map,
    const uint32_t inode_pointers[MFS4_BM_INODE_POINTERS],
    uint64_t logical_block, uint32_t *physical_block)
{
	struct mfs4_path path;
	uint32_t block;
	int r;

	if (map == NULL || inode_pointers == NULL || physical_block == NULL)
		return EINVAL;
	r = mfs4_make_path(logical_block, &path);
	if (r != 0)
		return r;
	if (path.levels == 0) {
		block = inode_pointers[path.root_slot];
		if (block != 0 && !mfs4_valid_block(map, block))
			return EIO;
		*physical_block = block;
		return 0;
	}
	return mfs4_read_path(map, inode_pointers, &path, physical_block);
}

static int
mfs4_empty_ptrs(const uint32_t pointers[MFS4_BM_FANOUT])
{
	unsigned int i;

	for (i = 0; i < MFS4_BM_FANOUT; i++)
		if (pointers[i] != 0)
			return 0;
	return 1;
}

/*
 * Detach an empty indirect block from its parent before returning its bitmap
 * bit.  parent_block==0 means that the parent is the inode pointer array.
 */
static int
mfs4_detach_indirect(struct mfs4_blockmap *map,
    uint32_t roots[MFS4_BM_INODE_POINTERS], unsigned int root_slot,
    uint32_t parent_block, unsigned int parent_index, uint32_t child_block)
{
	uint32_t pointers[MFS4_BM_FANOUT];
	int r;

	if (parent_block == 0) {
		if (roots[root_slot] != child_block)
			return EIO;
		roots[root_slot] = 0;
	} else {
		if (!mfs4_valid_block(map, parent_block))
			return EIO;
		r = mfs4_read_ptrs(map, parent_block, pointers);
		if (r != 0)
			return r;
		if (pointers[parent_index] != child_block)
			return EIO;
		pointers[parent_index] = 0;
		r = mfs4_write_ptrs(map, parent_block, pointers);
		if (r != 0)
			return r;
	}
	return mfs4_blockmap_free(map, child_block);
}

int
mfs4_blockmap_set(struct mfs4_blockmap *map,
    uint32_t inode_pointers[MFS4_BM_INODE_POINTERS],
    uint64_t logical_block, uint32_t physical_block)
{
	struct mfs4_path path;
	uint32_t current, pointers[MFS4_BM_FANOUT];
	uint32_t created[MFS4_INDIRECT_LEVELS];
	uint32_t parents[MFS4_INDIRECT_LEVELS];
	unsigned int parent_indexes[MFS4_INDIRECT_LEVELS];
	unsigned int created_count, level, j;
	uint32_t current_parent = 0;
	int r, cleanup_error;

	if (map == NULL || inode_pointers == NULL)
		return EINVAL;
	if (physical_block != 0 && !mfs4_valid_block(map, physical_block))
		return EINVAL;