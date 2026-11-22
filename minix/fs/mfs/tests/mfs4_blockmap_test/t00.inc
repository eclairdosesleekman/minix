#include "mfs4_blockmap.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_TOTAL_BLOCKS 512U
#define TEST_ZMAP_START   68U
#define TEST_DATA_START   100U
#define TEST_BACKUP       500U

struct test_disk {
	uint8_t *bytes;
	unsigned int flushes;
};

static int
fail(const char *test, const char *message, int line)
{
	(void)fprintf(stderr, "FAIL %s:%d: %s\n", test, line, message);
	return 1;
}

#define CHECK(test, condition, message) \
	do { if (!(condition)) return fail((test), (message), __LINE__); } while (0)

static int
test_read(void *cookie, uint32_t block, void *buffer)
{
	struct test_disk *disk = cookie;
	if (block >= 512U)
		return EIO;
	memcpy(buffer, disk->bytes + (size_t)block * MFS4_BM_BLOCK_SIZE,
	    MFS4_BM_BLOCK_SIZE);
	return 0;
}

static int
test_write(void *cookie, uint32_t block, const void *buffer)
{
	struct test_disk *disk = cookie;
	if (block >= 512U)
		return EIO;
	memcpy(disk->bytes + (size_t)block * MFS4_BM_BLOCK_SIZE, buffer,
	    MFS4_BM_BLOCK_SIZE);
	return 0;
}

static int
test_flush(void *cookie)
{
	struct test_disk *disk = cookie;
	disk->flushes++;
	return 0;
}

static void
set_bit(uint8_t *bitmap, unsigned int bit)
{
	bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
}

static int
init_disk(struct test_disk *disk, struct mfs4_blockmap *map)
{
	struct mfs4_blockmap_io io;
	uint8_t *zmap;
	unsigned int i;

	disk->bytes = calloc(TEST_TOTAL_BLOCKS, MFS4_BM_BLOCK_SIZE);
	if (disk->bytes == NULL)
		return ENOMEM;
	disk->flushes = 0;
	zmap = disk->bytes + (size_t)TEST_ZMAP_START * MFS4_BM_BLOCK_SIZE;
	/* Formatter reserves root directory data block at data index zero. */
	set_bit(zmap, 0);
	for (i = TEST_BACKUP - TEST_DATA_START;
	    i < MFS4_BM_BITS_PER_BLOCK; i++)
		set_bit(zmap, i);
	memset(&io, 0, sizeof(io));
	io.cookie = disk;
	io.read_block = test_read;
	io.write_block = test_write;
	io.flush = test_flush;
	return mfs4_blockmap_init(map, &io, TEST_DATA_START, TEST_BACKUP,
	    TEST_ZMAP_START, 1);
}


struct bitmap_only_disk {
	uint8_t zmap[2][MFS4_BM_BLOCK_SIZE];
};

static int
bitmap_only_read(void *cookie, uint32_t block, void *buffer)
{
	struct bitmap_only_disk *disk = cookie;
	if (block < TEST_ZMAP_START || block >= TEST_ZMAP_START + 2)