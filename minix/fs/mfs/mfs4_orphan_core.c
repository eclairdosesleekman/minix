#include <errno.h>
#include <string.h>

#include <minix/mfs4_disk.h>

#include "mfs4_orphan_core.h"

int
mfs4_orphan_state_validate(const uint8_t state[MFS4_BLOCK_SIZE],
    uint64_t volume_generation, uint32_t total_inodes, uint32_t *head_out)
{
	uint32_t flags, head;
	unsigned int i;

	if (state == NULL || head_out == NULL || total_inodes == 0 ||
	    volume_generation == 0)
		return EINVAL;

	if (memcmp(state + 0, MFS4_STATE_MAGIC, 4) != 0 ||
	    mfs4_get_le32(state + 4) != MFS4_STATE_VERSION ||
	    mfs4_get_le64(state + MFS4_STATE_GENERATION) !=
	    volume_generation ||
	    mfs4_get_le32(state + MFS4_STATE_CRC32C) !=
	    mfs4_crc32c(state, MFS4_STATE_CRC32C))
		return EIO;

	flags = mfs4_get_le32(state + MFS4_STATE_FLAGS);
	head = mfs4_get_le32(state + MFS4_STATE_ORPHAN_HEAD);
	if ((flags & ~MFS4_STATE_CLEAN) != 0 ||
	    head > total_inodes || head == 1)
		return EIO;

	for (i = 24; i < MFS4_STATE_CRC32C; i++) {
		if (state[i] != 0)
			return EIO;
	}

	*head_out = head;
	return 0;
}

int
mfs4_orphan_state_set_head(uint8_t state[MFS4_BLOCK_SIZE],
    uint64_t volume_generation, uint32_t total_inodes, uint32_t head)
{
	uint32_t old_head;
	int r;

	if (head > total_inodes || head == 1)
		return EINVAL;
	if ((r = mfs4_orphan_state_validate(state, volume_generation,
	    total_inodes, &old_head)) != 0)
		return r;

	mfs4_put_le32(state + MFS4_STATE_ORPHAN_HEAD, head);
	mfs4_put_le32(state + MFS4_STATE_CRC32C,
	    mfs4_crc32c(state, MFS4_STATE_CRC32C));
	return 0;
}

int
mfs4_orphan_can_reclaim(uint16_t inode_flags, uint32_t nlink,
    unsigned int open_refs, int mount_recovery)
{
	if ((inode_flags & I4_ORPHAN) == 0)
		return 0;
	if (inode_flags & I4_TRUNCATING)
		return 1;
	if (nlink != 0)
		return 0;
	return mount_recovery || open_refs <= 1;
}
