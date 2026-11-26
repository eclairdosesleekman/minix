#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <minix/mfs4_disk.h>

#include "mfs4_fsck_core.h"
#include "mfs4_orphan_core.h"

#define TEST_BLOCKS 100U
#define TEST_INODES 32U

struct test_device {
	uint8_t blocks[TEST_BLOCKS][MFS4_BLOCK_SIZE];
	unsigned int writes;
};

static int
test_read(void *cookie, uint64_t block, void *data)
{
	struct test_device *device = cookie;

	if (block >= TEST_BLOCKS)
		return EIO;
	memcpy(data, device->blocks[block], MFS4_BLOCK_SIZE);
	return 0;
}

static int
test_write(void *cookie, uint64_t block, const void *data)
{
	struct test_device *device = cookie;

	if (block >= TEST_BLOCKS)
		return EIO;
	memcpy(device->blocks[block], data, MFS4_BLOCK_SIZE);
	device->writes++;
	return 0;
}

static int
test_flush(void *cookie)
{
	(void)cookie;
	return 0;
}

static void
set_bit(uint8_t *map, uint32_t bit)
{
	map[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
}

static void
clear_bit(uint8_t *map, uint32_t bit)
{
	map[bit >> 3] &= (uint8_t)~(1U << (bit & 7U));
}

static void
make_super(uint8_t raw[MFS4_SUPER_SIZE])
{
	memset(raw, 0, MFS4_SUPER_SIZE);
	memcpy(raw + MFS4_SB_MAGIC, MFS4_SUPER_MAGIC,
	    MFS4_SUPER_MAGIC_SIZE);
	mfs4_put_le16(raw + MFS4_SB_MAJOR, MFS4_SUPER_MAJOR);
	mfs4_put_le16(raw + MFS4_SB_MINOR, MFS4_SUPER_MINOR);
	mfs4_put_le32(raw + MFS4_SB_HEADER_SIZE, MFS4_SUPER_SIZE);
	mfs4_put_le32(raw + MFS4_SB_BLOCK_SIZE, MFS4_BLOCK_SIZE);
	mfs4_put_le16(raw + MFS4_SB_INODE_SIZE, MFS4_INODE_SIZE);
	mfs4_put_le16(raw + MFS4_SB_DIRENT_SIZE, MFS4_DIRENT_SIZE);
	mfs4_put_le64(raw + MFS4_SB_TOTAL_BLOCKS, TEST_BLOCKS);
	mfs4_put_le32(raw + MFS4_SB_TOTAL_INODES, TEST_INODES);
	mfs4_put_le64(raw + MFS4_SB_IMAP_START, 68);
	mfs4_put_le32(raw + MFS4_SB_IMAP_BLOCKS, 1);
	mfs4_put_le32(raw + MFS4_SB_ZMAP_BLOCKS, 1);
	mfs4_put_le64(raw + MFS4_SB_ZMAP_START, 69);
	mfs4_put_le64(raw + MFS4_SB_INODE_TABLE_START, 70);
	mfs4_put_le64(raw + MFS4_SB_INODE_TABLE_BLOCKS, 2);
	mfs4_put_le64(raw + MFS4_SB_STATE_BLOCK, 72);
	mfs4_put_le64(raw + MFS4_SB_DATA_START, 73);
	mfs4_put_le64(raw + MFS4_SB_JOURNAL_START, MFS4_JOURNAL_START);
	mfs4_put_le32(raw + MFS4_SB_JOURNAL_BLOCKS, MFS4_JOURNAL_BLOCKS);
	mfs4_put_le32(raw + MFS4_SB_MAX_TX_IMAGES, MFS4_JOURNAL_MAX_IMAGES);
	mfs4_put_le64(raw + MFS4_SB_BACKUP_BLOCK, TEST_BLOCKS - 1);
	mfs4_put_le32(raw + MFS4_SB_ROOT_INODE, 1);
	mfs4_put_le64(raw + MFS4_SB_MAX_FILE_SIZE, MFS4_MAX_FILE_SIZE);
	mfs4_put_le64(raw + MFS4_SB_VOLUME_GENERATION, 1);
	mfs4_put_le32(raw + MFS4_SB_CRC32C,
	    mfs4_crc32c(raw, MFS4_SB_CRC32C));
}

static void
make_state(uint8_t block[MFS4_BLOCK_SIZE])
{
	memset(block, 0, MFS4_BLOCK_SIZE);
	memcpy(block, MFS4_STATE_MAGIC, 4);
	mfs4_put_le32(block + 4, MFS4_STATE_VERSION);
	mfs4_put_le32(block + MFS4_STATE_ORPHAN_HEAD, 0);
	mfs4_put_le32(block + MFS4_STATE_FLAGS, MFS4_STATE_CLEAN);
	mfs4_put_le64(block + MFS4_STATE_GENERATION, 1);
	mfs4_put_le32(block + MFS4_STATE_CRC32C,
	    mfs4_crc32c(block, MFS4_STATE_CRC32C));
}

static void
make_inode(uint8_t raw[MFS4_INODE_SIZE])
{
	memset(raw, 0, MFS4_INODE_SIZE);
	mfs4_put_le16(raw + MFS4_I_MODE, 0040755);
	mfs4_put_le32(raw + MFS4_I_NLINK, 2);
	mfs4_put_le64(raw + MFS4_I_SIZE, 2 * MFS4_DIRENT_SIZE);
	mfs4_put_le32(raw + MFS4_I_GENERATION, 1);
	mfs4_put_le32(raw + MFS4_I_DIRECT, 73);
}

static void
make_root_directory(uint8_t block[MFS4_BLOCK_SIZE])
{
	memset(block, 0, MFS4_BLOCK_SIZE);
	mfs4_put_le32(block + MFS4_DIRENT_INODE, 1);
	memcpy(block + MFS4_DIRENT_NAME, ".", 1);
	mfs4_put_le32(block + MFS4_DIRENT_SIZE + MFS4_DIRENT_INODE, 1);
	memcpy(block + MFS4_DIRENT_SIZE + MFS4_DIRENT_NAME, "..", 2);
}

static void
make_volume(struct test_device *device)
{
	uint8_t super[MFS4_SUPER_SIZE];
	uint8_t inode[MFS4_INODE_SIZE];
	uint32_t bit;

	memset(device, 0, sizeof(*device));
	make_super(super);
	memcpy(device->blocks[0] + MFS4_SUPER_PRIMARY_OFFSET,
	    super, sizeof(super));
	memcpy(device->blocks[TEST_BLOCKS - 1], super, sizeof(super));

	/* inode 0 and root inode 1 are reserved/allocated. */
	set_bit(device->blocks[68], 0);
	set_bit(device->blocks[68], 1);
	for (bit = TEST_INODES + 1; bit < MFS4_BITS_PER_BLOCK; bit++)
		set_bit(device->blocks[68], bit);

	/* Only the root directory's first data block is allocated. */
	set_bit(device->blocks[69], 0);
	for (bit = 26; bit < MFS4_BITS_PER_BLOCK; bit++)
		set_bit(device->blocks[69], bit);

	make_inode(inode);
	memcpy(device->blocks[70] + MFS4_INODE_SIZE, inode, sizeof(inode));
	make_state(device->blocks[72]);
	make_root_directory(device->blocks[73]);
}

static void
test_clean_volume_is_read_only_verified(void)
{
	struct test_device device;
	struct mfs4_fsck_io io;
	struct mfs4_fsck_report report;
	int r;

	make_volume(&device);
	memset(&io, 0, sizeof(io));
	io.cookie = &device;
	io.read_block = test_read;
	io.write_block = test_write;
	io.flush = test_flush;

	r = mfs4_fsck_run(&io, TEST_BLOCKS, 0, &report);
	assert(r == 0);
	assert(report.total_blocks == TEST_BLOCKS);
	assert(report.total_inodes == TEST_INODES);
	assert(report.used_inodes == 1);
	assert(report.used_data_blocks == 1);
	assert(report.orphan_inodes == 0);
	assert(report.journal_pending == 0);
	assert(report.journal_replayed == 0);
	assert(report.clean == 1);
	assert(report.superblock_degraded == 0);
	assert(device.writes == 0);
}

static void
test_nested_directory_link_counts(void)
{
	struct test_device device;
	struct mfs4_fsck_io io;
	struct mfs4_fsck_report report;
	uint8_t child_inode[MFS4_INODE_SIZE];
	uint8_t *root, *child;
	int r;

	make_volume(&device);
	root = device.blocks[73];
	child = device.blocks[74];

	/* Root nlink is 2 + one child directory. */
	mfs4_put_le32(device.blocks[70] + MFS4_INODE_SIZE + MFS4_I_NLINK, 3);
	mfs4_put_le64(device.blocks[70] + MFS4_INODE_SIZE + MFS4_I_SIZE,
	    3 * MFS4_DIRENT_SIZE);
	mfs4_put_le32(root + 2 * MFS4_DIRENT_SIZE + MFS4_DIRENT_INODE, 2);
	memcpy(root + 2 * MFS4_DIRENT_SIZE + MFS4_DIRENT_NAME, "child", 5);

	memset(child_inode, 0, sizeof(child_inode));
	mfs4_put_le16(child_inode + MFS4_I_MODE, 0040755);
	mfs4_put_le32(child_inode + MFS4_I_NLINK, 2);
	mfs4_put_le64(child_inode + MFS4_I_SIZE, 2 * MFS4_DIRENT_SIZE);
	mfs4_put_le32(child_inode + MFS4_I_GENERATION, 1);
	mfs4_put_le32(child_inode + MFS4_I_DIRECT, 74);
	memcpy(device.blocks[70] + 2 * MFS4_INODE_SIZE,
	    child_inode, sizeof(child_inode));
	set_bit(device.blocks[68], 2);
	set_bit(device.blocks[69], 1);

	memset(child, 0, MFS4_BLOCK_SIZE);
	mfs4_put_le32(child + MFS4_DIRENT_INODE, 2);
	memcpy(child + MFS4_DIRENT_NAME, ".", 1);
	mfs4_put_le32(child + MFS4_DIRENT_SIZE + MFS4_DIRENT_INODE, 1);
	memcpy(child + MFS4_DIRENT_SIZE + MFS4_DIRENT_NAME, "..", 2);

	memset(&io, 0, sizeof(io));
	io.cookie = &device;
	io.read_block = test_read;
	io.write_block = test_write;
	io.flush = test_flush;

	r = mfs4_fsck_run(&io, TEST_BLOCKS, 0, &report);
	assert(r == 0);
	assert(report.used_inodes == 2);
	assert(report.used_data_blocks == 2);
	assert(device.writes == 0);
}

static void
test_detached_orphan_directory_link_counts(void)
{
	struct test_device device;
	struct mfs4_fsck_io io;
	struct mfs4_fsck_report report;
	uint8_t *inode, *child;
	int r;

	make_volume(&device);

	/*
	 * Model rmdir of an open child directory. The parent no longer has a
	 * name for it and its link count has reached zero, but its on-disk "."
	 * and ".." entries remain until orphan cleanup can reclaim the inode.
	 */
	inode = device.blocks[70] + 2 * MFS4_INODE_SIZE;
	mfs4_put_le16(inode + MFS4_I_MODE, 0040755);
	mfs4_put_le16(inode + MFS4_I_FLAGS, I4_ORPHAN);
	mfs4_put_le32(inode + MFS4_I_NLINK, 0);
	mfs4_put_le64(inode + MFS4_I_SIZE, 2 * MFS4_DIRENT_SIZE);
	mfs4_put_le32(inode + MFS4_I_GENERATION, 1);
	mfs4_put_le32(inode + MFS4_I_DIRECT, 74);
	set_bit(device.blocks[68], 2);
	set_bit(device.blocks[69], 1);

	child = device.blocks[74];
	memset(child, 0, MFS4_BLOCK_SIZE);
	mfs4_put_le32(child + MFS4_DIRENT_INODE, 2);
	memcpy(child + MFS4_DIRENT_NAME, ".", 1);
	mfs4_put_le32(child + MFS4_DIRENT_SIZE +
	    MFS4_DIRENT_INODE, 1);
	memcpy(child + MFS4_DIRENT_SIZE + MFS4_DIRENT_NAME, "..", 2);
	assert(mfs4_orphan_state_set_head(device.blocks[72], 1,
	    TEST_INODES, 2) == 0);

	memset(&io, 0, sizeof(io));
	io.cookie = &device;
	io.read_block = test_read;
	io.write_block = test_write;
	io.flush = test_flush;

	r = mfs4_fsck_run(&io, TEST_BLOCKS, 0, &report);
	assert(r == 0);
	assert(report.used_inodes == 2);
	assert(report.orphan_inodes == 1);
	assert(report.used_data_blocks == 2);
	assert(device.writes == 0);
}

static void
test_directory_corruption_is_not_repaired(void)
{
	struct test_device device;
	struct mfs4_fsck_io io;
	struct mfs4_fsck_report report;
	int r;

	make_volume(&device);
	/* Point the root's ".." entry at a free inode. */
	mfs4_put_le32(device.blocks[73] + MFS4_DIRENT_SIZE +
	    MFS4_DIRENT_INODE, 2);
	memset(&io, 0, sizeof(io));
	io.cookie = &device;
	io.read_block = test_read;
	io.write_block = test_write;
	io.flush = test_flush;

	r = mfs4_fsck_run(&io, TEST_BLOCKS, MFS4_FSCK_REPAIR_ALL, &report);
	assert(r == EIO);
	assert(device.writes == 0);
}

static void
test_bad_state_crc_is_rejected_without_writes(void)
{
	struct test_device device;
	struct mfs4_fsck_io io;
	struct mfs4_fsck_report report;
	int r;

	make_volume(&device);
	device.blocks[72][100] ^= 0x80;
	memset(&io, 0, sizeof(io));
	io.cookie = &device;
	io.read_block = test_read;
	io.write_block = test_write;
	io.flush = test_flush;

	r = mfs4_fsck_run(&io, TEST_BLOCKS, MFS4_FSCK_REPAIR_ALL, &report);
	assert(r == EIO);
	assert(device.writes == 0);
}

static void
test_safe_repairs_redundant_metadata(void)
{
	struct test_device device;
	struct mfs4_fsck_io io;
	struct mfs4_fsck_report report;
	uint8_t *root_inode, super[MFS4_SUPER_SIZE];
	uint32_t flags;
	int r;

	make_volume(&device);
	/* Reconstruct reserved bitmap bits, link count and the backup superblock. */
	clear_bit(device.blocks[68], 1);
	clear_bit(device.blocks[69], 0);
	root_inode = device.blocks[70] + MFS4_INODE_SIZE;
	mfs4_put_le32(root_inode + MFS4_I_NLINK, 7);
	memset(device.blocks[TEST_BLOCKS - 1], 0, MFS4_BLOCK_SIZE);

	memset(&io, 0, sizeof(io));
	io.cookie = &device;
	io.read_block = test_read;
	io.write_block = test_write;
	io.flush = test_flush;

	r = mfs4_fsck_run(&io, TEST_BLOCKS, MFS4_FSCK_REPAIR_SAFE, &report);
	assert(r == 0);
	assert(report.repairs_made >= 3);
	assert(device.writes != 0);
	assert((device.blocks[68][0] & 0x03U) == 0x03U);
	assert((device.blocks[69][0] & 0x01U) == 0x01U);
	assert(mfs4_get_le32(device.blocks[70] + MFS4_INODE_SIZE +
	    MFS4_I_NLINK) == 2);
	memcpy(super, device.blocks[0] + MFS4_SUPER_PRIMARY_OFFSET,
	    sizeof(super));
	assert(memcmp(device.blocks[TEST_BLOCKS - 1], super,
	    sizeof(super)) == 0);
	flags = mfs4_get_le32(device.blocks[72] + MFS4_STATE_FLAGS);
	assert((flags & MFS4_STATE_CLEAN) != 0);
	assert(report.clean == 1);
}

static void
test_primary_superblock_is_restored_from_backup(void)
{
	struct test_device device;
	struct mfs4_fsck_io io;
	struct mfs4_fsck_report report;
	uint8_t primary[MFS4_SUPER_SIZE];
	uint8_t *raw;
	int r;

	make_volume(&device);
	raw = device.blocks[0] + MFS4_SUPER_PRIMARY_OFFSET;
	memset(raw, 0, MFS4_SUPER_SIZE);
	memset(&io, 0, sizeof(io));
	io.cookie = &device;
	io.read_block = test_read;
	io.write_block = test_write;
	io.flush = test_flush;

	r = mfs4_fsck_run(&io, TEST_BLOCKS, MFS4_FSCK_REPAIR_SAFE, &report);
	assert(r == 0);
	assert(report.repairs_made != 0);
	memcpy(primary, device.blocks[TEST_BLOCKS - 1], sizeof(primary));
	assert(memcmp(raw, primary, sizeof(primary)) == 0);
	assert(report.superblock_degraded == 0);
}

static void
test_inode_and_referenced_data_bitmap_bits_are_restored(void)
{
	struct test_device device;
	struct mfs4_fsck_io io;
	struct mfs4_fsck_report report;
	uint8_t child_inode[MFS4_INODE_SIZE];
	uint8_t *root, *child;
	int r;

	make_volume(&device);
	root = device.blocks[73];
	child = device.blocks[74];

	/* Add one child directory, then lose both its imap and data zmap bits. */
	mfs4_put_le32(device.blocks[70] + MFS4_INODE_SIZE + MFS4_I_NLINK, 3);
	mfs4_put_le64(device.blocks[70] + MFS4_INODE_SIZE + MFS4_I_SIZE,
	    3 * MFS4_DIRENT_SIZE);
	mfs4_put_le32(root + 2 * MFS4_DIRENT_SIZE + MFS4_DIRENT_INODE, 2);
	memcpy(root + 2 * MFS4_DIRENT_SIZE + MFS4_DIRENT_NAME, "child", 5);

	memset(child_inode, 0, sizeof(child_inode));
	mfs4_put_le16(child_inode + MFS4_I_MODE, 0040755);
	mfs4_put_le32(child_inode + MFS4_I_NLINK, 2);
	mfs4_put_le64(child_inode + MFS4_I_SIZE, 2 * MFS4_DIRENT_SIZE);
	mfs4_put_le32(child_inode + MFS4_I_GENERATION, 1);
	mfs4_put_le32(child_inode + MFS4_I_DIRECT, 74);
	memcpy(device.blocks[70] + 2 * MFS4_INODE_SIZE,
	    child_inode, sizeof(child_inode));

	memset(child, 0, MFS4_BLOCK_SIZE);
	mfs4_put_le32(child + MFS4_DIRENT_INODE, 2);
	memcpy(child + MFS4_DIRENT_NAME, ".", 1);
	mfs4_put_le32(child + MFS4_DIRENT_SIZE + MFS4_DIRENT_INODE, 1);
	memcpy(child + MFS4_DIRENT_SIZE + MFS4_DIRENT_NAME, "..", 2);

	clear_bit(device.blocks[68], 2);
	clear_bit(device.blocks[69], 1);
	memset(&io, 0, sizeof(io));
	io.cookie = &device;
	io.read_block = test_read;
	io.write_block = test_write;
	io.flush = test_flush;

	r = mfs4_fsck_run(&io, TEST_BLOCKS, MFS4_FSCK_REPAIR_SAFE, &report);
	assert(r == 0);
	assert(report.used_inodes == 2);
	assert(report.used_data_blocks == 2);
	assert(report.repairs_made != 0);
	assert((device.blocks[68][0] & (1U << 2)) != 0);
	assert((device.blocks[69][0] & (1U << 1)) != 0);
}

static void
test_preen_preserves_unreferenced_blocks_but_yes_frees_them(void)
{
	struct test_device device;
	struct mfs4_fsck_io io;
	struct mfs4_fsck_report report;
	int r;

	make_volume(&device);
	/* zmap bit 1 names data block 74, which no inode references. */
	set_bit(device.blocks[69], 1);
	memset(&io, 0, sizeof(io));
	io.cookie = &device;
	io.read_block = test_read;
	io.write_block = test_write;
	io.flush = test_flush;

	r = mfs4_fsck_run(&io, TEST_BLOCKS, MFS4_FSCK_REPAIR_SAFE, &report);
	assert(r == EIO);
	assert(report.unreferenced_data_blocks == 1);
	assert(device.writes == 0);
	assert((device.blocks[69][0] & 0x02U) != 0);

	r = mfs4_fsck_run(&io, TEST_BLOCKS, MFS4_FSCK_REPAIR_ALL, &report);
	assert(r == 0);
	assert(report.repairs_made != 0);
	assert(report.unreferenced_data_blocks == 0);
	assert((device.blocks[69][0] & 0x02U) == 0);
	assert(report.clean == 1);
}

static void
test_unknown_incompat_feature_is_never_repaired(void)
{
	struct test_device device;
	struct mfs4_fsck_io io;
	struct mfs4_fsck_report report;
	uint8_t *primary, *backup;
	int r;

	make_volume(&device);
	primary = device.blocks[0] + MFS4_SUPER_PRIMARY_OFFSET;
	backup = device.blocks[TEST_BLOCKS - 1];
	mfs4_put_le32(primary + MFS4_SB_INCOMPAT_FEATURES, 1);
	mfs4_put_le32(primary + MFS4_SB_CRC32C, 0);
	mfs4_put_le32(primary + MFS4_SB_CRC32C,
	    mfs4_crc32c(primary, MFS4_SB_CRC32C));
	mfs4_put_le32(backup + MFS4_SB_INCOMPAT_FEATURES, 1);
	mfs4_put_le32(backup + MFS4_SB_CRC32C, 0);
	mfs4_put_le32(backup + MFS4_SB_CRC32C,
	    mfs4_crc32c(backup, MFS4_SB_CRC32C));
	memset(&io, 0, sizeof(io));
	io.cookie = &device;
	io.read_block = test_read;
	io.write_block = test_write;
	io.flush = test_flush;

	r = mfs4_fsck_run(&io, TEST_BLOCKS, MFS4_FSCK_REPAIR_ALL, &report);
	assert(r == EOPNOTSUPP);
	assert(device.writes == 0);
}

int
main(void)
{
	test_clean_volume_is_read_only_verified();
	test_nested_directory_link_counts();
	test_detached_orphan_directory_link_counts();
	test_directory_corruption_is_not_repaired();
	test_bad_state_crc_is_rejected_without_writes();
	test_safe_repairs_redundant_metadata();
	test_primary_superblock_is_restored_from_backup();
	test_inode_and_referenced_data_bitmap_bits_are_restored();
	test_preen_preserves_unreferenced_blocks_but_yes_frees_them();
	test_unknown_incompat_feature_is_never_repaired();
	puts("mfs4_fsck_test: all tests passed");
	return 0;
}
