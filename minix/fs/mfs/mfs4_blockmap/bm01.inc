	if (!mfs4_valid_block(map, block))
		return EIO;
	memset(raw, 0, sizeof(raw));
	r = map->io.write_block(map->io.cookie, block, raw);
	if (r != 0)
		return r;
	return mfs4_flush(map);
}

int
mfs4_blockmap_init(struct mfs4_blockmap *map,
    const struct mfs4_blockmap_io *io, uint64_t data_start,
    uint64_t backup_block, uint64_t zmap_start, uint64_t zmap_blocks)
{
	uint64_t data_blocks, required_zmap;

	if (map == NULL || io == NULL || io->read_block == NULL ||
	    io->write_block == NULL)
		return EINVAL;
	if (data_start == 0 || data_start >= backup_block ||
	    backup_block > (1ULL << 32) || zmap_start < MFS4_IMAP_START ||
	    zmap_blocks == 0)
		return EINVAL;
	data_blocks = backup_block - data_start;
	required_zmap = data_blocks / MFS4_BM_BITS_PER_BLOCK +
	    (data_blocks % MFS4_BM_BITS_PER_BLOCK != 0);
	if (zmap_blocks > (1ULL << 32) || zmap_blocks < required_zmap ||
	    zmap_start > (1ULL << 32) - zmap_blocks ||
	    zmap_start + zmap_blocks > data_start)
		return EINVAL;

	memset(map, 0, sizeof(*map));
	map->io = *io;
	map->data_start = data_start;
	map->backup_block = backup_block;
	map->zmap_start = zmap_start;
	map->zmap_blocks = zmap_blocks;
	return 0;
}

int
mfs4_blockmap_alloc(struct mfs4_blockmap *map, uint32_t *block_out)
{
	uint64_t data_blocks, step, bit_index, map_block;
	uint8_t bitmap[MFS4_BM_BLOCK_SIZE];
	uint64_t cached_map_block = UINT64_MAX;
	int r;

	if (map == NULL || block_out == NULL)
		return EINVAL;
	data_blocks = map->backup_block - map->data_start;
	if (data_blocks == 0)
		return ENOSPC;
	if (map->next_alloc >= data_blocks)
		map->next_alloc = 0;

	for (step = 0; step < data_blocks; step++) {
		unsigned int bit_in_byte, byte_index;
		uint8_t mask;

		bit_index = map->next_alloc + step;
		if (bit_index >= data_blocks)
			bit_index -= data_blocks;
		map_block = map->zmap_start +
		    bit_index / MFS4_BM_BITS_PER_BLOCK;
		if (map_block >= map->zmap_start + map->zmap_blocks ||
		    map_block > UINT32_MAX)
			return EIO;
		if (cached_map_block != map_block) {
			r = map->io.read_block(map->io.cookie,
			    (uint32_t)map_block, bitmap);
			if (r != 0)
				return r;
			cached_map_block = map_block;
		}
		bit_in_byte = (unsigned int)(bit_index % 8U);
		byte_index = (unsigned int)((bit_index %
		    MFS4_BM_BITS_PER_BLOCK) / 8U);
		mask = (uint8_t)(1U << bit_in_byte);
		if ((bitmap[byte_index] & mask) != 0)
			continue;
		bitmap[byte_index] |= mask;
		r = map->io.write_block(map->io.cookie, (uint32_t)map_block,
		    bitmap);
		if (r != 0)
			return r;
		if (map->data_start + bit_index > UINT32_MAX)
			return EOVERFLOW;
		*block_out = (uint32_t)(map->data_start + bit_index);
		map->next_alloc = bit_index + 1;
		if (map->next_alloc == data_blocks)
			map->next_alloc = 0;
		return 0;
	}
	return ENOSPC;
}

int
mfs4_blockmap_free(struct mfs4_blockmap *map, uint32_t block)