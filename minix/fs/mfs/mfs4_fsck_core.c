/*
 * Structural verifier and conservative repair engine for the MFS4 v1.0 format.
 *
 * The checker shares journal, orphan, xattr and ACL codecs with the MFS server.
 * Repair writes are staged until the complete metadata graph has been checked;
 * a dirty state marker is flushed before repairs, and the repaired volume is
 * rescanned before CLEAN may be set. Ambiguous structural corruption is never
 * guessed at or rewritten.
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <minix/mfs4_disk.h>
#include <minix/posix_acl.h>

#include "mfs4_fsck_core.h"
#include "mfs4_journal.h"
#include "mfs4_orphan_core.h"
#include "mfs4_xattr_core.h"

#define MFS4_IFMT       0170000U
#define MFS4_IFDIR      0040000U
#define MFS4_IFREG      0100000U
#define MFS4_IFLNK      0120000U
#define MFS4_IFBLK      0060000U
#define MFS4_IFCHR      0020000U
#define MFS4_IFIFO      0010000U
#define MFS4_IFSOCK     0140000U
#define MFS4_KNOWN_IFLAGS (I4_ORPHAN | I4_TRUNCATING)
#define MFS4_PTRS       MFS4_POINTERS_PER_BLOCK
#define MFS4_DIRS_PER_BLOCK (MFS4_BLOCK_SIZE / MFS4_DIRENT_SIZE)

struct mfs4_fsck_super {
	uint64_t total_blocks;
	uint32_t total_inodes;
	uint64_t imap_start;
	uint32_t imap_blocks;
	uint64_t zmap_start;
	uint32_t zmap_blocks;
	uint64_t inode_table_start;
	uint64_t inode_table_blocks;
	uint64_t state_block;
	uint64_t data_start;
	uint64_t backup_block;
	uint64_t volume_generation;
	uint32_t compat_features;
	uint32_t ro_compat_features;
	uint32_t incompat_features;
	uint64_t max_file_size;
};

struct mfs4_fsck_inode {
	uint16_t mode;
	uint16_t flags;
	uint32_t nlink;
	uint32_t uid;
	uint32_t gid;
	uint64_t size;
	uint32_t generation;
	uint32_t direct[MFS4_DIRECT_POINTERS];
	uint32_t single;
	uint32_t doubly;
	uint32_t triply;
	uint32_t xattr_head;
	uint32_t orphan_next;
	uint32_t atime_nsec;
	uint32_t mtime_nsec;
	uint32_t ctime_nsec;
};

struct mfs4_fsck_ctx {
	const struct mfs4_fsck_io *io;
	struct mfs4_fsck_super sb;
	struct mfs4_fsck_report *report;
	uint8_t *imap;
	uint8_t *zmap;
	uint8_t *seen_blocks;
	uint8_t *orphan_seen;
	uint32_t *link_refs;
	size_t imap_bytes;
	size_t zmap_bytes;
	size_t data_map_bytes;
	size_t inode_map_bytes;
	struct mfs4_journal_io journal_io;
	int repair_mode;
	int primary_super_valid;
	int backup_super_valid;
	uint8_t valid_super[MFS4_SUPER_SIZE];
	uint32_t orphan_head;
	uint16_t *acl_modes;
	uint8_t *acl_mode_valid;
};

static int
mfs4_fsck_read(struct mfs4_fsck_ctx *ctx, uint64_t block, void *data)
{
	if (block >= ctx->sb.total_blocks)
		return EIO;
	return ctx->io->read_block(ctx->io->cookie, block, data);
}

static int
mfs4_fsck_bit(const uint8_t *map, uint64_t bit)
{
	return (map[bit >> 3] & (uint8_t)(1U << (bit & 7U))) != 0;
}

static void
mfs4_fsck_setbit(uint8_t *map, uint64_t bit)
{
	map[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
}

static void
mfs4_fsck_clrbit(uint8_t *map, uint64_t bit)
{
	map[bit >> 3] &= (uint8_t)~(1U << (bit & 7U));
}

static int
mfs4_fsck_all_zero(const uint8_t *p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		if (p[i] != 0)
			return 0;
	return 1;
}

static int
mfs4_fsck_super_crc(const uint8_t raw[MFS4_SUPER_SIZE])
{
	uint8_t copy[MFS4_SUPER_SIZE];
	uint32_t crc;

	memcpy(copy, raw, sizeof(copy));
	mfs4_put_le32(copy + MFS4_SB_CRC32C, 0);
	crc = mfs4_crc32c(copy, MFS4_SB_CRC32C);
	return crc == mfs4_get_le32(raw + MFS4_SB_CRC32C);
}

static int
mfs4_fsck_geometry(struct mfs4_fsck_super *s)
{
	uint64_t imap_blocks, inode_blocks, zmap_blocks = 1;
	uint64_t zmap_start, inode_start, state_block, data_start, data_blocks;
	uint64_t bits;
	unsigned int pass;

	if (s->total_blocks <= MFS4_IMAP_START ||
	    s->total_blocks > MFS4_MAX_BLOCKS ||
	    s->total_inodes == 0 ||
	    s->backup_block != s->total_blocks - 1 ||
	    s->max_file_size != MFS4_MAX_FILE_SIZE ||
	    s->imap_start != MFS4_IMAP_START ||
	    s->imap_blocks == 0 || s->zmap_blocks == 0)
		return EIO;

	bits = (uint64_t)s->total_inodes + 1;
	imap_blocks = (bits + MFS4_BITS_PER_BLOCK - 1) /
	    MFS4_BITS_PER_BLOCK;
	inode_blocks = (bits + MFS4_INODES_PER_BLOCK - 1) /
	    MFS4_INODES_PER_BLOCK;
	if (imap_blocks > UINT32_MAX || inode_blocks > UINT32_MAX)
		return EIO;

	for (pass = 0; pass < 64; pass++) {
		zmap_start = s->imap_start + imap_blocks;
		inode_start = zmap_start + zmap_blocks;
		state_block = inode_start + inode_blocks;
		if (state_block >= s->backup_block)
			return EIO;
		data_start = state_block + 1;
		data_blocks = s->backup_block - data_start;
		if (data_blocks == 0)
			return EIO;
		bits = (data_blocks + MFS4_BITS_PER_BLOCK - 1) /
		    MFS4_BITS_PER_BLOCK;
		if (bits == zmap_blocks)
			break;
		zmap_blocks = bits;
	}
	if (pass == 64)
		return EIO;

	if (s->imap_blocks != imap_blocks ||
	    s->zmap_start != zmap_start ||
	    s->zmap_blocks != zmap_blocks ||
	    s->inode_table_start != inode_start ||
	    s->inode_table_blocks != inode_blocks ||
	    s->state_block != state_block ||
	    s->data_start != data_start ||
	    s->backup_block <= s->data_start)
		return EIO;
	return 0;
}

static int
mfs4_fsck_decode_super(const uint8_t raw[MFS4_SUPER_SIZE],
    struct mfs4_fsck_super *s)
{
	if (memcmp(raw + MFS4_SB_MAGIC, MFS4_SUPER_MAGIC,
	    MFS4_SUPER_MAGIC_SIZE) != 0 ||
	    mfs4_get_le16(raw + MFS4_SB_MAJOR) != MFS4_SUPER_MAJOR ||
	    mfs4_get_le16(raw + MFS4_SB_MINOR) != MFS4_SUPER_MINOR ||
	    mfs4_get_le32(raw + MFS4_SB_HEADER_SIZE) != MFS4_SUPER_SIZE ||
	    mfs4_get_le32(raw + MFS4_SB_BLOCK_SIZE) != MFS4_BLOCK_SIZE ||
	    mfs4_get_le16(raw + MFS4_SB_INODE_SIZE) != MFS4_INODE_SIZE ||
	    mfs4_get_le16(raw + MFS4_SB_DIRENT_SIZE) != MFS4_DIRENT_SIZE ||
	    mfs4_get_le32(raw + MFS4_SB_VOLUME_FLAGS) != 0 ||
	    mfs4_get_le32(raw + 52) != 0 ||
	    mfs4_get_le32(raw + 140) != 0 ||
	    mfs4_get_le32(raw + MFS4_SB_JOURNAL_BLOCKS) !=
	    MFS4_JOURNAL_BLOCKS ||
	    mfs4_get_le64(raw + MFS4_SB_JOURNAL_START) !=
	    MFS4_JOURNAL_START ||
	    mfs4_get_le32(raw + MFS4_SB_MAX_TX_IMAGES) !=
	    MFS4_JOURNAL_MAX_IMAGES ||
	    mfs4_get_le32(raw + MFS4_SB_ROOT_INODE) != 1 ||
	    !mfs4_fsck_all_zero(raw + 184, 324) ||
	    !mfs4_fsck_super_crc(raw))
		return EIO;

	memset(s, 0, sizeof(*s));
	s->compat_features = mfs4_get_le32(raw + MFS4_SB_COMPAT_FEATURES);
	s->ro_compat_features = mfs4_get_le32(raw + MFS4_SB_RO_COMPAT_FEATURES);
	s->incompat_features = mfs4_get_le32(raw + MFS4_SB_INCOMPAT_FEATURES);
	s->total_blocks = mfs4_get_le64(raw + MFS4_SB_TOTAL_BLOCKS);
	s->total_inodes = mfs4_get_le32(raw + MFS4_SB_TOTAL_INODES);
	s->imap_start = mfs4_get_le64(raw + MFS4_SB_IMAP_START);
	s->imap_blocks = mfs4_get_le32(raw + MFS4_SB_IMAP_BLOCKS);
	s->zmap_blocks = mfs4_get_le32(raw + MFS4_SB_ZMAP_BLOCKS);
	s->zmap_start = mfs4_get_le64(raw + MFS4_SB_ZMAP_START);
	s->inode_table_start = mfs4_get_le64(raw + MFS4_SB_INODE_TABLE_START);
	s->inode_table_blocks = mfs4_get_le64(raw + MFS4_SB_INODE_TABLE_BLOCKS);
	s->state_block = mfs4_get_le64(raw + MFS4_SB_STATE_BLOCK);
	s->data_start = mfs4_get_le64(raw + MFS4_SB_DATA_START);
	s->backup_block = mfs4_get_le64(raw + MFS4_SB_BACKUP_BLOCK);
	s->max_file_size = mfs4_get_le64(raw + MFS4_SB_MAX_FILE_SIZE);
	s->volume_generation = mfs4_get_le64(raw + MFS4_SB_VOLUME_GENERATION);

	if (s->total_inodes == 0 || s->volume_generation == 0 ||
	    s->incompat_features != 0)
		return EOPNOTSUPP;
	return mfs4_fsck_geometry(s);
}

static int
mfs4_fsck_load_super(struct mfs4_fsck_ctx *ctx, uint64_t device_blocks)
{
	uint8_t block[MFS4_BLOCK_SIZE], primary[MFS4_SUPER_SIZE];
	uint8_t backup[MFS4_SUPER_SIZE], backup_block[MFS4_BLOCK_SIZE];
	struct mfs4_fsck_super ps, bs;
	int pv, bv, pr, br, r;

	if ((r = ctx->io->read_block(ctx->io->cookie, 0, block)) != 0)
		return r;
	memcpy(primary, block + MFS4_SUPER_PRIMARY_OFFSET, sizeof(primary));
	pr = mfs4_fsck_decode_super(primary, &ps);
	if (pr == EOPNOTSUPP)
		return pr;
	pv = (pr == 0);
	ctx->primary_super_valid = pv;

	if (pv) {
		memcpy(ctx->valid_super, primary, sizeof(primary));
		if (device_blocks != 0 && ps.total_blocks > device_blocks)
			return EIO;
		if ((r = ctx->io->read_block(ctx->io->cookie,
		    ps.backup_block, backup_block)) != 0)
			return r;
		memcpy(backup, backup_block, sizeof(backup));
		br = mfs4_fsck_decode_super(backup, &bs);
		if (br == EOPNOTSUPP)
			return br;
		bv = (br == 0);
		ctx->backup_super_valid = bv;
		if (bv && memcmp(primary, backup, MFS4_SUPER_SIZE) != 0)
			return EIO;
		ctx->sb = ps;
		if (!bv)
			ctx->report->superblock_degraded = 1;
		return 0;
	}

	if (device_blocks < 2)
		return EIO;
	if ((r = ctx->io->read_block(ctx->io->cookie,
	    device_blocks - 1, backup_block)) != 0)
		return r;
	memcpy(backup, backup_block, sizeof(backup));
	br = mfs4_fsck_decode_super(backup, &bs);
	if (br != 0)
		return br == EOPNOTSUPP ? br : EIO;
	if (bs.backup_block != device_blocks - 1)
		return EIO;
	ctx->backup_super_valid = 1;
	memcpy(ctx->valid_super, backup, sizeof(backup));
	ctx->sb = bs;
	ctx->report->superblock_degraded = 1; /* primary copy is damaged */
	return 0;
}

