/*
 * MFS4 fixed-size single-transaction redo journal.
 *
 * This file is intentionally independent of MINIX buffer-cache structures.
 * The caller supplies synchronous block read/write and durable flush hooks.
 */
#include <errno.h>
#include <string.h>
#include "mfs4_journal.h"

static uint32_t
crc32c_update(uint32_t crc, const uint8_t *data, size_t length)
{
	size_t i;
	unsigned int bit;

	for (i = 0; i < length; i++) {
		crc ^= data[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^
			    ((crc & 1U) ? 0x82f63b78U : 0U);
	}
	return crc;
}

static uint32_t
crc32c_bytes(const void *data, size_t length)
{
	return crc32c_update(0xffffffffU, (const uint8_t *)data, length) ^
	    0xffffffffU;
}

static uint32_t
crc32c_block_with_zeroed_field(const uint8_t block[MFS4_BLOCK_SIZE],
    size_t field_offset)
{
	uint8_t copy[MFS4_BLOCK_SIZE];

	memcpy(copy, block, sizeof(copy));
	memset(copy + field_offset, 0, sizeof(uint32_t));
	return crc32c_bytes(copy, sizeof(copy));
}

static int
io_read_valid(const struct mfs4_journal_io *io)
{
	return io != NULL && io->read_block != NULL &&
	    io->geometry.total_blocks > MFS4_JOURNAL_COMMIT_BLOCK &&
	    io->geometry.total_blocks <= MFS4_MAX_BLOCKS &&
	    io->geometry.backup_block == io->geometry.total_blocks - 1 &&
	    io->geometry.data_start < io->geometry.backup_block;
}

static int
io_valid(const struct mfs4_journal_io *io)
{
	return io_read_valid(io) && io->write_block != NULL &&
	    io->flush != NULL;
}

static int
home_block_valid(const struct mfs4_journal_io *io, uint64_t block,
    uint32_t type)
{
	const struct mfs4_journal_geometry *g = &io->geometry;

	if (block >= g->total_blocks || block == 0 || block == 1 ||
	    (block >= MFS4_JOURNAL_START &&
	    block <= MFS4_JOURNAL_COMMIT_BLOCK) ||
	    block == g->backup_block)
		return 0;

	switch (type) {
	case MFS4_JMETA_IMAP:
		return block >= g->imap_start &&
		    block - g->imap_start < g->imap_blocks;
	case MFS4_JMETA_ZMAP:
		return block >= g->zmap_start &&
		    block - g->zmap_start < g->zmap_blocks;
	case MFS4_JMETA_INODE:
		return block >= g->inode_table_start &&
		    block - g->inode_table_start < g->inode_table_blocks;
	case MFS4_JMETA_STATE:
		return block == g->state_block;
	case MFS4_JMETA_INDIRECT:
	case MFS4_JMETA_DIRECTORY:
	case MFS4_JMETA_XATTR:
		return block >= g->data_start &&
		    block < g->backup_block;
	default:
		return 0;
	}
}

static int
read_commit(const struct mfs4_journal_io *io,
    uint8_t commit[MFS4_BLOCK_SIZE], int *valid)
{
	int r;

	r = io->read_block(io->cookie, MFS4_JOURNAL_COMMIT_BLOCK, commit);
	if (r != 0)
		return r;

	*valid = 0;
	if (memcmp(commit, MFS4_JOURNAL_COMMIT_MAGIC, 8) != 0)
		return 0;
	if (mfs4_get_le32(commit + MFS4_JC_MARKER) !=
	    MFS4_JOURNAL_COMMIT_MARKER)
		return 0;
	if (mfs4_get_le32(commit + MFS4_JC_CRC) !=
	    crc32c_block_with_zeroed_field(commit, MFS4_JC_CRC))
		return 0;
	*valid = 1;
	return 0;
}

static int
validate_header(const struct mfs4_journal_io *io,
    const uint8_t header[MFS4_BLOCK_SIZE],
    const uint8_t commit[MFS4_BLOCK_SIZE], uint32_t *count_out)
{
	uint32_t count, i, desc_bytes, crc;
	uint64_t home;

	if (memcmp(header, MFS4_JOURNAL_HEADER_MAGIC, 8) != 0 ||
	    mfs4_get_le32(header + MFS4_JH_VERSION) !=
	    MFS4_JOURNAL_VERSION)
		return EIO;

	count = mfs4_get_le32(header + MFS4_JH_COUNT);
	desc_bytes = mfs4_get_le32(header + MFS4_JH_DESC_BYTES);
	if (count == 0 || count > MFS4_JOURNAL_MAX_IMAGES ||
	    desc_bytes != count * MFS4_JDESC_SIZE ||
	    mfs4_get_le32(header + MFS4_JH_FLAGS) != 0 ||
	    mfs4_get_le64(header + MFS4_JH_VOLUME_GEN) !=
	    io->geometry.volume_generation ||
	    mfs4_get_le64(header + MFS4_JH_SEQUENCE) !=
	    mfs4_get_le64(commit + MFS4_JC_SEQUENCE) ||
	    count != mfs4_get_le32(commit + MFS4_JC_COUNT) ||
	    mfs4_get_le32(header + MFS4_JH_CRC) !=
	    mfs4_get_le32(commit + MFS4_JC_HEADER_CRC) ||
	    mfs4_get_le32(header + MFS4_JH_CRC) !=
	    crc32c_block_with_zeroed_field(header, MFS4_JH_CRC))
		return EIO;

	crc = MFS4_JH_DESCRIPTORS + desc_bytes;
	for (i = crc; i < MFS4_JH_CRC; i++) {
		if (header[i] != 0)
			return EIO;
	}

	for (i = 0; i < count; i++) {
		const uint8_t *desc = header + MFS4_JH_DESCRIPTORS +
		    i * MFS4_JDESC_SIZE;
		uint32_t type = mfs4_get_le32(desc + 12);
		uint32_t j;

		home = mfs4_get_le64(desc);
		if (!home_block_valid(io, home, type))
			return EIO;
		for (j = 0; j < i; j++) {
			const uint8_t *prior = header + MFS4_JH_DESCRIPTORS +
			    j * MFS4_JDESC_SIZE;
			if (mfs4_get_le64(prior) == home)
				return EIO;
		}
	}

	for (i = MFS4_JC_RESERVED; i < MFS4_JC_CRC; i++) {
		if (commit[i] != 0)
			return EIO;
	}
	*count_out = count;
	return 0;
}

int
mfs4_journal_pending(const struct mfs4_journal_io *io)
{
	uint8_t commit[MFS4_BLOCK_SIZE];
	int valid, r;

	if (!io_valid(io))
		return -EINVAL;
	r = read_commit(io, commit, &valid);
	if (r != 0)
		return -r;
	return valid;
}

int
mfs4_journal_validate(const struct mfs4_journal_io *io)
{
	uint8_t header[MFS4_BLOCK_SIZE], commit[MFS4_BLOCK_SIZE];
	uint8_t image[MFS4_BLOCK_SIZE];
	uint32_t count, i, crc_state, expected_crc;
	int valid, r;

	if (!io_read_valid(io))
		return -EINVAL;
	if ((r = read_commit(io, commit, &valid)) != 0)
		return -r;
	if (!valid)
		return 0;
	if ((r = io->read_block(io->cookie, MFS4_JOURNAL_HEADER_BLOCK,
	    header)) != 0)
		return -(r < 0 ? -r : EIO);
	if ((r = validate_header(io, header, commit, &count)) != 0)
		return -r;

	crc_state = 0xffffffffU;
	for (i = 0; i < count; i++) {
		const uint8_t *desc = header + MFS4_JH_DESCRIPTORS +
		    i * MFS4_JDESC_SIZE;
		if ((r = io->read_block(io->cookie,
		    MFS4_JOURNAL_IMAGES_START + i, image)) != 0)
			return -(r < 0 ? -r : EIO);
		expected_crc = mfs4_get_le32(desc + 8);
		if (crc32c_bytes(image, sizeof(image)) != expected_crc)
			return -EIO;
		crc_state = crc32c_update(crc_state, image, sizeof(image));
	}
	crc_state ^= 0xffffffffU;
	if (crc_state != mfs4_get_le32(commit + MFS4_JC_IMAGES_CRC))
		return -EIO;
	return 1;
}

int
mfs4_journal_tx_begin(struct mfs4_journal_tx *tx,
    const struct mfs4_journal_io *io, uint64_t sequence)
{
	int pending;

	if (tx == NULL || !io_valid(io))
		return EINVAL;
	pending = mfs4_journal_pending(io);
	if (pending < 0)
		return -pending;
	if (pending)
		return EBUSY;

	/* Reset descriptors only; all 64 image buffers are preallocated. */
	tx->io = io;
	tx->sequence = sequence;
	tx->count = 0;
	tx->active = 1;
	return 0;
}

int
mfs4_journal_tx_find_meta(struct mfs4_journal_tx *tx,
    uint64_t home_block, const uint8_t **image_out)
{
	uint32_t i;

	if (tx == NULL || !tx->active || image_out == NULL)
		return EINVAL;
	for (i = 0; i < tx->count; i++) {
		if (tx->images[i].home_block == home_block) {
			*image_out = tx->images[i].data;
			return 0;
		}
	}
	return ENOENT;
}

int
mfs4_journal_tx_get_meta(struct mfs4_journal_tx *tx,
    uint64_t home_block, uint32_t metadata_type, uint8_t **image_out)
{
	uint32_t i;
	int r;
	struct mfs4_journal_image *image;

	if (tx == NULL || !tx->active || image_out == NULL)
		return EINVAL;
	if (!home_block_valid(tx->io, home_block, metadata_type))
		return EINVAL;

	for (i = 0; i < tx->count; i++) {
		if (tx->images[i].home_block == home_block) {
			if (tx->images[i].metadata_type != metadata_type)
				return EINVAL;
			*image_out = tx->images[i].data;
			return 0;
		}
	}
	if (tx->count == MFS4_JOURNAL_MAX_IMAGES)
		return E2BIG;

	image = &tx->images[tx->count];
	memset(image, 0, sizeof(*image));
	r = tx->io->read_block(tx->io->cookie, home_block, image->data);
	if (r != 0)
		return r < 0 ? -r : EIO;
	image->home_block = home_block;
	image->metadata_type = metadata_type;
	tx->count++;
	*image_out = image->data;
	return 0;
}

void
mfs4_journal_tx_abort(struct mfs4_journal_tx *tx)
{
	if (tx == NULL)
		return;
	tx->active = 0;
	tx->count = 0;
	tx->io = NULL;
	tx->sequence = 0;
}

int
mfs4_journal_tx_commit(struct mfs4_journal_tx *tx)
{
	uint8_t block[MFS4_BLOCK_SIZE];
	uint32_t i, images_crc;
	int err, commit_attempted = 0;
	const struct mfs4_journal_io *io;

	if (tx == NULL || !tx->active || !io_valid(tx->io))
		return EINVAL;
	io = tx->io;
	if (tx->count == 0) {
		mfs4_journal_tx_abort(tx);
		return 0;
	}

	/*
	 * Ordered data must be stable before any metadata image can become
	 * committed. Callers must not dirty home metadata buffers during a tx.
	 */
	if ((err = io->flush(io->cookie)) != 0)
		goto precommit_fail;

	memset(block, 0, sizeof(block));
	memcpy(block, MFS4_JOURNAL_HEADER_MAGIC, 8);
	mfs4_put_le32(block + MFS4_JH_VERSION, MFS4_JOURNAL_VERSION);
	mfs4_put_le32(block + MFS4_JH_COUNT, tx->count);
	mfs4_put_le64(block + MFS4_JH_SEQUENCE, tx->sequence);
	mfs4_put_le64(block + MFS4_JH_VOLUME_GEN,
	    io->geometry.volume_generation);
	mfs4_put_le32(block + MFS4_JH_DESC_BYTES,
	    tx->count * MFS4_JDESC_SIZE);

	images_crc = 0xffffffffU;
	for (i = 0; i < tx->count; i++) {
		uint8_t *desc = block + MFS4_JH_DESCRIPTORS +
		    i * MFS4_JDESC_SIZE;
		struct mfs4_journal_image *image = &tx->images[i];

		mfs4_put_le64(desc, image->home_block);
		mfs4_put_le32(desc + 8, crc32c_bytes(image->data,
		    MFS4_BLOCK_SIZE));
		mfs4_put_le32(desc + 12, image->metadata_type);
		images_crc = crc32c_update(images_crc, image->data,
		    MFS4_BLOCK_SIZE);
	}
	images_crc ^= 0xffffffffU;
	mfs4_put_le32(block + MFS4_JH_CRC,
	    crc32c_block_with_zeroed_field(block, MFS4_JH_CRC));

	if ((err = io->write_block(io->cookie, MFS4_JOURNAL_HEADER_BLOCK,
	    block)) != 0)
		goto precommit_fail;
	for (i = 0; i < tx->count; i++) {
		if ((err = io->write_block(io->cookie,
		    MFS4_JOURNAL_IMAGES_START + i,
		    tx->images[i].data)) != 0)
			goto precommit_fail;
	}
	if ((err = io->flush(io->cookie)) != 0)
		goto precommit_fail;

	memset(block, 0, sizeof(block));
	memcpy(block, MFS4_JOURNAL_COMMIT_MAGIC, 8);
	mfs4_put_le64(block + MFS4_JC_SEQUENCE, tx->sequence);
	mfs4_put_le32(block + MFS4_JC_COUNT, tx->count);
	/* Header CRC is copied from the just-built header block. */
	{
		uint8_t header[MFS4_BLOCK_SIZE];
		if ((err = io->read_block(io->cookie,
		    MFS4_JOURNAL_HEADER_BLOCK, header)) != 0)
			goto precommit_fail;
		mfs4_put_le32(block + MFS4_JC_HEADER_CRC,
		    mfs4_get_le32(header + MFS4_JH_CRC));
	}
	mfs4_put_le32(block + MFS4_JC_IMAGES_CRC, images_crc);
	mfs4_put_le32(block + MFS4_JC_MARKER, MFS4_JOURNAL_COMMIT_MARKER);
	mfs4_put_le32(block + MFS4_JC_CRC,
	    crc32c_block_with_zeroed_field(block, MFS4_JC_CRC));

	commit_attempted = 1;
	if ((err = io->write_block(io->cookie,
	    MFS4_JOURNAL_COMMIT_BLOCK, block)) != 0)
		goto uncertain_fail;
	if ((err = io->flush(io->cookie)) != 0)
		goto uncertain_fail;

	/* Commit point reached. Redo images may now be applied to home blocks. */
	for (i = 0; i < tx->count; i++) {
		if ((err = io->write_block(io->cookie,
		    tx->images[i].home_block, tx->images[i].data)) != 0)
			goto uncertain_fail;
	}
	if ((err = io->flush(io->cookie)) != 0)
		goto uncertain_fail;

	memset(block, 0, sizeof(block));
	if ((err = io->write_block(io->cookie,
	    MFS4_JOURNAL_COMMIT_BLOCK, block)) != 0)
		goto uncertain_fail;
	if ((err = io->flush(io->cookie)) != 0)
		goto uncertain_fail;

	mfs4_journal_tx_abort(tx);
	return 0;

precommit_fail:
	mfs4_journal_tx_abort(tx);
	return err < 0 ? -err : EIO;

uncertain_fail:
	(void)commit_attempted;
	if (io->fail_closed != NULL)
		io->fail_closed(io->cookie);
	tx->active = 0;
	tx->io = NULL;
	return err < 0 ? -err : EIO;
}

int
mfs4_journal_replay(const struct mfs4_journal_io *io)
{
	uint8_t header[MFS4_BLOCK_SIZE], commit[MFS4_BLOCK_SIZE];
	uint8_t image[MFS4_BLOCK_SIZE], zero[MFS4_BLOCK_SIZE];
	uint32_t count, i, crc_state, expected_crc, desc_crc;
	int valid, r;

	if (!io_valid(io))
		return EINVAL;
	if ((r = read_commit(io, commit, &valid)) != 0)
		return r;
	if (!valid)
		return 0;

	if ((r = io->read_block(io->cookie, MFS4_JOURNAL_HEADER_BLOCK,
	    header)) != 0)
		return r < 0 ? -r : EIO;
	if ((r = validate_header(io, header, commit, &count)) != 0)
		return r;

	crc_state = 0xffffffffU;
	for (i = 0; i < count; i++) {
		const uint8_t *desc = header + MFS4_JH_DESCRIPTORS +
		    i * MFS4_JDESC_SIZE;
		expected_crc = mfs4_get_le32(desc + 8);
		if ((r = io->read_block(io->cookie,
		    MFS4_JOURNAL_IMAGES_START + i, image)) != 0)
			return r < 0 ? -r : EIO;
		desc_crc = crc32c_bytes(image, sizeof(image));
		if (desc_crc != expected_crc)
			return EIO;
		crc_state = crc32c_update(crc_state, image, sizeof(image));
	}
	crc_state ^= 0xffffffffU;
	if (crc_state != mfs4_get_le32(commit + MFS4_JC_IMAGES_CRC))
		return EIO;

	/* Do not modify home metadata until every descriptor and image is valid. */
	for (i = 0; i < count; i++) {
		const uint8_t *desc = header + MFS4_JH_DESCRIPTORS +
		    i * MFS4_JDESC_SIZE;
		if ((r = io->read_block(io->cookie,
		    MFS4_JOURNAL_IMAGES_START + i, image)) != 0)
			goto replay_fail;
		if ((r = io->write_block(io->cookie,
		    mfs4_get_le64(desc), image)) != 0)
			goto replay_fail;
	}
	if ((r = io->flush(io->cookie)) != 0)
		goto replay_fail;

	memset(zero, 0, sizeof(zero));
	if ((r = io->write_block(io->cookie,
	    MFS4_JOURNAL_COMMIT_BLOCK, zero)) != 0)
		goto replay_fail;
	if ((r = io->flush(io->cookie)) != 0)
		goto replay_fail;
	return 0;

replay_fail:
	if (io->fail_closed != NULL)
		io->fail_closed(io->cookie);
	return r < 0 ? -r : EIO;
}
