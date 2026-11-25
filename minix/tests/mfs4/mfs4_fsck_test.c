#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <minix/mfs4_disk.h>

#include "mfs4_fsck_core.h"

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

	r = mfs4_fsck_run(&io, TEST_BLOCKS, 0, &report);
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

	r = mfs4_fsck_run(&io, TEST_BLOCKS, 0, &report);
	assert(r == EIO);
	assert(device.writes == 0);
}

int
main(void)
{
	test_clean_volume_is_read_only_verified();
	test_directory_corruption_is_not_repaired();
	test_bad_state_crc_is_rejected_without_writes();
	puts("mfs4_fsck_test: all tests passed");
	return 0;
}
