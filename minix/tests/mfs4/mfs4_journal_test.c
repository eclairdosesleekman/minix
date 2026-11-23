#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <minix/mfs4_disk.h>
#include "mfs4_journal.h"

#define TEST_BLOCKS 256U

struct memory_disk {
	uint8_t working[TEST_BLOCKS][MFS4_BLOCK_SIZE];
	uint8_t durable[TEST_BLOCKS][MFS4_BLOCK_SIZE];
	int flush_count;
	int fail_flush_at;
	uint64_t fail_write_block;
	int fail_write_occurrence;
	int fail_write_seen;
	int fail_closed;
};

static int
disk_read(void *cookie, uint64_t block, void *data)
{
	struct memory_disk *disk = cookie;

	if (block >= TEST_BLOCKS)
		return EIO;
	memcpy(data, disk->working[block], MFS4_BLOCK_SIZE);
	return 0;
}

static int
disk_write(void *cookie, uint64_t block, const void *data)
{
	struct memory_disk *disk = cookie;

	if (block >= TEST_BLOCKS)
		return EIO;
	if (block == disk->fail_write_block) {
		disk->fail_write_seen++;
		if (disk->fail_write_occurrence == 0 ||
		    disk->fail_write_seen == disk->fail_write_occurrence)
			return EIO;
	}
	memcpy(disk->working[block], data, MFS4_BLOCK_SIZE);
	return 0;
}

static int
disk_flush(void *cookie)
{
	struct memory_disk *disk = cookie;

	disk->flush_count++;
	if (disk->fail_flush_at != 0 &&
	    disk->flush_count == disk->fail_flush_at)
		return EIO;
	memcpy(disk->durable, disk->working, sizeof(disk->working));
	return 0;
}

static void
disk_fail_closed(void *cookie)
{
	struct memory_disk *disk = cookie;

	disk->fail_closed = 1;
}

static struct mfs4_journal_io
make_io(struct memory_disk *disk)
{
	struct mfs4_journal_io io;

	memset(&io, 0, sizeof(io));
	io.cookie = disk;
	io.geometry.total_blocks = TEST_BLOCKS;
	io.geometry.imap_start = 68;
	io.geometry.imap_blocks = 1;
	io.geometry.zmap_start = 69;
	io.geometry.zmap_blocks = 1;
	io.geometry.inode_table_start = 70;
	io.geometry.inode_table_blocks = 2;
	io.geometry.state_block = 72;
	io.geometry.data_start = 73;
	io.geometry.backup_block = TEST_BLOCKS - 1;
	io.geometry.volume_generation = 0x12345678;
	io.read_block = disk_read;
	io.write_block = disk_write;
	io.flush = disk_flush;
	io.fail_closed = disk_fail_closed;
	return io;
}

static void
simulate_crash(struct memory_disk *disk)
{
	memcpy(disk->working, disk->durable, sizeof(disk->working));
	disk->flush_count = 0;
	disk->fail_flush_at = 0;
	disk->fail_write_block = UINT64_MAX;
	disk->fail_write_occurrence = 0;
	disk->fail_write_seen = 0;
	disk->fail_closed = 0;
}

static void
test_committed_transaction(void)
{
	struct memory_disk disk;
	struct mfs4_journal_io io;
	struct mfs4_journal_tx tx;
	uint8_t *image;
	uint32_t i;

	memset(&disk, 0, sizeof(disk));
	disk.fail_write_block = UINT64_MAX;
	io = make_io(&disk);
	assert(mfs4_journal_tx_begin(&tx, &io, 9) == 0);
	assert(mfs4_journal_tx_get_meta(&tx, 73, MFS4_JMETA_DIRECTORY,
	    &image) == 0);
	memcpy(image, "namespace-v1", 12);
	assert(mfs4_journal_tx_commit(&tx) == 0);
	assert(memcmp(disk.durable[73], "namespace-v1", 12) == 0);
	assert(mfs4_journal_pending(&io) == 0);
	assert(mfs4_journal_replay(&io) == 0);
	/* The single-transaction protocol has 5 durability barriers. */
	assert(disk.flush_count == 5);
	for (i = 0; i < MFS4_BLOCK_SIZE; i++) {
		if (i >= 12)
			assert(disk.durable[73][i] == 0);
	}
}

static void
test_uncommitted_images_are_ignored(void)
{
	struct memory_disk disk;
	struct mfs4_journal_io io;
	struct mfs4_journal_tx tx;
	uint8_t *image;

	memset(&disk, 0, sizeof(disk));
	disk.fail_write_block = UINT64_MAX;
	io = make_io(&disk);
	assert(mfs4_journal_tx_begin(&tx, &io, 10) == 0);
	assert(mfs4_journal_tx_get_meta(&tx, 74, MFS4_JMETA_INDIRECT,
	    &image) == 0);
	memcpy(image, "must-not-reach-home", 19);
	/* Header/images flush fails, before the commit record is written. */
	disk.fail_flush_at = 2;
	assert(mfs4_journal_tx_commit(&tx) == EIO);
	assert(disk.fail_closed == 0);
	simulate_crash(&disk);
	assert(mfs4_journal_replay(&io) == 0);
	assert(memcmp(disk.working[74], "must-not-reach-home", 19) != 0);
	assert(mfs4_journal_pending(&io) == 0);
}

