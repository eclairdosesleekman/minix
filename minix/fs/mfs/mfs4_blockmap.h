/*	$NetBSD$	*/

#ifndef _MFS4_BLOCKMAP_H
#define _MFS4_BLOCKMAP_H

#include <stddef.h>
#include <stdint.h>
#include <minix/mfs4_disk.h>

/* Alias the shared MFS4 format constants; do not fork disk geometry here. */
#define MFS4_BM_BLOCK_SIZE       MFS4_BLOCK_SIZE
#define MFS4_BM_FANOUT           MFS4_POINTERS_PER_BLOCK
#define MFS4_BM_DIRECT           MFS4_DIRECT_POINTERS
#define MFS4_BM_SINGLE_SLOT      MFS4_BM_DIRECT
#define MFS4_BM_DOUBLE_SLOT      (MFS4_BM_DIRECT + 1U)
#define MFS4_BM_TRIPLE_SLOT      (MFS4_BM_DIRECT + 2U)
#define MFS4_BM_INODE_POINTERS   MFS4_INODE_POINTERS
#define MFS4_BM_MAX_FILE_SIZE    MFS4_MAX_FILE_SIZE
#define MFS4_BM_MAX_FILE_BLOCKS  (MFS4_BM_MAX_FILE_SIZE / MFS4_BM_BLOCK_SIZE)
#define MFS4_BM_BITS_PER_BLOCK   MFS4_BITS_PER_BLOCK

/*
 * Storage callbacks operate on one complete 4096-byte filesystem block.
 * The backend is responsible for translating these calls to LMFS buffers or
 * a test image.  Indirect blocks and file data use the same read/write path.
 */
struct mfs4_blockmap_io {
	void *cookie;
	int (*read_block)(void *cookie, uint32_t block, void *buffer);
	int (*write_block)(void *cookie, uint32_t block, const void *buffer);
	int (*flush)(void *cookie);
};

/*
 * Geometry is taken from the MFS4 superblock.  The data bitmap describes
 * exactly [data_start, backup_block); bit zero represents data_start.
 */
struct mfs4_blockmap {
	struct mfs4_blockmap_io io;
	uint64_t data_start;
	uint64_t backup_block;
	uint64_t zmap_start;
	uint64_t zmap_blocks;
	uint64_t next_alloc;
};

int mfs4_blockmap_init(struct mfs4_blockmap *map,
    const struct mfs4_blockmap_io *io, uint64_t data_start,
    uint64_t backup_block, uint64_t zmap_start, uint64_t zmap_blocks);
/* alloc/free update the data-area zmap.  Bit zero is reserved by mkfs. */
int mfs4_blockmap_alloc(struct mfs4_blockmap *map, uint32_t *block_out);
int mfs4_blockmap_free(struct mfs4_blockmap *map, uint32_t block);
/* A nonzero physical_block passed to set must already be allocated.  set()
 * updates the supplied in-memory inode pointer array, but does not encode or
 * persist that inode; the MFS caller owns the inode codec/metadata transaction.
 */
int mfs4_blockmap_lookup(struct mfs4_blockmap *map,
    const uint32_t inode_pointers[MFS4_BM_INODE_POINTERS],
    uint64_t logical_block, uint32_t *physical_block);
int mfs4_blockmap_set(struct mfs4_blockmap *map,
    uint32_t inode_pointers[MFS4_BM_INODE_POINTERS],
    uint64_t logical_block, uint32_t physical_block);
int mfs4_blockmap_read(struct mfs4_blockmap *map,
    const uint32_t inode_pointers[MFS4_BM_INODE_POINTERS],
    uint64_t file_size, uint64_t offset, void *buffer, size_t length,
    size_t *bytes_read);
int mfs4_blockmap_write(struct mfs4_blockmap *map,
    uint32_t inode_pointers[MFS4_BM_INODE_POINTERS], uint64_t *file_size,
    uint64_t offset, const void *buffer, size_t length,
    size_t *bytes_written);
int mfs4_blockmap_truncate(struct mfs4_blockmap *map,
    uint32_t inode_pointers[MFS4_BM_INODE_POINTERS], uint64_t *file_size,
    uint64_t new_size);

#endif /* _MFS4_BLOCKMAP_H */
