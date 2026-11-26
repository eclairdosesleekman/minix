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
	uint32_t repairs_made;
	uint64_t unreferenced_data_blocks;
};

#define MFS4_FSCK_CHECK_ONLY 0
#define MFS4_FSCK_REPAIR_SAFE 1
#define MFS4_FSCK_REPAIR_ALL 2

/*
 * Verify and, when requested, repair an MFS4 volume. CHECK_ONLY never writes.
 * Both repair modes replay a fully validated committed redo transaction
 * before scanning home metadata. REPAIR_SAFE corrects deterministic metadata
 * redundancies but never frees unreferenced data blocks. REPAIR_ALL also frees
 * unreferenced allocated blocks; callers must make an image backup first.
 * Structural corruption (bad pointers, directories, xattrs or ACL payloads)
 * is never guessed at or rewritten.
 *
 * device_blocks may be zero when the caller cannot determine the device size.
 * All callbacks return zero on success and a positive errno value on failure.
 */
int mfs4_fsck_run(const struct mfs4_fsck_io *io, uint64_t device_blocks,
    int repair_mode, struct mfs4_fsck_report *report);

#endif /* _MFS4_FSCK_CORE_H */
