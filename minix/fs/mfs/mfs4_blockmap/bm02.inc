{
	uint64_t bit_index, map_block;
	unsigned int bit_in_byte, byte_index;
	uint8_t bitmap[MFS4_BM_BLOCK_SIZE], mask;
	int r;

	if (map == NULL || !mfs4_valid_block(map, block))
		return EINVAL;
	/* The first data block permanently stores the root directory. */
	if ((uint64_t)block == map->data_start)
		return EINVAL;
	bit_index = (uint64_t)block - map->data_start;
	map_block = map->zmap_start +
	    bit_index / MFS4_BM_BITS_PER_BLOCK;
	if (map_block >= map->zmap_start + map->zmap_blocks ||
	    map_block > UINT32_MAX)
		return EIO;
	r = map->io.read_block(map->io.cookie, (uint32_t)map_block, bitmap);
	if (r != 0)
		return r;
	bit_in_byte = (unsigned int)(bit_index % 8U);
	byte_index = (unsigned int)((bit_index %
	    MFS4_BM_BITS_PER_BLOCK) / 8U);
	mask = (uint8_t)(1U << bit_in_byte);
	if ((bitmap[byte_index] & mask) == 0)
		return EIO;
	bitmap[byte_index] &= (uint8_t)~mask;
	r = map->io.write_block(map->io.cookie, (uint32_t)map_block, bitmap);
	if (r != 0)
		return r;
	if (bit_index < map->next_alloc)
		map->next_alloc = bit_index;
	return 0;
}

struct mfs4_path {
	unsigned int root_slot;
	unsigned int levels;
	unsigned int index[MFS4_INDIRECT_LEVELS];
};

static int
mfs4_make_path(uint64_t logical_block, struct mfs4_path *path)
{
	uint64_t excess;
	uint64_t double_capacity = (uint64_t)MFS4_BM_FANOUT *
	    MFS4_BM_FANOUT;
	uint64_t triple_capacity = double_capacity * MFS4_BM_FANOUT;

	if (logical_block >= MFS4_BM_MAX_FILE_BLOCKS)
		return EFBIG;
	if (logical_block < MFS4_BM_DIRECT) {
		path->root_slot = (unsigned int)logical_block;
		path->levels = 0;
		return 0;
	}
	excess = logical_block - MFS4_BM_DIRECT;
	if (excess < MFS4_BM_FANOUT) {
		path->root_slot = MFS4_ROOT_SINGLE;
		path->levels = 1;
		path->index[0] = (unsigned int)excess;
		return 0;
	}
	excess -= MFS4_BM_FANOUT;
	if (excess < double_capacity) {
		path->root_slot = MFS4_ROOT_DOUBLE;
		path->levels = 2;
		path->index[0] = (unsigned int)(excess / MFS4_BM_FANOUT);
		path->index[1] = (unsigned int)(excess % MFS4_BM_FANOUT);
		return 0;
	}
	excess -= double_capacity;
	if (excess >= triple_capacity)
		return EFBIG;
	path->root_slot = MFS4_ROOT_TRIPLE;
	path->levels = 3;
	path->index[0] = (unsigned int)(excess / double_capacity);
	path->index[1] = (unsigned int)((excess / MFS4_BM_FANOUT) %
	    MFS4_BM_FANOUT);
	path->index[2] = (unsigned int)(excess % MFS4_BM_FANOUT);
	return 0;
}

static int
mfs4_read_path(struct mfs4_blockmap *map,
    const uint32_t roots[MFS4_BM_INODE_POINTERS],
    const struct mfs4_path *path, uint32_t *physical)
{
	uint32_t current, pointers[MFS4_BM_FANOUT];
	unsigned int level;
	int r;

	current = roots[path->root_slot];
	if (current == 0) {
		*physical = 0;
		return 0;
	}
	for (level = 0; level < path->levels; level++) {
		if (!mfs4_valid_block(map, current))
			return EIO;