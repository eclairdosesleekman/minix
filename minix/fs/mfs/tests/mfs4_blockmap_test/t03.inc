static int
test_blockmap_fail_closed_contract(void)
{
	const char *name = "blockmap-fail-closed-contract";
	struct test_disk disk;
	struct mfs4_blockmap map;
	struct mfs4_blockmap_io io;
	uint8_t *zmap;
	uint32_t block = 0x12345678U;

	CHECK(name, init_disk(&disk, &map) == 0, "initialize test image");
	io = map.io;
	io.flush = NULL;
	CHECK(name, mfs4_blockmap_init(&map, &io, TEST_DATA_START,
	    TEST_BACKUP, TEST_ZMAP_START, 1) == EINVAL,
	    "missing durability callback is rejected");

	zmap = disk.bytes +
	    (size_t)TEST_ZMAP_START * MFS4_BM_BLOCK_SIZE;
	zmap[0] &= (uint8_t)~1U;
	CHECK(name, mfs4_blockmap_alloc(&map, &block) == EIO,
	    "allocator rejects a cleared reserved root bit");
	CHECK(name, block == 0x12345678U,
	    "failed allocation does not return a block");
	free(disk.bytes);
	return 0;
}

static int
test_blockmap_rollback_keeps_ambiguous_child_allocated(void)
{
	const char *name = "blockmap-rollback-ambiguous-child";
	struct test_disk disk;
	struct mfs4_blockmap map;
	uint32_t roots[MFS4_BM_INODE_POINTERS] = { 0 };
	uint32_t data_block;
	uint64_t triple_first;
	unsigned int base_writes;
	int r;

	CHECK(name, init_disk(&disk, &map) == 0, "initialize test image");
	CHECK(name, mfs4_blockmap_alloc(&map, &data_block) == 0,
	    "allocate data block for mapping");
	base_writes = disk.write_calls;
	/*
	 * First fail the parent-pointer write, then fail its rollback clear.
	 * The test backend copies each block before returning EIO.
	 */
	disk.fail_write_first = base_writes + 5;
	disk.fail_write_second = base_writes + 6;
	triple_first = MFS4_BM_DIRECT + MFS4_BM_FANOUT +
	    (uint64_t)MFS4_BM_FANOUT * MFS4_BM_FANOUT;
	r = mfs4_blockmap_set(&map, roots, triple_first, data_block);
	CHECK(name, r == EIO, "ambiguous parent write is reported");
	CHECK(name, roots[MFS4_BM_TRIPLE_SLOT] == 0,
	    "new triple-indirect root is detached");
	CHECK(name, test_block_is_allocated(&disk, data_block),
	    "mapped data block remains allocated");
	CHECK(name, !test_block_is_allocated(&disk, TEST_DATA_START + 2),
	    "detached root block is freed");
	CHECK(name, test_block_is_allocated(&disk, TEST_DATA_START + 3),
	    "child with ambiguous detach remains allocated");
	free(disk.bytes);
	return 0;
}

static int
test_write_reports_completed_prefix_on_enospc(void)
{
	const char *name = "write-completed-prefix-enospc";
	struct test_disk disk;
	struct mfs4_blockmap map;
	struct mfs4_blockmap_io io;
	uint32_t roots[MFS4_BM_INODE_POINTERS] = { 0 };
	uint8_t payload[2 * MFS4_BM_BLOCK_SIZE];
	uint64_t size = 0;
	size_t written = 0;
	int r;

	CHECK(name, init_disk(&disk, &map) == 0, "initialize test image");
	io = map.io;
	CHECK(name, mfs4_blockmap_init(&map, &io, TEST_DATA_START,
	    TEST_DATA_START + 2, TEST_ZMAP_START, 1) == 0,
	    "constrain volume to one free data block");
	memset(payload, 0xa5, sizeof(payload));
	r = mfs4_blockmap_write(&map, roots, &size, 0, payload,
	    sizeof(payload), &written);
	CHECK(name, r == ENOSPC, "second allocation fails with ENOSPC");
	CHECK(name, written == MFS4_BM_BLOCK_SIZE,
	    "completed first block is reported to caller");
	CHECK(name, size == MFS4_BM_BLOCK_SIZE,
	    "file size matches the completed prefix");
	free(disk.bytes);
	return 0;
}