static int
mfs4_fsck_alloc_maps(struct mfs4_fsck_ctx *ctx)
{
	uint64_t data_blocks;
	uint32_t i;
	int r;

	data_blocks = ctx->sb.backup_block - ctx->sb.data_start;
	ctx->imap_bytes = (size_t)ctx->sb.imap_blocks * MFS4_BLOCK_SIZE;
	ctx->zmap_bytes = (size_t)ctx->sb.zmap_blocks * MFS4_BLOCK_SIZE;
	ctx->data_map_bytes = (size_t)((data_blocks + 7) / 8);
	ctx->inode_map_bytes = (size_t)(((uint64_t)ctx->sb.total_inodes + 8) / 8);
	ctx->imap = malloc(ctx->imap_bytes);
	ctx->zmap = malloc(ctx->zmap_bytes);
	ctx->seen_blocks = calloc(ctx->data_map_bytes, 1);
	ctx->orphan_seen = calloc((size_t)ctx->sb.total_inodes + 1, 1);
	ctx->link_refs = calloc((size_t)ctx->sb.total_inodes + 1,
	    sizeof(*ctx->link_refs));
	ctx->acl_modes = calloc((size_t)ctx->sb.total_inodes + 1,
	    sizeof(*ctx->acl_modes));
	ctx->acl_mode_valid = calloc((size_t)ctx->sb.total_inodes + 1, 1);
	if (ctx->imap == NULL || ctx->zmap == NULL ||
	    ctx->seen_blocks == NULL || ctx->orphan_seen == NULL ||
	    ctx->link_refs == NULL || ctx->acl_modes == NULL ||
	    ctx->acl_mode_valid == NULL)
		return ENOMEM;

	for (i = 0; i < ctx->sb.imap_blocks; i++) {
		r = mfs4_fsck_read(ctx, ctx->sb.imap_start + i,
		    ctx->imap + (size_t)i * MFS4_BLOCK_SIZE);
		if (r != 0)
			return r;
	}
	for (i = 0; i < ctx->sb.zmap_blocks; i++) {
		r = mfs4_fsck_read(ctx, ctx->sb.zmap_start + i,
		    ctx->zmap + (size_t)i * MFS4_BLOCK_SIZE);
		if (r != 0)
			return r;
	}
	return 0;
}

