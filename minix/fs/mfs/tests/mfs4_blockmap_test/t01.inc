		return EIO;
	memcpy(buffer, disk->zmap[block - TEST_ZMAP_START],
	    MFS4_BM_BLOCK_SIZE);
	return 0;
}

static int
bitmap_only_write(void *cookie, uint32_t block, const void *buffer)
{
	struct bitmap_only_disk *disk = cookie;
	if (block < TEST_ZMAP_START || block >= TEST_ZMAP_START + 2)
		return EIO;
	memcpy(disk->zmap[block - TEST_ZMAP_START], buffer,
	    MFS4_BM_BLOCK_SIZE);
	return 0;
}

static int
bitmap_only_flush(void *cookie)
{
	(void)cookie;
	return 0;
}

static int
test_large_bitmap_index(void)
{
	const char *name = "large-bitmap-index";
	struct bitmap_only_disk disk;
	struct mfs4_blockmap_io io;
	struct mfs4_blockmap map;
	uint32_t block;
	unsigned int i;
	uint64_t data_blocks = MFS4_BM_BITS_PER_BLOCK + 232ULL;

	memset(&disk, 0, sizeof(disk));
	/* Mark all first-map bits allocated (including the root data block). */
	memset(disk.zmap[0], 0xff, sizeof(disk.zmap[0]));
	/* In the second map, only bit zero is available; all tail bits are set. */
	memset(disk.zmap[1], 0xff, sizeof(disk.zmap[1]));
	disk.zmap[1][0] &= (uint8_t)~1U;
	for (i = 1; i < MFS4_BM_BLOCK_SIZE * 8U; i++)
		set_bit(disk.zmap[1], i);
	memset(&io, 0, sizeof(io));
	io.cookie = &disk;
	io.read_block = bitmap_only_read;
	io.write_block = bitmap_only_write;
	io.flush = bitmap_only_flush;
	CHECK(name, mfs4_blockmap_init(&map, &io, 100,
	    100 + data_blocks, TEST_ZMAP_START, 2) == 0,
	    "initialize two-block zmap");
	CHECK(name, mfs4_blockmap_alloc(&map, &block) == 0,
	    "allocate across bitmap block boundary");
	CHECK(name, block == 100 + MFS4_BM_BITS_PER_BLOCK,
	    "u64 bit index selects second zmap block");
	CHECK(name, mfs4_blockmap_init(&map, &io, 100,
	    100 + data_blocks, 99, 2) == EINVAL,
	    "zmap below the metadata boundary is rejected");
	CHECK(name, mfs4_blockmap_init(&map, &io, 100,
	    100 + data_blocks, 99 + data_blocks, 2) == EINVAL,
	    "zmap overlapping data area is rejected");
	return 0;
}

static int
test_corrupt_pointer_rejection(void)
{
	const char *name = "corrupt-pointer-rejection";
	struct test_disk disk;
	struct mfs4_blockmap map;
	uint32_t roots[MFS4_BM_INODE_POINTERS] = { 0 };
	uint32_t physical;

	CHECK(name, init_disk(&disk, &map) == 0, "initialize test image");
	roots[0] = TEST_BACKUP;
	CHECK(name, mfs4_blockmap_lookup(&map, roots, 0, &physical) == EIO,
	    "direct pointer at backup superblock is rejected");
	free(disk.bytes);
	return 0;
}

static int
test_allocator(void)
{
	const char *name = "allocator";
	struct test_disk disk;
	struct mfs4_blockmap map;
	uint32_t first, second, again;

	CHECK(name, init_disk(&disk, &map) == 0, "initialize test image");
	CHECK(name, mfs4_blockmap_alloc(&map, &first) == 0, "first allocate");
	CHECK(name, first == TEST_DATA_START + 1, "reserved root block skipped");
	CHECK(name, mfs4_blockmap_alloc(&map, &second) == 0, "second allocate");
	CHECK(name, second == first + 1, "allocation hint advances");
	CHECK(name, mfs4_blockmap_free(&map, TEST_DATA_START) == EINVAL,
	    "reserved root directory block cannot be freed");
	CHECK(name, mfs4_blockmap_free(&map, first) == 0, "free first block");
	CHECK(name, mfs4_blockmap_alloc(&map, &again) == 0, "reuse freed block");
	CHECK(name, again == first, "allocator wraps to freed bit");
	CHECK(name, mfs4_blockmap_free(&map, again) == 0, "free reused block");