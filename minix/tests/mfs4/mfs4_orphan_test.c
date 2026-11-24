#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <minix/mfs4_disk.h>
#include "mfs4_orphan_core.h"

#define TEST_GENERATION 0x123456789abcdef0ULL
#define TEST_INODES 4096U

static void
init_state(uint8_t state[MFS4_BLOCK_SIZE])
{
	memset(state, 0, MFS4_BLOCK_SIZE);
	memcpy(state + 0, MFS4_STATE_MAGIC, 4);
	mfs4_put_le32(state + 4, MFS4_STATE_VERSION);
	mfs4_put_le32(state + MFS4_STATE_FLAGS, MFS4_STATE_CLEAN);
	mfs4_put_le64(state + MFS4_STATE_GENERATION, TEST_GENERATION);
	mfs4_put_le32(state + MFS4_STATE_CRC32C,
	    mfs4_crc32c(state, MFS4_STATE_CRC32C));
}

static void
test_state_head_updates(void)
{
	uint8_t state[MFS4_BLOCK_SIZE];
	uint32_t head;

	init_state(state);
	assert(mfs4_orphan_state_validate(state, TEST_GENERATION,
	    TEST_INODES, &head) == 0);
	assert(head == 0);

	assert(mfs4_orphan_state_set_head(state, TEST_GENERATION,
	    TEST_INODES, 42) == 0);
	assert(mfs4_orphan_state_validate(state, TEST_GENERATION,
	    TEST_INODES, &head) == 0);
	assert(head == 42);
	assert(mfs4_get_le64(state + MFS4_STATE_GENERATION) ==
	    TEST_GENERATION);

	assert(mfs4_orphan_state_set_head(state, TEST_GENERATION,
	    TEST_INODES, 0) == 0);
	assert(mfs4_orphan_state_validate(state, TEST_GENERATION,
	    TEST_INODES, &head) == 0);
	assert(head == 0);
}

static void
test_state_corruption_rejected(void)
{
	uint8_t state[MFS4_BLOCK_SIZE];
	uint32_t head;

	init_state(state);
	state[MFS4_STATE_ORPHAN_HEAD] = 3;
	assert(mfs4_orphan_state_validate(state, TEST_GENERATION,
	    TEST_INODES, &head) == EIO); /* CRC mismatch */

	init_state(state);
	state[100] = 1;
	mfs4_put_le32(state + MFS4_STATE_CRC32C,
	    mfs4_crc32c(state, MFS4_STATE_CRC32C));
	assert(mfs4_orphan_state_validate(state, TEST_GENERATION,
	    TEST_INODES, &head) == EIO); /* reserved bytes */

	init_state(state);
	mfs4_put_le32(state + MFS4_STATE_ORPHAN_HEAD, TEST_INODES + 1);
	mfs4_put_le32(state + MFS4_STATE_CRC32C,
	    mfs4_crc32c(state, MFS4_STATE_CRC32C));
	assert(mfs4_orphan_state_validate(state, TEST_GENERATION,
	    TEST_INODES, &head) == EIO);

	init_state(state);
	assert(mfs4_orphan_state_validate(state, TEST_GENERATION + 1,
	    TEST_INODES, &head) == EIO);
	assert(mfs4_orphan_state_set_head(state, TEST_GENERATION,
	    TEST_INODES, 1) == EINVAL); /* root inode cannot be orphaned */
}

static void
test_reclaim_policy(void)
{
	assert(mfs4_orphan_can_reclaim(I4_ORPHAN, 0, 1, 0));
	assert(!mfs4_orphan_can_reclaim(I4_ORPHAN, 0, 2, 0));
	assert(mfs4_orphan_can_reclaim(I4_ORPHAN, 0, 2, 1));
	assert(mfs4_orphan_can_reclaim(I4_ORPHAN | I4_TRUNCATING,
	    17, 4, 0));
	assert(!mfs4_orphan_can_reclaim(0, 0, 1, 1));
	assert(!mfs4_orphan_can_reclaim(I4_ORPHAN, 1, 1, 1));
}

int
main(void)
{
	test_state_head_updates();
	test_state_corruption_rejected();
	test_reclaim_policy();
	puts("mfs4_orphan_test: all tests passed");
	return 0;
}