static int
mfs4_fsck_check_bitmap_tails(struct mfs4_fsck_ctx *ctx)
{
	uint64_t imap_bits = (uint64_t)ctx->sb.total_inodes + 1;
	uint64_t imap_capacity = (uint64_t)ctx->sb.imap_blocks *
	    MFS4_BITS_PER_BLOCK;
	uint64_t data_blocks = ctx->sb.backup_block - ctx->sb.data_start;
	uint64_t zmap_capacity = (uint64_t)ctx->sb.zmap_blocks *
	    MFS4_BITS_PER_BLOCK;
	uint64_t bit;
	int changed = 0;

	if (!mfs4_fsck_bit(ctx->imap, 0) || !mfs4_fsck_bit(ctx->imap, 1) ||
	    !mfs4_fsck_bit(ctx->zmap, 0)) {
		if (ctx->repair_mode == MFS4_FSCK_CHECK_ONLY)
			return EIO;
		mfs4_fsck_setbit(ctx->imap, 0);
		mfs4_fsck_setbit(ctx->imap, 1);
		mfs4_fsck_setbit(ctx->zmap, 0);
		changed = 1;
	}
	for (bit = imap_bits; bit < imap_capacity; bit++) {
		if (!mfs4_fsck_bit(ctx->imap, bit)) {
			if (ctx->repair_mode == MFS4_FSCK_CHECK_ONLY)
				return EIO;
			mfs4_fsck_setbit(ctx->imap, bit);
			changed = 1;
		}
	}
	for (bit = data_blocks; bit < zmap_capacity; bit++) {
		if (!mfs4_fsck_bit(ctx->zmap, bit)) {
			if (ctx->repair_mode == MFS4_FSCK_CHECK_ONLY)
				return EIO;
			mfs4_fsck_setbit(ctx->zmap, bit);
			changed = 1;
		}
	}
	if (changed)
		ctx->report->repairs_made++;
	return 0;
}

static int
mfs4_fsck_read_inode(struct mfs4_fsck_ctx *ctx, uint32_t ino,
    struct mfs4_fsck_inode *ip)
{
	uint8_t block[MFS4_BLOCK_SIZE];
	uint64_t blockno;
	size_t offset;
	int r;

	if (ino == 0 || ino > ctx->sb.total_inodes)
		return EIO;
	blockno = ctx->sb.inode_table_start + ino / MFS4_INODES_PER_BLOCK;
	offset = (size_t)(ino % MFS4_INODES_PER_BLOCK) * MFS4_INODE_SIZE;
	if ((r = mfs4_fsck_read(ctx, blockno, block)) != 0)
		return r;

	memset(ip, 0, sizeof(*ip));
	ip->mode = mfs4_get_le16(block + offset + MFS4_I_MODE);
	ip->flags = mfs4_get_le16(block + offset + MFS4_I_FLAGS);
	ip->nlink = mfs4_get_le32(block + offset + MFS4_I_NLINK);
	ip->uid = mfs4_get_le32(block + offset + MFS4_I_UID);
	ip->gid = mfs4_get_le32(block + offset + MFS4_I_GID);
	ip->size = mfs4_get_le64(block + offset + MFS4_I_SIZE);
	ip->atime_nsec = mfs4_get_le32(block + offset + MFS4_I_ATIME_NSEC);
	ip->mtime_nsec = mfs4_get_le32(block + offset + MFS4_I_MTIME_NSEC);
	ip->ctime_nsec = mfs4_get_le32(block + offset + MFS4_I_CTIME_NSEC);
	ip->generation = mfs4_get_le32(block + offset + MFS4_I_GENERATION);
	for (unsigned int i = 0; i < MFS4_DIRECT_POINTERS; i++)
		ip->direct[i] = mfs4_get_le32(block + offset +
		    MFS4_I_DIRECT + i * sizeof(uint32_t));
	ip->single = mfs4_get_le32(block + offset + MFS4_I_SINGLE_INDIRECT);
	ip->doubly = mfs4_get_le32(block + offset + MFS4_I_DOUBLE_INDIRECT);
	ip->triply = mfs4_get_le32(block + offset + MFS4_I_TRIPLE_INDIRECT);
	ip->xattr_head = mfs4_get_le32(block + offset + MFS4_I_XATTR_HEAD);
	ip->orphan_next = mfs4_get_le32(block + offset + MFS4_I_ORPHAN_NEXT);
	if (mfs4_get_le32(block + offset + MFS4_I_RESERVED) != 0)
		return EIO;
	return 0;
}

static int
mfs4_fsck_mark_block(struct mfs4_fsck_ctx *ctx, uint32_t block)
{
	uint64_t bit;

	if (block < ctx->sb.data_start || block >= ctx->sb.backup_block)
		return EIO;
	bit = (uint64_t)block - ctx->sb.data_start;
	if (mfs4_fsck_bit(ctx->seen_blocks, bit))
		return EIO;
	mfs4_fsck_setbit(ctx->seen_blocks, bit);
	ctx->report->used_data_blocks++;
	return 0;
}

static uint64_t
mfs4_fsck_power1024(unsigned int exponent)
{
	uint64_t value = 1;
	while (exponent-- != 0)
		value *= MFS4_PTRS;
	return value;
}