static int
test_truncate_and_tail_zero(void)
{
	const char *name = "truncate-tail-zero";
	struct test_disk disk;
	struct mfs4_blockmap map;
	uint32_t roots[MFS4_BM_INODE_POINTERS] = { 0 };
	uint64_t size = 0, high = 7ULL * MFS4_BM_BLOCK_SIZE + 11;
	const uint8_t old_data[] = "abcdefghij";
	const uint8_t new_data[] = "XYZ";
	uint8_t out[16];
	uint32_t before, after;
	size_t done;

	CHECK(name, init_disk(&disk, &map) == 0, "initialize test image");
	CHECK(name, mfs4_blockmap_write(&map, roots, &size, 100, old_data,
	    sizeof(old_data) - 1, &done) == 0, "write original data");
	CHECK(name, mfs4_blockmap_lookup(&map, roots, 0, &before) == 0 &&
	    before != 0, "original block is allocated");
	CHECK(name, mfs4_blockmap_truncate(&map, roots, &size, 105) == 0,
	    "shrink to partial block");
	CHECK(name, size == 105, "size shrunk");
	CHECK(name, mfs4_blockmap_truncate(&map, roots, &size, 120) == 0,
	    "extend after shrink");
	memset(out, 0xff, sizeof(out));
	CHECK(name, mfs4_blockmap_read(&map, roots, size, 100, out,
	    sizeof(out), &done) == 0, "read extended tail");
	CHECK(name, done == sizeof(out), "tail read length");
	CHECK(name, memcmp(out, "abcde", 5) == 0, "retained prefix unchanged");
	CHECK(name, out[5] == 0 && out[6] == 0 && out[15] == 0,
	    "discarded tail remains zero after extension");

	CHECK(name, mfs4_blockmap_write(&map, roots, &size, high, new_data,
	    sizeof(new_data), &done) == 0, "write triple-tree range");
	CHECK(name, mfs4_blockmap_lookup(&map, roots, high /
	    MFS4_BM_BLOCK_SIZE, &before) == 0 && before != 0,
	    "high logical block allocated");
	CHECK(name, mfs4_blockmap_truncate(&map, roots, &size, 120) == 0,
	    "truncate sparse high range");
	CHECK(name, mfs4_blockmap_lookup(&map, roots, high /
	    MFS4_BM_BLOCK_SIZE, &after) == 0 && after == 0,
	    "high logical block freed");
	CHECK(name, roots[MFS4_BM_SINGLE_SLOT] == 0,
	    "empty single-indirect root pruned");
	CHECK(name, roots[MFS4_BM_DOUBLE_SLOT] == 0,
	    "empty double-indirect root pruned");
	CHECK(name, roots[MFS4_BM_TRIPLE_SLOT] == 0,
	    "empty triple-indirect root pruned");
	free(disk.bytes);
	return 0;
}

int
main(void)
{
	int failed = 0;
	failed |= test_allocator();
	failed |= test_blockmap_fail_closed_contract();
	failed |= test_blockmap_rollback_keeps_ambiguous_child_allocated();
	failed |= test_write_reports_completed_prefix_on_enospc();
	failed |= test_large_bitmap_index();
	failed |= test_corrupt_pointer_rejection();
	failed |= test_mapping_boundaries();
	failed |= test_sparse_io_and_64bit();
	failed |= test_truncate_and_tail_zero();
	if (failed != 0)
		return 1;
	(void)printf("MFS4 blockmap tests: all passed\n");
	return 0;
}