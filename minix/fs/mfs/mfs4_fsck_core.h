#ifndef _MFS4_FSCK_CORE_H
#define _MFS4_FSCK_CORE_H

#include <stdint.h>

#include <minix/mfs4_disk.h>

struct mfs4_fsck_io {
	void *cookie;
	int (*read_block)(void *cookie, uint64_t block, void *data);
	int (*write_block)(void *cookie, uint64_t block, const void *data);
	int (*flush)(void *cookie);
};

struct mfs4_fsck_report {
	uint64_t total_blocks;
	uint64_t data_blocks;
	uint32_t total_inodes;
	uint32_t used_inodes;
	uint64_t used_data_blocks;
	uint32_t orphan_inodes;
	int journal_pending;
	int journal_replayed;
	int superblock_degraded;
	int clean;
};

/*
 * Verify an MFS4 volume. If replay_journal is nonzero, a fully validated
 * committed redo transaction is replayed before any home metadata is scanned.
 * Structural errors are never modified by this routine.
 *
 * device_blocks may be zero when the caller cannot determine the device size.
 * All callbacks return zero on success and a positive errno value on failure.
 */
int mfs4_fsck_run(const struct mfs4_fsck_io *io, uint64_t device_blocks,
    int replay_journal, struct mfs4_fsck_report *report);

#endif /* _MFS4_FSCK_CORE_H */