static int
mfs4_fsck_walk_indirect(struct mfs4_fsck_ctx *ctx, uint32_t block,
    unsigned int level, uint64_t base, uint64_t file_blocks, int orphan)
{
	uint8_t image[MFS4_BLOCK_SIZE];
	uint64_t child_span, index;
	uint32_t i, child;
	int r;

	if (level == 0 || level > 3)
		return EINVAL;
	if ((r = mfs4_fsck_mark_block(ctx, block)) != 0)
		return r;
	if ((r = mfs4_fsck_read(ctx, block, image)) != 0)
		return r;
	child_span = mfs4_fsck_power1024(level - 1);
	for (i = 0; i < MFS4_PTRS; i++) {
		child = mfs4_get_le32(image + (size_t)i * sizeof(uint32_t));
		if (child == 0)
			continue;
		index = base + (uint64_t)i * child_span;
		if (index >= (MFS4_MAX_FILE_SIZE / MFS4_BLOCK_SIZE))
			return EIO;
		if (!orphan && index >= file_blocks)
			return EIO;
		if (level == 1)
			r = mfs4_fsck_mark_block(ctx, child);
		else
			r = mfs4_fsck_walk_indirect(ctx, child, level - 1,
			    index, file_blocks, orphan);
		if (r != 0)
			return r;
	}
	return 0;
}

static int
mfs4_fsck_walk_inode_blocks(struct mfs4_fsck_ctx *ctx,
    const struct mfs4_fsck_inode *ip)
{
	uint64_t file_blocks = (ip->size + MFS4_BLOCK_SIZE - 1) /
	    MFS4_BLOCK_SIZE;
	uint64_t base;
	unsigned int i;
	int orphan = (ip->flags & I4_ORPHAN) != 0;
	int r;

	if (ip->size > ctx->sb.max_file_size)
		return EFBIG;
	for (i = 0; i < MFS4_DIRECT_POINTERS; i++) {
		if (ip->direct[i] == 0)
			continue;
		if (!orphan && i >= file_blocks)
			return EIO;
		if ((r = mfs4_fsck_mark_block(ctx, ip->direct[i])) != 0)
			return r;
	}

	base = MFS4_DIRECT_POINTERS;
	if (ip->single != 0) {
		if (!orphan && base >= file_blocks)
			return EIO;
		if ((r = mfs4_fsck_walk_indirect(ctx, ip->single, 1, base,
		    file_blocks, orphan)) != 0)
			return r;
	}
	base += MFS4_PTRS;
	if (ip->doubly != 0) {
		if (!orphan && base >= file_blocks)
			return EIO;
		if ((r = mfs4_fsck_walk_indirect(ctx, ip->doubly, 2, base,
		    file_blocks, orphan)) != 0)
			return r;
	}
	base += (uint64_t)MFS4_PTRS * MFS4_PTRS;
	if (ip->triply != 0) {
		if (!orphan && base >= file_blocks)
			return EIO;
		if ((r = mfs4_fsck_walk_indirect(ctx, ip->triply, 3, base,
		    file_blocks, orphan)) != 0)
			return r;
	}
	return 0;
}

static int
mfs4_fsck_validate_xattrs(struct mfs4_fsck_ctx *ctx, uint32_t ino,
    const struct mfs4_fsck_inode *ip)
{
	uint8_t block[MFS4_BLOCK_SIZE];
	uint8_t *stream;
	uint32_t seen[MFS4_XATTR_MAX_BLOCKS], next, total_entries = 0;
	uint16_t used, entries;
	uint32_t current = ip->xattr_head;
	size_t total = 0, offset;
	unsigned int blocks = 0, i;
	int is_directory = (ip->mode & MFS4_IFMT) == MFS4_IFDIR;
	int r;

	if (current == 0)
		return 0;
	stream = malloc(MFS4_XATTR_MAX_BYTES);
	if (stream == NULL)
		return ENOMEM;
	while (current != 0) {
		if (blocks == MFS4_XATTR_MAX_BLOCKS ||
		    current < ctx->sb.data_start ||
		    current >= ctx->sb.backup_block) {
			r = EIO;
			goto out;
		}
		for (i = 0; i < blocks; i++) {
			if (seen[i] == current) {
				r = ELOOP;
				goto out;
			}
		}
		if ((r = mfs4_fsck_mark_block(ctx, current)) != 0)
			goto out;
		if ((r = mfs4_fsck_read(ctx, current, block)) != 0)
			goto out;
		if ((r = mfs4_xattr_validate_block(block, ino, ip->generation,
		    &next, &used, &entries)) != 0)
			goto out;
		if (used == 0 || entries == 0 ||
		    total > MFS4_XATTR_MAX_BYTES - used ||
		    total_entries > UINT32_MAX - entries) {
			r = EIO;
			goto out;
		}
		seen[blocks++] = current;
		memcpy(stream + total, block + MFS4_XA_ENTRIES, used);
		total += used;
		total_entries += entries;
		current = next;
	}
	if ((r = mfs4_xattr_validate_entries(stream, total,
	    total_entries, 1)) != 0)
		goto out;
	offset = 0;
	while (offset < total) {
		struct mfs4_xattr_entry entry;
		char name[MFS4_XATTR_MAX_NAME + 1];

		if ((r = mfs4_xattr_entry_next(stream, total, &offset,
		    &entry)) != 0)
			goto out;
		memcpy(name, entry.name, entry.name_len);
		name[entry.name_len] = '\0';
		if (strcmp(name, "system.posix_acl_access") == 0 ||
		    strcmp(name, "system.posix_acl_default") == 0) {
			if (strcmp(name, "system.posix_acl_default") == 0 &&
			    !is_directory) {
				r = EIO;
				goto out;
			}
			if (minix_acl_validate(entry.value, entry.value_len) != 0) {
				r = EIO;
				goto out;
			}
			if (strcmp(name, "system.posix_acl_access") == 0) {
				uint16_t acl_mode;
				if (minix_acl_mode(entry.value, entry.value_len,
				    &acl_mode) != 0) {
					r = EIO;
					goto out;
				}
				if ((ip->mode & 0777U) != acl_mode) {
					if (ctx->repair_mode == MFS4_FSCK_CHECK_ONLY) {
						r = EIO;
						goto out;
					}
					ctx->acl_modes[ino] = acl_mode;
					ctx->acl_mode_valid[ino] = 1;
					ctx->report->repairs_made++;
				}
			}
		}
	}
	r = 0;
out:
	free(stream);
	return r;
}

static int
mfs4_fsck_get_indirect(struct mfs4_fsck_ctx *ctx, uint32_t block,
    uint32_t index, uint32_t *value)
{
	uint8_t image[MFS4_BLOCK_SIZE];
	int r;

	if (block < ctx->sb.data_start || block >= ctx->sb.backup_block ||
	    index >= MFS4_PTRS)
		return EIO;
	if ((r = mfs4_fsck_read(ctx, block, image)) != 0)
		return r;
	*value = mfs4_get_le32(image + (size_t)index * sizeof(uint32_t));
	if (*value != 0 && (*value < ctx->sb.data_start ||
	    *value >= ctx->sb.backup_block))
		return EIO;
	return 0;
}

