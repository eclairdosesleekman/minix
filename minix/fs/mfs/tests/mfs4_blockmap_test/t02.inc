	CHECK(name, mfs4_blockmap_free(&map, again) == EIO,
	    "double free is rejected");
	free(disk.bytes);
	return 0;
}

static int
test_mapping_boundaries(void)
{
	const char *name = "mapping-boundaries";
	struct test_disk disk;
	struct mfs4_blockmap map;
	uint32_t roots[MFS4_BM_INODE_POINTERS] = { 0 };
	uint32_t physical, block;
	uint64_t logical[] = {
		0, 6, 7, 7 + 1023,
		7 + 1024,
		7 + 1024 + 1024ULL * 1024ULL - 1,
		7 + 1024 + 1024ULL * 1024ULL,
		MFS4_BM_MAX_FILE_BLOCKS - 1
	};
	unsigned int i;

	CHECK(name, init_disk(&disk, &map) == 0, "initialize test image");
	for (i = 0; i < sizeof(logical) / sizeof(logical[0]); i++) {
		CHECK(name, mfs4_blockmap_alloc(&map, &physical) == 0,
		    "allocate data block");
		CHECK(name, mfs4_blockmap_set(&map, roots, logical[i], physical) == 0,
		    "insert logical block");
		CHECK(name, mfs4_blockmap_lookup(&map, roots, logical[i], &block) == 0,
		    "lookup logical block");
		CHECK(name, block == physical, "lookup returns exact physical block");
	}
	CHECK(name, roots[0] != 0 && roots[6] != 0, "direct pointers populated");
	CHECK(name, roots[7] != 0, "single-indirect root populated");
	CHECK(name, roots[8] != 0, "double-indirect root populated");
	CHECK(name, roots[9] != 0, "triple-indirect root populated");
	CHECK(name, mfs4_blockmap_lookup(&map, roots,
	    MFS4_BM_MAX_FILE_BLOCKS, &block) == EFBIG,
	    "mapping at 1 TiB is rejected");
	CHECK(name, mfs4_blockmap_set(&map, roots, logical[2], 0) == 0,
	    "remove single-indirect leaf");
	CHECK(name, mfs4_blockmap_set(&map, roots, logical[3], 0) == 0,
	    "remove final single-indirect leaf");
	CHECK(name, roots[MFS4_BM_SINGLE_SLOT] == 0,
	    "empty single-indirect tree is pruned");
	CHECK(name, mfs4_blockmap_set(&map, roots, logical[4], 0) == 0,
	    "remove double-indirect leaf");
	CHECK(name, mfs4_blockmap_set(&map, roots, logical[5], 0) == 0,
	    "remove final double-indirect leaf");
	CHECK(name, roots[MFS4_BM_DOUBLE_SLOT] == 0,
	    "empty double-indirect tree is pruned");
	CHECK(name, mfs4_blockmap_set(&map, roots, logical[6], 0) == 0,
	    "remove first triple-indirect leaf");
	CHECK(name, mfs4_blockmap_set(&map, roots, logical[7], 0) == 0,
	    "remove final triple-indirect leaf");
	CHECK(name, roots[MFS4_BM_TRIPLE_SLOT] == 0,
	    "empty triple-indirect tree is pruned");
	free(disk.bytes);
	return 0;
}

static int
test_sparse_io_and_64bit(void)
{
	const char *name = "sparse-io-64bit";
	struct test_disk disk;
	struct mfs4_blockmap map;
	uint32_t roots[MFS4_BM_INODE_POINTERS] = { 0 };
	uint64_t size = 0, high = (1ULL << 33) + 321;
	const uint8_t payload[] = { 0xa5, 0x5a, 0x33 };
	uint8_t result[12];
	size_t done;

	CHECK(name, init_disk(&disk, &map) == 0, "initialize test image");
	CHECK(name, mfs4_blockmap_write(&map, roots, &size, high, payload,
	    sizeof(payload), &done) == 0, "write beyond 4 GiB");
	CHECK(name, done == sizeof(payload), "all bytes written");
	CHECK(name, size == high + sizeof(payload), "64-bit size updated");
	memset(result, 0xff, sizeof(result));
	CHECK(name, mfs4_blockmap_read(&map, roots, size, high - 4, result,
	    sizeof(result), &done) == 0, "read sparse range");
	CHECK(name, done == 7, "sparse read is clipped at EOF");
	CHECK(name, result[0] == 0 && result[1] == 0 && result[2] == 0 &&
	    result[3] == 0, "unallocated gap reads as zero");
	CHECK(name, result[4] == payload[0] && result[5] == payload[1] &&
	    result[6] == payload[2], "written bytes round trip");
	CHECK(name, mfs4_blockmap_read(&map, roots, size, high + 3, result,
	    1, &done) == 0 && done == 0, "read at EOF returns no bytes");
	CHECK(name, mfs4_blockmap_write(&map, roots, &size,
	    MFS4_BM_MAX_FILE_SIZE - 1, payload, 1, &done) == 0,
	    "write final byte below 1 TiB");
	CHECK(name, size == MFS4_BM_MAX_FILE_SIZE, "exact 1 TiB size accepted");
	CHECK(name, mfs4_blockmap_write(&map, roots, &size,
	    MFS4_BM_MAX_FILE_SIZE, payload, 1, &done) == EFBIG,
	    "write at 1 TiB rejected");
	free(disk.bytes);
	return 0;
}