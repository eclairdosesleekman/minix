    uint64_t first, uint64_t end)
{
	uint32_t root = inode_pointers[root_slot];
	int empty, r;

	if (root == 0)
		return 0;
	r = mfs4_prune_node(map, root, level, root_first, first, end, &empty);
	if (r != 0)
		return r;
	if (empty) {
		inode_pointers[root_slot] = 0;
		r = mfs4_blockmap_free(map, root);
	}
	return r;
}

static int
mfs4_zero_tail(struct mfs4_blockmap *map,
    const uint32_t inode_pointers[MFS4_BM_INODE_POINTERS], uint64_t size)
{
	uint64_t logical_block;
	uint32_t physical;
	uint8_t raw[MFS4_BM_BLOCK_SIZE];
	unsigned int offset;
	int r;

	offset = (unsigned int)(size % MFS4_BM_BLOCK_SIZE);
	if (offset == 0 || size == 0)
		return 0;
	logical_block = size / MFS4_BM_BLOCK_SIZE;
	r = mfs4_blockmap_lookup(map, inode_pointers, logical_block, &physical);
	if (r != 0 || physical == 0)
		return r;
	r = map->io.read_block(map->io.cookie, physical, raw);
	if (r != 0)
		return r;
	memset(raw + offset, 0, MFS4_BM_BLOCK_SIZE - offset);
	r = map->io.write_block(map->io.cookie, physical, raw);
	if (r != 0)
		return r;
	return mfs4_flush(map);
}

int
mfs4_blockmap_read(struct mfs4_blockmap *map,
    const uint32_t inode_pointers[MFS4_BM_INODE_POINTERS],
    uint64_t file_size, uint64_t offset, void *buffer, size_t length,
    size_t *bytes_read)
{
	uint8_t raw[MFS4_BM_BLOCK_SIZE];
	uint8_t *out = (uint8_t *)buffer;
	uint64_t logical_block;
	uint32_t physical;
	size_t amount, done = 0;
	unsigned int block_offset;
	int r;

	if (bytes_read != NULL)
		*bytes_read = 0;
	if (map == NULL || inode_pointers == NULL ||
	    (length != 0 && buffer == NULL) || bytes_read == NULL)
		return EINVAL;
	if (file_size > MFS4_BM_MAX_FILE_SIZE || offset > MFS4_BM_MAX_FILE_SIZE)
		return EFBIG;
	if (offset >= file_size || length == 0)
		return 0;
	if ((uint64_t)length > file_size - offset)
		length = (size_t)(file_size - offset);

	while (done < length) {
		logical_block = (offset + done) / MFS4_BM_BLOCK_SIZE;
		block_offset = (unsigned int)((offset + done) % MFS4_BM_BLOCK_SIZE);
		amount = MFS4_BM_BLOCK_SIZE - block_offset;
		if (amount > length - done)
			amount = length - done;
		r = mfs4_blockmap_lookup(map, inode_pointers, logical_block,
		    &physical);
		if (r != 0) {
			*bytes_read = done;
			return r;
		}
		if (physical == 0) {
			memset(out + done, 0, amount);
		} else {
			r = map->io.read_block(map->io.cookie, physical, raw);
			if (r != 0) {
				*bytes_read = done;
				return r;
			}
			memcpy(out + done, raw + block_offset, amount);
		}
		done += amount;
	}
	*bytes_read = done;
	return 0;
}

int
mfs4_blockmap_write(struct mfs4_blockmap *map,