static int
mfs4_fsck_map_block(struct mfs4_fsck_ctx *ctx,
    const struct mfs4_fsck_inode *ip, uint64_t logical, uint32_t *physical)
{
	uint64_t n, q;
	uint32_t root, index;
	int r;

	if (logical < MFS4_DIRECT_POINTERS) {
		*physical = ip->direct[logical];
		return 0;
	}
	n = logical - MFS4_DIRECT_POINTERS;
	if (n < MFS4_PTRS) {
		if (ip->single == 0) {
			*physical = 0;
			return 0;
		}
		return mfs4_fsck_get_indirect(ctx, ip->single,
		    (uint32_t)n, physical);
	}
	n -= MFS4_PTRS;
	if (n < (uint64_t)MFS4_PTRS * MFS4_PTRS) {
		if (ip->doubly == 0) {
			*physical = 0;
			return 0;
		}
		if ((r = mfs4_fsck_get_indirect(ctx, ip->doubly,
		    (uint32_t)(n / MFS4_PTRS), &root)) != 0)
			return r;
		if (root == 0) {
			*physical = 0;
			return 0;
		}
		return mfs4_fsck_get_indirect(ctx, root,
		    (uint32_t)(n % MFS4_PTRS), physical);
	}
	n -= (uint64_t)MFS4_PTRS * MFS4_PTRS;
	if (n >= (uint64_t)MFS4_PTRS * MFS4_PTRS * MFS4_PTRS)
		return EFBIG;
	if (ip->triply == 0) {
		*physical = 0;
		return 0;
	}
	q = n / ((uint64_t)MFS4_PTRS * MFS4_PTRS);
	if ((r = mfs4_fsck_get_indirect(ctx, ip->triply,
	    (uint32_t)q, &root)) != 0)
		return r;
	if (root == 0) {
		*physical = 0;
		return 0;
	}
	n %= (uint64_t)MFS4_PTRS * MFS4_PTRS;
	if ((r = mfs4_fsck_get_indirect(ctx, root,
	    (uint32_t)(n / MFS4_PTRS), &index)) != 0)
		return r;
	if (index == 0) {
		*physical = 0;
		return 0;
	}
	return mfs4_fsck_get_indirect(ctx, index,
	    (uint32_t)(n % MFS4_PTRS), physical);
}

static int
mfs4_fsck_name(const uint8_t name[MFS4_NAME_MAX], size_t *length)
{
	size_t i;
	int terminated = 0;

	*length = MFS4_NAME_MAX;
	for (i = 0; i < MFS4_NAME_MAX; i++) {
		if (name[i] == 0) {
			*length = i;
			terminated = 1;
			break;
		}
		if (name[i] == '/')
			return EIO;
	}
	if (*length == 0)
		return EIO;
	if (terminated && !mfs4_fsck_all_zero(name + *length,
	    MFS4_NAME_MAX - *length))
		return EIO;
	return 0;
}

static int
mfs4_fsck_directories(struct mfs4_fsck_ctx *ctx)
{
	uint8_t block[MFS4_BLOCK_SIZE];
	uint32_t ino, target, physical;
	struct mfs4_fsck_inode ip, target_ip;
	uint64_t logical, blocks;
	uint8_t *entry;
	size_t name_len;
	unsigned int dot, dotdot, i;
	int detached, r;

	for (ino = 1; ino <= ctx->sb.total_inodes; ino++) {
		if (!mfs4_fsck_bit(ctx->imap, ino))
			continue;
		if ((r = mfs4_fsck_read_inode(ctx, ino, &ip)) != 0)
			return r;
		if ((ip.mode & MFS4_IFMT) != MFS4_IFDIR)
			continue;
		if (ip.size == 0 || ip.size % MFS4_DIRENT_SIZE != 0)
			return EIO;
		blocks = (ip.size + MFS4_BLOCK_SIZE - 1) / MFS4_BLOCK_SIZE;
		/*
		 * rmdir may leave an open, zero-link directory on the orphan list
		 * until its final reference is dropped. Its "." and ".." entries
		 * remain for validation, but no longer contribute to link counts.
		 */
		detached = (ip.flags & I4_ORPHAN) != 0 && ip.nlink == 0;
		dot = dotdot = 0;
		for (logical = 0; logical < blocks; logical++) {
			if ((r = mfs4_fsck_map_block(ctx, &ip, logical,
			    &physical)) != 0)
				return r;
			if (physical == 0)
				return EIO;
			if ((r = mfs4_fsck_read(ctx, physical, block)) != 0)
				return r;
			for (i = 0; i < MFS4_BLOCK_SIZE / MFS4_DIRENT_SIZE; i++) {
				uint64_t byte_offset = logical * MFS4_BLOCK_SIZE +
				    (uint64_t)i * MFS4_DIRENT_SIZE;
				if (byte_offset >= ip.size)
					break;
				entry = block + (size_t)i * MFS4_DIRENT_SIZE;
				target = mfs4_get_le32(entry + MFS4_DIRENT_INODE);
				if (target == 0) {
					if (!mfs4_fsck_all_zero(entry +
					    MFS4_DIRENT_NAME, MFS4_NAME_MAX))
						return EIO;
					continue;
				}
				if (target > ctx->sb.total_inodes ||
				    !mfs4_fsck_bit(ctx->imap, target) ||
				    (r = mfs4_fsck_name(entry + MFS4_DIRENT_NAME,
				    &name_len)) != 0)
					return EIO;
				if ((r = mfs4_fsck_read_inode(ctx, target,
				    &target_ip)) != 0)
					return r;
				if (target_ip.mode == 0)
					return EIO;
				if (name_len == 1 && entry[MFS4_DIRENT_NAME] == '.') {
					if (target != ino || ++dot != 1)
						return EIO;
				} else if (name_len == 2 &&
				    entry[MFS4_DIRENT_NAME] == '.' &&
				    entry[MFS4_DIRENT_NAME + 1] == '.') {
					if ((target_ip.mode & MFS4_IFMT) != MFS4_IFDIR ||
					    ++dotdot != 1)
						return EIO;
					if (ino == 1 && target != 1)
						return EIO;
				} else if (detached) {
					/* rmdir cannot leave children in a detached directory. */
					return EIO;
				}
				/* Detached orphan directories are no longer namespace links. */
				if (!detached) {
					if (ctx->link_refs[target] == UINT32_MAX)
						return EOVERFLOW;
					ctx->link_refs[target]++;
				}
			}
		}
		if (dot != 1 || dotdot != 1)
			return EIO;
	}
	return 0;
}