static void
test_commit_record_replays_after_home_write_failure(void)
{
	struct memory_disk disk;
	struct mfs4_journal_io io;
	struct mfs4_journal_tx tx;
	uint8_t *image;

	memset(&disk, 0, sizeof(disk));
	disk.fail_write_block = 73;
	disk.fail_write_occurrence = 1;
	io = make_io(&disk);
	assert(mfs4_journal_tx_begin(&tx, &io, 11) == 0);
	assert(mfs4_journal_tx_get_meta(&tx, 73, MFS4_JMETA_DIRECTORY,
	    &image) == 0);
	memcpy(image, "replay-is-idempotent", 20);
	assert(mfs4_journal_tx_commit(&tx) == EIO);
	assert(disk.fail_closed == 1);
	assert(mfs4_journal_pending(&io) == 1);
	simulate_crash(&disk);
	assert(mfs4_journal_pending(&io) == 1);
	assert(mfs4_journal_replay(&io) == 0);
	assert(memcmp(disk.working[73], "replay-is-idempotent", 20) == 0);
	assert(mfs4_journal_pending(&io) == 0);
}

static void
test_replay_after_home_flush_before_clear(void)
{
	struct memory_disk disk;
	struct mfs4_journal_io io;
	struct mfs4_journal_tx tx;
	uint8_t *image;

	memset(&disk, 0, sizeof(disk));
	disk.fail_write_block = MFS4_JOURNAL_COMMIT_BLOCK;
	disk.fail_write_occurrence = 2; /* first write commits; second clears */
	io = make_io(&disk);
	assert(mfs4_journal_tx_begin(&tx, &io, 12) == 0);
	assert(mfs4_journal_tx_get_meta(&tx, 75, MFS4_JMETA_XATTR,
	    &image) == 0);
	memcpy(image, "home-already-updated", 20);
	assert(mfs4_journal_tx_commit(&tx) == EIO);
	assert(memcmp(disk.durable[75], "home-already-updated", 20) == 0);
	assert(mfs4_journal_pending(&io) == 1);
	simulate_crash(&disk);
	assert(mfs4_journal_replay(&io) == 0);
	assert(memcmp(disk.durable[75], "home-already-updated", 20) == 0);
	assert(mfs4_journal_pending(&io) == 0);
}

static void
test_corrupt_image_rejected_before_home_write(void)
{
	struct memory_disk disk;
	struct mfs4_journal_io io;
	struct mfs4_journal_tx tx;
	uint8_t *image;
	uint8_t before[MFS4_BLOCK_SIZE];

	memset(&disk, 0, sizeof(disk));
	disk.fail_write_block = 76; /* retain the committed transaction */
	io = make_io(&disk);
	assert(mfs4_journal_tx_begin(&tx, &io, 13) == 0);
	assert(mfs4_journal_tx_get_meta(&tx, 76, MFS4_JMETA_DIRECTORY,
	    &image) == 0);
	memcpy(image, "integrity-check", 15);
	assert(mfs4_journal_tx_commit(&tx) == EIO);
	memcpy(before, disk.durable[76], sizeof(before));
	simulate_crash(&disk);
	disk.working[MFS4_JOURNAL_IMAGES_START][40] ^= 0x80;
	assert(disk_flush(&disk) == 0);
	simulate_crash(&disk);
	assert(mfs4_journal_replay(&io) == EIO);
	assert(memcmp(disk.working[76], before, sizeof(before)) == 0);
	assert(mfs4_journal_pending(&io) == 1);
}

static void
test_dedup_limit_and_ranges(void)
{
	struct memory_disk disk;
	struct mfs4_journal_io io;
	struct mfs4_journal_tx tx;
	uint8_t *first, *again, *image;
	uint32_t i;

	memset(&disk, 0, sizeof(disk));
	disk.fail_write_block = UINT64_MAX;
	io = make_io(&disk);
	assert(mfs4_journal_tx_begin(&tx, &io, 14) == 0);
	assert(mfs4_journal_tx_get_meta(&tx, 73, MFS4_JMETA_INDIRECT,
	    &first) == 0);
	assert(mfs4_journal_tx_get_meta(&tx, 73, MFS4_JMETA_INDIRECT,
	    &again) == 0);
	assert(first == again && tx.count == 1);
	assert(mfs4_journal_tx_get_meta(&tx, 73, MFS4_JMETA_DIRECTORY,
	    &image) == EINVAL);
	assert(mfs4_journal_tx_get_meta(&tx, 2, MFS4_JMETA_DIRECTORY,
	    &image) == EINVAL);
	for (i = 1; i < MFS4_JOURNAL_MAX_IMAGES; i++)
		assert(mfs4_journal_tx_get_meta(&tx, 73 + i,
		    MFS4_JMETA_INDIRECT, &image) == 0);
	assert(tx.count == MFS4_JOURNAL_MAX_IMAGES);
	assert(mfs4_journal_tx_get_meta(&tx, 73 + i,
	    MFS4_JMETA_INDIRECT, &image) == E2BIG);
	mfs4_journal_tx_abort(&tx);
}

int
main(void)
{
	test_committed_transaction();
	test_uncommitted_images_are_ignored();
	test_commit_record_replays_after_home_write_failure();
	test_replay_after_home_flush_before_clear();
	test_corrupt_image_rejected_before_home_write();
	test_dedup_limit_and_ranges();
	puts("mfs4_journal_test: all tests passed");
	return 0;
}
