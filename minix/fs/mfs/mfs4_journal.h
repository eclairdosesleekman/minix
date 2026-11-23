/* MFS4 fixed redo journal: transaction API and byte-level journal contract. */
#ifndef _MFS4_JOURNAL_H
#define _MFS4_JOURNAL_H

#include <stddef.h>
#include <stdint.h>
#include <minix/mfs4_disk.h>

#define MFS4_JOURNAL_VERSION       1U
#define MFS4_JOURNAL_HEADER_MAGIC  "MFS4JHDR"
#define MFS4_JOURNAL_COMMIT_MAGIC  "MFS4JCMT"
#define MFS4_JOURNAL_COMMIT_MARKER 0x4d465334U

#define MFS4_JMETA_IMAP      1U
#define MFS4_JMETA_ZMAP      2U
#define MFS4_JMETA_INODE     3U
#define MFS4_JMETA_INDIRECT  4U
#define MFS4_JMETA_DIRECTORY 5U
#define MFS4_JMETA_XATTR     6U
#define MFS4_JMETA_STATE     7U

#define MFS4_JH_VERSION       8U
#define MFS4_JH_COUNT         12U
#define MFS4_JH_SEQUENCE      16U
#define MFS4_JH_VOLUME_GEN    24U
#define MFS4_JH_DESC_BYTES    32U
#define MFS4_JH_FLAGS         36U
#define MFS4_JH_DESCRIPTORS   40U
#define MFS4_JH_CRC           4092U
#define MFS4_JDESC_SIZE       16U

#define MFS4_JC_SEQUENCE      8U
#define MFS4_JC_COUNT         16U
#define MFS4_JC_HEADER_CRC    20U
#define MFS4_JC_IMAGES_CRC    24U
#define MFS4_JC_MARKER         28U
#define MFS4_JC_RESERVED       32U
#define MFS4_JC_CRC            4092U

struct mfs4_journal_geometry {
	uint64_t total_blocks;
	uint64_t imap_start;
	uint64_t zmap_start;
	uint64_t inode_table_start;
	uint64_t state_block;
	uint64_t data_start;
	uint64_t backup_block;
	uint64_t volume_generation;
	uint32_t imap_blocks;
	uint32_t zmap_blocks;
	uint64_t inode_table_blocks;
};

struct mfs4_journal_io {
	void *cookie;
	struct mfs4_journal_geometry geometry;
	int (*read_block)(void *cookie, uint64_t block, void *data);
	int (*write_block)(void *cookie, uint64_t block, const void *data);
	/* Must flush LMFS buffers and the device's volatile write cache. */
	int (*flush)(void *cookie);
	void (*fail_closed)(void *cookie);
};

struct mfs4_journal_image {
	uint64_t home_block;
	uint32_t metadata_type;
	uint32_t reserved;
	uint8_t data[MFS4_BLOCK_SIZE];
};

/*
 * Allocate this object statically or from a mount-time pool. It is about
 * 256 KiB and deliberately contains all 64 image buffers up front.
 */
struct mfs4_journal_tx {
	const struct mfs4_journal_io *io;
	uint64_t sequence;
	uint32_t count;
	int active;
	struct mfs4_journal_image images[MFS4_JOURNAL_MAX_IMAGES];
};

int mfs4_journal_tx_begin(struct mfs4_journal_tx *tx,
    const struct mfs4_journal_io *io, uint64_t sequence);
int mfs4_journal_tx_get_meta(struct mfs4_journal_tx *tx,
    uint64_t home_block, uint32_t metadata_type, uint8_t **image_out);
int mfs4_journal_tx_find_meta(struct mfs4_journal_tx *tx,
    uint64_t home_block, const uint8_t **image_out);
int mfs4_journal_tx_commit(struct mfs4_journal_tx *tx);
void mfs4_journal_tx_abort(struct mfs4_journal_tx *tx);

/* Replay is idempotent. A valid commit record is never discarded on error. */
int mfs4_journal_replay(const struct mfs4_journal_io *io);
/* Return 1 for a valid committed transaction, 0 for no valid commit, <0 error. */
int mfs4_journal_pending(const struct mfs4_journal_io *io);

#endif /* _MFS4_JOURNAL_H */