static int
mfs4_fsck_orphans(struct mfs4_fsck_ctx *ctx)
{
	uint8_t state[MFS4_BLOCK_SIZE];
	uint32_t head, current, next, ino;
	struct mfs4_fsck_inode ip;
	uint64_t walked = 0;
	int r;

	if ((r = mfs4_fsck_read(ctx, ctx->sb.state_block, state)) != 0)
		return r;
	if ((r = mfs4_orphan_state_validate(state,
	    ctx->sb.volume_generation, ctx->sb.total_inodes, &head)) != 0)
		return r;
	ctx->orphan_head = head;
	if ((mfs4_get_le32(state + MFS4_STATE_FLAGS) &
	    ~MFS4_STATE_CLEAN) != 0)
		return EIO;
	ctx->report->clean =
	    (mfs4_get_le32(state + MFS4_STATE_FLAGS) & MFS4_STATE_CLEAN) != 0;

	current = head;
	while (current != 0) {
		if (current <= 1 || current > ctx->sb.total_inodes ||
		    ctx->orphan_seen[current] || ++walked > ctx->sb.total_inodes)
			return ELOOP;
		if (!mfs4_fsck_bit(ctx->imap, current))
			return EIO;
		if ((r = mfs4_fsck_read_inode(ctx, current, &ip)) != 0)
			return r;
		if (ip.mode == 0 || !(ip.flags & I4_ORPHAN) ||
		    ((ip.flags & I4_TRUNCATING) == 0 && ip.nlink != 0))
			return EIO;
		ctx->orphan_seen[current] = 1;
		ctx->report->orphan_inodes++;
		next = ip.orphan_next;
		if (next == current || (next != 0 &&
		    (next <= 1 || next > ctx->sb.total_inodes)))
			return EIO;
		current = next;
	}

	for (ino = 1; ino <= ctx->sb.total_inodes; ino++) {
		if (!mfs4_fsck_bit(ctx->imap, ino))
			continue;
		if ((r = mfs4_fsck_read_inode(ctx, ino, &ip)) != 0)
			return r;
		if ((ip.flags & I4_ORPHAN) != 0 && !ctx->orphan_seen[ino])
			return EIO;
		if ((ip.flags & I4_TRUNCATING) != 0 &&
		    !(ip.flags & I4_ORPHAN))
			return EIO;
	}
	return 0;
}

static int
mfs4_fsck_scan_inodes(struct mfs4_fsck_ctx *ctx)
{
	struct mfs4_fsck_inode ip;
	uint32_t ino;
	int allocated, r;

	for (ino = 1; ino <= ctx->sb.total_inodes; ino++) {
		allocated = mfs4_fsck_bit(ctx->imap, ino);
		if ((r = mfs4_fsck_read_inode(ctx, ino, &ip)) != 0)
			return r;
		if (ip.mode == 0) {
			if (allocated) {
				if (ctx->repair_mode == MFS4_FSCK_CHECK_ONLY)
					return EIO;
				mfs4_fsck_clrbit(ctx->imap, ino);
				ctx->report->repairs_made++;
			}
			continue;
		}
		if (!allocated) {
			if (ctx->repair_mode == MFS4_FSCK_CHECK_ONLY)
				return EIO;
			mfs4_fsck_setbit(ctx->imap, ino);
			ctx->report->repairs_made++;
		}
		if (ip.generation == 0 ||
		    (ip.flags & ~MFS4_KNOWN_IFLAGS) != 0 ||
		    (!(ip.flags & I4_ORPHAN) && ip.orphan_next != 0) ||
		    ip.atime_nsec >= 1000000000U ||
		    ip.mtime_nsec >= 1000000000U ||
		    ip.ctime_nsec >= 1000000000U ||
		    ip.size > ctx->sb.max_file_size)
			return EIO;
		if (ip.nlink == 0 && !(ip.flags & I4_ORPHAN))
			return EIO;
		if ((ip.flags & I4_TRUNCATING) &&
		    !(ip.flags & I4_ORPHAN))
			return EIO;
		switch (ip.mode & MFS4_IFMT) {
		case MFS4_IFREG:
		case MFS4_IFDIR:
		case MFS4_IFLNK:
		case MFS4_IFBLK:
		case MFS4_IFCHR:
		case MFS4_IFIFO:
		case MFS4_IFSOCK:
			break;
		default:
			return EIO;
		}
		if (ino == 1 && (ip.mode & MFS4_IFMT) != MFS4_IFDIR)
			return EIO;
		if ((r = mfs4_fsck_walk_inode_blocks(ctx, &ip)) != 0)
			return r;
		if ((r = mfs4_fsck_validate_xattrs(ctx, ino, &ip)) != 0)
			return r;
		ctx->report->used_inodes++;
	}
	if (ctx->report->used_inodes == 0)
		return EIO;
	return 0;
}

static int
mfs4_fsck_check_link_counts(struct mfs4_fsck_ctx *ctx)
{
	struct mfs4_fsck_inode ip;
	uint32_t ino;
	int r;

	for (ino = 1; ino <= ctx->sb.total_inodes; ino++) {
		if (!mfs4_fsck_bit(ctx->imap, ino))
			continue;
		if ((r = mfs4_fsck_read_inode(ctx, ino, &ip)) != 0)
			return r;
		if (ip.nlink != ctx->link_refs[ino]) {
			if ((ip.flags & I4_ORPHAN) && ip.nlink == 0 &&
			    ctx->link_refs[ino] == 0)
				continue;
			/* A referenced inode with a zero directory-derived count is
			 * ambiguous (e.g. a lost inode); do not invent a namespace.
			 */
			if (ctx->link_refs[ino] == 0 ||
			    ctx->repair_mode == MFS4_FSCK_CHECK_ONLY)
				return EIO;
			ctx->report->repairs_made++;
		}
	}
	return 0;
}

static int
mfs4_fsck_check_zmap(struct mfs4_fsck_ctx *ctx)
{
	uint64_t data_blocks = ctx->sb.backup_block - ctx->sb.data_start;
	uint64_t bit;
	int changed = 0;

	ctx->report->unreferenced_data_blocks = 0;
	for (bit = 0; bit < data_blocks; bit++) {
		int seen = mfs4_fsck_bit(ctx->seen_blocks, bit);
		int allocated = mfs4_fsck_bit(ctx->zmap, bit);

		if (seen && !allocated) {
			if (ctx->repair_mode == MFS4_FSCK_CHECK_ONLY)
				return EIO;
			mfs4_fsck_setbit(ctx->zmap, bit);
			changed = 1;
		} else if (!seen && allocated) {
			ctx->report->unreferenced_data_blocks++;
			if (ctx->repair_mode != MFS4_FSCK_REPAIR_ALL)
				continue;
			mfs4_fsck_clrbit(ctx->zmap, bit);
			changed = 1;
		}
	}
	if (ctx->report->unreferenced_data_blocks != 0 &&
	    ctx->repair_mode != MFS4_FSCK_REPAIR_ALL)
		return EIO;
	if (changed)
		ctx->report->repairs_made++;
	return 0;
}


static int
mfs4_fsck_reload_maps(struct mfs4_fsck_ctx *ctx)
{
	uint32_t i;
	int r;

	for (i = 0; i < ctx->sb.imap_blocks; i++) {
		r = mfs4_fsck_read(ctx, ctx->sb.imap_start + i,
		    ctx->imap + (size_t)i * MFS4_BLOCK_SIZE);
		if (r != 0)
			return r;
	}
	for (i = 0; i < ctx->sb.zmap_blocks; i++) {
		r = mfs4_fsck_read(ctx, ctx->sb.zmap_start + i,
		    ctx->zmap + (size_t)i * MFS4_BLOCK_SIZE);
		if (r != 0)
			return r;
	}
	return 0;
}

