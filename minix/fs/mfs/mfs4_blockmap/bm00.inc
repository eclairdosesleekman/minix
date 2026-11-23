/*	$NetBSD$	*/

/*
 * MFS4 allocator and 64-bit block-map engine.
 *
 * This module deliberately uses a small block-I/O callback interface.  The
 * MFS server can back it with LMFS buffers, while the host unit test uses an
 * in-memory image.  All on-disk pointers and bitmap bits are decoded
 * explicitly as little-endian/LSB-first values.
 *
 * Metadata updates are written through the callback directly in this phase.
 * Phase 4 replaces that backend with transaction-shadow/journal writes; this
 * module never claims crash atomicity for the Phase 3 update sequence.
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#define MFS4_ROOT_SINGLE MFS4_BM_SINGLE_SLOT
#define MFS4_ROOT_DOUBLE MFS4_BM_DOUBLE_SLOT
#define MFS4_ROOT_TRIPLE MFS4_BM_TRIPLE_SLOT
#define MFS4_INDIRECT_LEVELS 3U

typedef uint32_t mfs4_ptr_block[MFS4_BM_FANOUT];

static int
mfs4_flush(struct mfs4_blockmap *map)
{
	return map->io.flush(map->io.cookie);
}

static int
mfs4_valid_block(const struct mfs4_blockmap *map, uint32_t block)
{
	return block != 0 && (uint64_t)block >= map->data_start &&
	    (uint64_t)block < map->backup_block;
}

static int
mfs4_read_ptrs(struct mfs4_blockmap *map, uint32_t block,
    uint32_t pointers[MFS4_BM_FANOUT])
{
	uint8_t raw[MFS4_BM_BLOCK_SIZE];
	unsigned int i;
	int r;

	if (!mfs4_valid_block(map, block))
		return EIO;
	r = map->io.read_block(map->io.cookie, block, raw);
	if (r != 0)
		return r;
	for (i = 0; i < MFS4_BM_FANOUT; i++)
		pointers[i] = mfs4_get_le32(raw + i * sizeof(uint32_t));
	return 0;
}

static int
mfs4_write_ptrs(struct mfs4_blockmap *map, uint32_t block,
    const uint32_t pointers[MFS4_BM_FANOUT])
{
	uint8_t raw[MFS4_BM_BLOCK_SIZE];
	unsigned int i;
	int r;

	if (!mfs4_valid_block(map, block))
		return EIO;
	for (i = 0; i < MFS4_BM_FANOUT; i++)
		mfs4_put_le32(raw + i * sizeof(uint32_t), pointers[i]);
	r = map->io.write_block(map->io.cookie, block, raw);
	if (r != 0)
		return r;
	return mfs4_flush(map);
}

static int
mfs4_zero_block(struct mfs4_blockmap *map, uint32_t block)
{
	uint8_t raw[MFS4_BM_BLOCK_SIZE];
	int r;