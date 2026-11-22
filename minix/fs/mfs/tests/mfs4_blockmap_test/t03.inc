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