static int
mfs4_fsck_scan(struct mfs4_fsck_ctx *ctx)
{
	int r;

	memset(ctx->seen_blocks, 0, ctx->data_map_bytes);
	memset(ctx->orphan_seen, 0, (size_t)ctx->sb.total_inodes + 1);
	memset(ctx->link_refs, 0,
	    ((size_t)ctx->sb.total_inodes + 1) * sizeof(*ctx->link_refs));
	memset(ctx->acl_modes, 0,
	    ((size_t)ctx->sb.total_inodes + 1) * sizeof(*ctx->acl_modes));
	memset(ctx->acl_mode_valid, 0, (size_t)ctx->sb.total_inodes + 1);
	ctx->orphan_head = 0;
	ctx->report->used_inodes = 0;
	ctx->report->used_data_blocks = 0;
	ctx->report->orphan_inodes = 0;
	ctx->report->unreferenced_data_blocks = 0;

	if ((r = mfs4_fsck_check_bitmap_tails(ctx)) != 0)
		return r;
	if ((r = mfs4_fsck_scan_inodes(ctx)) != 0)
		return r;
	if ((r = mfs4_fsck_orphans(ctx)) != 0)
		return r;
	if ((r = mfs4_fsck_directories(ctx)) != 0)
		return r;
	if ((r = mfs4_fsck_check_link_counts(ctx)) != 0)
		return r;
	return mfs4_fsck_check_zmap(ctx);
}

static int
mfs4_fsck_verify_superblocks(struct mfs4_fsck_ctx *ctx)
{
	uint8_t block0[MFS4_BLOCK_SIZE], backup_block[MFS4_BLOCK_SIZE];
	uint8_t primary[MFS4_SUPER_SIZE], backup[MFS4_SUPER_SIZE];
	struct mfs4_fsck_super ps, bs;
	int r;

	if ((r = ctx->io->read_block(ctx->io->cookie, 0, block0)) != 0)
		return r;
	if ((r = ctx->io->read_block(ctx->io->cookie,
	    ctx->sb.backup_block, backup_block)) != 0)
		return r;
	memcpy(primary, block0 + MFS4_SUPER_PRIMARY_OFFSET, sizeof(primary));
	memcpy(backup, backup_block, sizeof(backup));
	if (mfs4_fsck_decode_super(primary, &ps) != 0 ||
	    mfs4_fsck_decode_super(backup, &bs) != 0 ||
	    memcmp(primary, backup, MFS4_SUPER_SIZE) != 0 ||
	    memcmp(primary, ctx->valid_super, MFS4_SUPER_SIZE) != 0)
		return EIO;
	return 0;
}

/* Restore the redundant copy only when exactly one copy decoded cleanly. */
static int
mfs4_fsck_repair_superblocks(struct mfs4_fsck_ctx *ctx)
{
	uint8_t block[MFS4_BLOCK_SIZE];
	int r;

	if (!ctx->report->superblock_degraded)
		return 0;
	if (!ctx->primary_super_valid) {
		if ((r = ctx->io->read_block(ctx->io->cookie, 0, block)) != 0)
			return r;
		memcpy(block + MFS4_SUPER_PRIMARY_OFFSET, ctx->valid_super,
		    MFS4_SUPER_SIZE);
		if ((r = ctx->io->write_block(ctx->io->cookie, 0, block)) != 0)
			return r;
		ctx->primary_super_valid = 1;
		ctx->report->repairs_made++;
	}
	if (!ctx->backup_super_valid) {
		memset(block, 0, sizeof(block));
		memcpy(block, ctx->valid_super, MFS4_SUPER_SIZE);
		if ((r = ctx->io->write_block(ctx->io->cookie,
		    ctx->sb.backup_block, block)) != 0)
			return r;
		ctx->backup_super_valid = 1;
		ctx->report->repairs_made++;
	}
	ctx->report->superblock_degraded = 0;
	return 0;
}

static int
mfs4_fsck_write_maps(struct mfs4_fsck_ctx *ctx)
{
	uint32_t i;
	int r;

	for (i = 0; i < ctx->sb.imap_blocks; i++) {
		r = ctx->io->write_block(ctx->io->cookie,
		    ctx->sb.imap_start + i,
		    ctx->imap + (size_t)i * MFS4_BLOCK_SIZE);
		if (r != 0)
			return r;
	}
	for (i = 0; i < ctx->sb.zmap_blocks; i++) {
		r = ctx->io->write_block(ctx->io->cookie,
		    ctx->sb.zmap_start + i,
		    ctx->zmap + (size_t)i * MFS4_BLOCK_SIZE);
		if (r != 0)
			return r;
	}
	return 0;
}

static int
mfs4_fsck_write_inode_repairs(struct mfs4_fsck_ctx *ctx)
{
	uint8_t block[MFS4_BLOCK_SIZE];
	uint64_t block_index, ino;
	size_t offset;
	uint16_t mode;
	uint32_t nlink;
	int changed, r;

	for (block_index = 0; block_index < ctx->sb.inode_table_blocks;
	    block_index++) {
		if ((r = mfs4_fsck_read(ctx,
		    ctx->sb.inode_table_start + block_index, block)) != 0)
			return r;
		changed = 0;
		for (uint32_t slot = 0; slot < MFS4_INODES_PER_BLOCK; slot++) {
			ino = block_index * MFS4_INODES_PER_BLOCK + slot;
			if (ino == 0 || ino > ctx->sb.total_inodes)
				continue;
			offset = (size_t)slot * MFS4_INODE_SIZE;
			mode = mfs4_get_le16(block + offset + MFS4_I_MODE);
			if (mode == 0)
				continue;
			nlink = mfs4_get_le32(block + offset + MFS4_I_NLINK);
			if (nlink != ctx->link_refs[ino]) {
				mfs4_put_le32(block + offset + MFS4_I_NLINK,
				    ctx->link_refs[ino]);
				changed = 1;
			}
			if (ctx->acl_mode_valid[ino]) {
				uint16_t new_mode = (uint16_t)((mode & ~0777U) |
				    (ctx->acl_modes[ino] & 0777U));
				if (new_mode != mode) {
					mfs4_put_le16(block + offset + MFS4_I_MODE,
					    new_mode);
					changed = 1;
				}
			}
		}
		if (changed && (r = ctx->io->write_block(ctx->io->cookie,
		    ctx->sb.inode_table_start + block_index, block)) != 0)
			return r;
	}
	return 0;
}

static int
mfs4_fsck_set_clean(struct mfs4_fsck_ctx *ctx, int clean)
{
	uint8_t state[MFS4_BLOCK_SIZE];
	uint32_t head, flags, new_flags;
	int r;

	if ((r = mfs4_fsck_read(ctx, ctx->sb.state_block, state)) != 0)
		return r;
	if ((r = mfs4_orphan_state_validate(state,
	    ctx->sb.volume_generation, ctx->sb.total_inodes, &head)) != 0)
		return r;
	flags = mfs4_get_le32(state + MFS4_STATE_FLAGS);
	if ((flags & ~MFS4_STATE_CLEAN) != 0)
		return EIO;
	ctx->orphan_head = head;

	/* A non-empty orphan list is not a cleanly unmounted filesystem. */
	new_flags = flags;
	if (clean && head == 0)
		new_flags |= MFS4_STATE_CLEAN;
	else
		new_flags &= ~MFS4_STATE_CLEAN;
	if (new_flags != flags) {
		mfs4_put_le32(state + MFS4_STATE_FLAGS, new_flags);
		mfs4_put_le32(state + MFS4_STATE_CRC32C,
		    mfs4_crc32c(state, MFS4_STATE_CRC32C));
		if ((r = ctx->io->write_block(ctx->io->cookie,
		    ctx->sb.state_block, state)) != 0)
			return r;
		if ((r = ctx->io->flush(ctx->io->cookie)) != 0)
			return r;
	}
	ctx->report->clean = (new_flags & MFS4_STATE_CLEAN) != 0;
	return 0;
}

static int
mfs4_fsck_repair(struct mfs4_fsck_ctx *ctx)
{
	int r, needs_changes, journal_pending;

	needs_changes = ctx->report->repairs_made != 0 ||
	    ctx->report->superblock_degraded;
	if (needs_changes) {
		/* Persist DIRTY before touching home metadata: repair is restartable. */
		if ((r = mfs4_fsck_set_clean(ctx, 0)) != 0)
			return r;
		if ((r = ctx->io->flush(ctx->io->cookie)) != 0)
			return r;
		if ((r = mfs4_fsck_repair_superblocks(ctx)) != 0)
			return r;
		if ((r = mfs4_fsck_verify_superblocks(ctx)) != 0)
			return r;
		if ((r = mfs4_fsck_write_maps(ctx)) != 0)
			return r;
		if ((r = mfs4_fsck_write_inode_repairs(ctx)) != 0)
			return r;
		if ((r = ctx->io->flush(ctx->io->cookie)) != 0)
			return r;

		/* Re-read the repaired home metadata and rerun every consistency pass. */
		if ((r = mfs4_fsck_reload_maps(ctx)) != 0)
			return r;
		ctx->repair_mode = MFS4_FSCK_CHECK_ONLY;
		if ((r = mfs4_fsck_scan(ctx)) != 0)
			return r;
	}

	/*
	 * The clean marker is the final publication step. Revalidate that no
	 * committed transaction remains, including after any earlier replay.
	 */
	journal_pending = mfs4_journal_validate(&ctx->journal_io);
	if (journal_pending < 0)
		return -journal_pending;
	if (journal_pending != 0)
		return EBUSY;

	/* Do not claim CLEAN while a valid orphan list still needs RW recovery. */
	return mfs4_fsck_set_clean(ctx, ctx->orphan_head == 0);
}

static void
mfs4_fsck_cleanup(struct mfs4_fsck_ctx *ctx)
{
	free(ctx->imap);
	free(ctx->zmap);
	free(ctx->seen_blocks);
	free(ctx->orphan_seen);
	free(ctx->link_refs);
	free(ctx->acl_modes);
	free(ctx->acl_mode_valid);
	memset(ctx, 0, sizeof(*ctx));
}

int
mfs4_fsck_run(const struct mfs4_fsck_io *io, uint64_t device_blocks,
    int repair_mode, struct mfs4_fsck_report *report)
{
	struct mfs4_fsck_ctx ctx;
	int pending, r;

	if (io == NULL || io->read_block == NULL || report == NULL ||
	    repair_mode < MFS4_FSCK_CHECK_ONLY ||
	    repair_mode > MFS4_FSCK_REPAIR_ALL ||
	    (repair_mode != MFS4_FSCK_CHECK_ONLY &&
	    (io->write_block == NULL || io->flush == NULL)))
		return EINVAL;
	memset(&ctx, 0, sizeof(ctx));
	memset(report, 0, sizeof(*report));
	ctx.io = io;
	ctx.report = report;
	ctx.repair_mode = repair_mode;

	if ((r = mfs4_fsck_load_super(&ctx, device_blocks)) != 0)
		goto out;
	report->total_blocks = ctx.sb.total_blocks;
	report->total_inodes = ctx.sb.total_inodes;
	report->data_blocks = ctx.sb.backup_block - ctx.sb.data_start;

	/* Unknown read-only-compatible features must never be written or replayed. */
	if (ctx.sb.ro_compat_features != 0) {
		r = EOPNOTSUPP;
		goto out;
	}

	memset(&ctx.journal_io, 0, sizeof(ctx.journal_io));
	ctx.journal_io.cookie = io->cookie;
	ctx.journal_io.geometry.total_blocks = ctx.sb.total_blocks;
	ctx.journal_io.geometry.imap_start = ctx.sb.imap_start;
	ctx.journal_io.geometry.imap_blocks = ctx.sb.imap_blocks;
	ctx.journal_io.geometry.zmap_start = ctx.sb.zmap_start;
	ctx.journal_io.geometry.zmap_blocks = ctx.sb.zmap_blocks;
	ctx.journal_io.geometry.inode_table_start = ctx.sb.inode_table_start;
	ctx.journal_io.geometry.inode_table_blocks = ctx.sb.inode_table_blocks;
	ctx.journal_io.geometry.state_block = ctx.sb.state_block;
	ctx.journal_io.geometry.data_start = ctx.sb.data_start;
	ctx.journal_io.geometry.backup_block = ctx.sb.backup_block;
	ctx.journal_io.geometry.volume_generation = ctx.sb.volume_generation;
	ctx.journal_io.read_block = io->read_block;
	ctx.journal_io.write_block = io->write_block;
	ctx.journal_io.flush = io->flush;

	pending = mfs4_journal_validate(&ctx.journal_io);
	if (pending < 0) {
		r = -pending;
		goto out;
	}
	report->journal_pending = pending;
	if (pending) {
		if (repair_mode == MFS4_FSCK_CHECK_ONLY) {
			r = EBUSY;
			goto out;
		}
		/* Replay only a fully validated committed record. */
		r = mfs4_journal_replay(&ctx.journal_io);
		if (r != 0)
			goto out;
		report->journal_replayed = 1;
		report->journal_pending = 0;
	}

	if ((r = mfs4_fsck_alloc_maps(&ctx)) != 0)
		goto out;
	if ((r = mfs4_fsck_scan(&ctx)) != 0)
		goto out;
	if (repair_mode != MFS4_FSCK_CHECK_ONLY)
		r = mfs4_fsck_repair(&ctx);
	else
		r = 0;
out:
	mfs4_fsck_cleanup(&ctx);
	return r;
}
