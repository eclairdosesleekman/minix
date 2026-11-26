/*	$NetBSD$	*/

/*
 * MFS4 image creator.  This code is linked into the existing MFS3 mkfs
 * executable; MFS3 remains the default format and uses its original code
 * path unchanged.
 */
#include <sys/types.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <minix/mfs4_disk.h>

#if defined(__minix)
#include <minix/minlib.h>
#include <minix/partition.h>
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

#define MFS4_ROOT_INODE			1U
#define MFS4_ROOT_MODE			0040755U
#define MFS4_DEFAULT_INODES_PER_BLOCK	16U

struct mfs4_geometry {
	uint64_t total_blocks;
	uint64_t total_inodes;
	uint64_t imap_start;
	uint64_t imap_blocks;
	uint64_t zmap_start;
	uint64_t zmap_blocks;
	uint64_t inode_table_start;
	uint64_t inode_table_blocks;
	uint64_t state_block;
	uint64_t data_start;
	uint64_t backup_block;
	uint64_t data_blocks;
};

/* check_mtab() is provided by the existing mkfs.c implementation. */
extern void check_mtab(const char *);

static uint64_t
mfs4_parse_u64(const char *option, const char *value)
{
	char *end;
	unsigned long long result;

	errno = 0;
	result = strtoull(value, &end, 0);
	if (errno != 0 || value == end || *end != '\0' || value[0] == '-')
		errx(1, "invalid value for %s: %s", option, value);
	return (uint64_t)result;
}

static int
mfs4_option_value(int argc, char **argv, int *index, const char *option,
    const char **value)
{
	size_t n = strlen(option);

	if (strcmp(argv[*index], option) == 0) {
		if (*index + 1 >= argc)
			errx(1, "missing argument for %s", option);
		(*index)++;
		*value = argv[*index];
		return 1;
	}
	if (strncmp(argv[*index], option, n) == 0 && argv[*index][n] != '\0') {
		*value = argv[*index] + n;
		return 1;
	}
	return 0;
}

/*
 * Called before the legacy getopt loop.  Return true only for -V 4 (or -V4)
 * so that old invocations and -V 3 keep using the original MFS3 formatter.
 */
int
mfs4_mkfs_requested(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0)
			break;
		if (strcmp(argv[i], "-V") == 0) {
			if (i + 1 < argc && strcmp(argv[i + 1], "4") == 0)
				return 1;
			if (i + 1 < argc && strcmp(argv[i + 1], "3") == 0)
				return 0;
		}
		if (strcmp(argv[i], "-V4") == 0)
			return 1;
		if (strcmp(argv[i], "-V3") == 0)
			return 0;
	}
	return 0;
}

static uint64_t
mfs4_ceil_div(uint64_t numerator, uint64_t denominator)
{
	return numerator / denominator + (numerator % denominator != 0);
}

static int
mfs4_compute_geometry(uint64_t blocks, uint64_t inodes,
    struct mfs4_geometry *g, char *error, size_t error_size)
{
	uint64_t imap_blocks, itable_blocks, zmap_blocks, next_zmap;
	uint64_t zmap_start, itable_start, state_block, data_start;
	unsigned int iteration;

	if (blocks < MFS4_IMAP_START + 5) {
		(void)snprintf(error, error_size, "volume is too small");
		return -1;
	}
	if (blocks > MFS4_MAX_BLOCKS) {
		(void)snprintf(error, error_size,
		    "block count exceeds the 32-bit block address space");
		return -1;
	}
	if (inodes == 0 || inodes > MFS4_MAX_INODES) {
		(void)snprintf(error, error_size,
		    "inode count must be in the range 1..UINT32_MAX");
		return -1;
	}

	/* Add the permanently reserved inode zero using checked-width arithmetic. */
	imap_blocks = mfs4_ceil_div(inodes + 1, MFS4_BITS_PER_BLOCK);
	itable_blocks = mfs4_ceil_div(inodes + 1, MFS4_INODES_PER_BLOCK);
	if (imap_blocks > UINT32_MAX || itable_blocks > MFS4_MAX_BLOCKS) {
		(void)snprintf(error, error_size, "inode metadata is too large");
		return -1;
	}

	/* Solve ZM = ceil((backup - data_start) / bits_per_block). */
	zmap_blocks = 1;
	for (iteration = 0; iteration < 64; iteration++) {
		zmap_start = MFS4_IMAP_START + imap_blocks;
		if (zmap_start < MFS4_IMAP_START ||
		    zmap_blocks > MFS4_MAX_BLOCKS - zmap_start) {
			(void)snprintf(error, error_size, "zmap geometry overflow");
			return -1;
		}
		itable_start = zmap_start + zmap_blocks;
		if (itable_blocks > MFS4_MAX_BLOCKS - itable_start) {
			(void)snprintf(error, error_size, "inode table geometry overflow");
			return -1;
		}
		state_block = itable_start + itable_blocks;
		if (state_block == UINT64_MAX) {
			(void)snprintf(error, error_size, "state block geometry overflow");
			return -1;
		}
		data_start = state_block + 1;
		if (blocks - 1 <= data_start) {
			(void)snprintf(error, error_size,
			    "volume has no room for data blocks and backup superblock");
			return -1;
		}
		next_zmap = mfs4_ceil_div((blocks - 1) - data_start,
		    MFS4_BITS_PER_BLOCK);
		if (next_zmap == 0)
			next_zmap = 1;
		if (next_zmap == zmap_blocks)
			break;
		zmap_blocks = next_zmap;
	}
	if (iteration == 64) {
		(void)snprintf(error, error_size, "zmap geometry did not converge");
		return -1;
	}

	/* Recompute final values and verify every region is within the volume. */
	zmap_start = MFS4_IMAP_START + imap_blocks;
	itable_start = zmap_start + zmap_blocks;
	state_block = itable_start + itable_blocks;
	data_start = state_block + 1;
	if (data_start >= blocks - 1 || blocks - 1 > UINT32_MAX) {
		(void)snprintf(error, error_size, "invalid final filesystem geometry");
		return -1;
	}
	if (zmap_blocks > UINT32_MAX) {
		(void)snprintf(error, error_size, "zmap is too large");
		return -1;
	}

	memset(g, 0, sizeof(*g));
	g->total_blocks = blocks;
	g->total_inodes = inodes;
	g->imap_start = MFS4_IMAP_START;
	g->imap_blocks = imap_blocks;
	g->zmap_start = zmap_start;
	g->zmap_blocks = zmap_blocks;
	g->inode_table_start = itable_start;
	g->inode_table_blocks = itable_blocks;
	g->state_block = state_block;
	g->data_start = data_start;
	g->backup_block = blocks - 1;
	g->data_blocks = g->backup_block - data_start;
	return 0;
}

static int
mfs4_read_uuid(uint8_t uuid[16])
{
	int fd;
	size_t done = 0;
	ssize_t n;

	fd = open("/dev/urandom", O_RDONLY | O_BINARY);
	if (fd < 0)
		return -1;
	while (done < 16) {
		n = read(fd, uuid + done, 16 - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			(void)close(fd);
			return -1;
		}
		done += (size_t)n;
	}
	(void)close(fd);
	/* RFC 4122 version 4 / variant bits, for UUID interoperability. */
	uuid[6] = (uint8_t)((uuid[6] & 0x0fU) | 0x40U);
	uuid[8] = (uint8_t)((uuid[8] & 0x3fU) | 0x80U);
	return 0;
}

static int
mfs4_write_all(int fd, const void *buffer, size_t length)
{
	const uint8_t *p = (const uint8_t *)buffer;
	ssize_t n;

	while (length != 0) {
		n = write(fd, p, length);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		length -= (size_t)n;
	}
	return 0;
}

static int
mfs4_write_at(int fd, uint64_t offset, const void *buffer, size_t length)
{
	off_t pos;

	if (offset > (uint64_t)LLONG_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	pos = (off_t)offset;
	if ((uint64_t)pos != offset || lseek(fd, pos, SEEK_SET) == (off_t)-1)
		return -1;
	return mfs4_write_all(fd, buffer, length);
}

static int
mfs4_write_block(int fd, uint64_t block, const uint8_t *buffer)
{
	if (block > (UINT64_MAX / MFS4_BLOCK_SIZE)) {
		errno = EOVERFLOW;
		return -1;
	}
	return mfs4_write_at(fd, block * MFS4_BLOCK_SIZE, buffer,
	    MFS4_BLOCK_SIZE);
}

static void
mfs4_set_bit(uint8_t *map, uint32_t bit)
{
	map[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
}

static void
mfs4_make_imap(uint8_t *block, uint64_t block_index, uint64_t inodes)
{
	uint64_t first = block_index * MFS4_BITS_PER_BLOCK;
	uint32_t bit;
	uint64_t index;

	memset(block, 0, MFS4_BLOCK_SIZE);
	for (bit = 0; bit < MFS4_BITS_PER_BLOCK; bit++) {
		index = first + bit;
		if (index == 0 || index == MFS4_ROOT_INODE || index > inodes)
			mfs4_set_bit(block, bit);
	}
}

static void
mfs4_make_zmap(uint8_t *block, uint64_t block_index, uint64_t data_blocks)
{
	uint64_t first = block_index * MFS4_BITS_PER_BLOCK;
	uint32_t bit;
	uint64_t index;

	memset(block, 0, MFS4_BLOCK_SIZE);
	for (bit = 0; bit < MFS4_BITS_PER_BLOCK; bit++) {
		index = first + bit;
		/* The first data block contains the root directory. */
		if (index == 0 || index >= data_blocks)
			mfs4_set_bit(block, bit);
	}
}

static void
mfs4_make_super(uint8_t raw[MFS4_SUPER_SIZE],
    const struct mfs4_geometry *g, const uint8_t uuid[16], int64_t now)
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
	mfs4_put_le64(raw + MFS4_SB_TOTAL_BLOCKS, g->total_blocks);
	mfs4_put_le32(raw + MFS4_SB_TOTAL_INODES, (uint32_t)g->total_inodes);
	mfs4_put_le64(raw + MFS4_SB_IMAP_START, g->imap_start);
	mfs4_put_le32(raw + MFS4_SB_IMAP_BLOCKS, (uint32_t)g->imap_blocks);
	mfs4_put_le32(raw + MFS4_SB_ZMAP_BLOCKS, (uint32_t)g->zmap_blocks);
	mfs4_put_le64(raw + MFS4_SB_ZMAP_START, g->zmap_start);
	mfs4_put_le64(raw + MFS4_SB_INODE_TABLE_START,
	    g->inode_table_start);
	mfs4_put_le64(raw + MFS4_SB_INODE_TABLE_BLOCKS,
	    g->inode_table_blocks);
	mfs4_put_le64(raw + MFS4_SB_STATE_BLOCK, g->state_block);
	mfs4_put_le64(raw + MFS4_SB_DATA_START, g->data_start);
	mfs4_put_le64(raw + MFS4_SB_JOURNAL_START, MFS4_JOURNAL_START);
	mfs4_put_le32(raw + MFS4_SB_JOURNAL_BLOCKS, MFS4_JOURNAL_BLOCKS);
	mfs4_put_le32(raw + MFS4_SB_MAX_TX_IMAGES, MFS4_JOURNAL_MAX_IMAGES);
	mfs4_put_le64(raw + MFS4_SB_BACKUP_BLOCK, g->backup_block);
	mfs4_put_le32(raw + MFS4_SB_ROOT_INODE, MFS4_ROOT_INODE);
	mfs4_put_le64(raw + MFS4_SB_MAX_FILE_SIZE, MFS4_MAX_FILE_SIZE);
	mfs4_put_le64(raw + MFS4_SB_VOLUME_GENERATION, 1);
	memcpy(raw + MFS4_SB_UUID, uuid, 16);
	mfs4_put_le64(raw + MFS4_SB_CREATED_AT, (uint64_t)now);
	mfs4_put_le32(raw + MFS4_SB_CRC32C,
	    mfs4_crc32c(raw, MFS4_SB_CRC32C));
}

static void
mfs4_make_inode(uint8_t raw[MFS4_INODE_SIZE], uint32_t data_block,
    int64_t now)
{
	memset(raw, 0, MFS4_INODE_SIZE);
	mfs4_put_le16(raw + MFS4_I_MODE, MFS4_ROOT_MODE);
	mfs4_put_le32(raw + MFS4_I_NLINK, 2);
	mfs4_put_le64(raw + MFS4_I_SIZE, 2 * MFS4_DIRENT_SIZE);
	mfs4_put_le64(raw + MFS4_I_ATIME_SEC, (uint64_t)now);
	mfs4_put_le64(raw + MFS4_I_MTIME_SEC, (uint64_t)now);
	mfs4_put_le64(raw + MFS4_I_CTIME_SEC, (uint64_t)now);
	mfs4_put_le32(raw + MFS4_I_GENERATION, 1);
	mfs4_put_le32(raw + MFS4_I_DIRECT, data_block);
}

static void
mfs4_make_state(uint8_t block[MFS4_BLOCK_SIZE])
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
mfs4_make_root_dir(uint8_t block[MFS4_BLOCK_SIZE])
{
	memset(block, 0, MFS4_BLOCK_SIZE);
	mfs4_put_le32(block + MFS4_DIRENT_INODE, MFS4_ROOT_INODE);
	memcpy(block + MFS4_DIRENT_NAME, ".", 1);
	mfs4_put_le32(block + MFS4_DIRENT_SIZE + MFS4_DIRENT_INODE,
	    MFS4_ROOT_INODE);
	memcpy(block + MFS4_DIRENT_SIZE + MFS4_DIRENT_NAME, "..", 2);
}

static void
mfs4_print_geometry(const struct mfs4_geometry *g)
{
	(void)printf("MFS4 layout (4096-byte blocks)\n");
	(void)printf("  total blocks:       %" PRIu64 "\n", g->total_blocks);
	(void)printf("  total inodes:       %" PRIu64 "\n", g->total_inodes);
	(void)printf("  journal:            %u..%u (%u blocks)\n",
	    MFS4_JOURNAL_START,
	    MFS4_JOURNAL_START + MFS4_JOURNAL_BLOCKS - 1,
	    MFS4_JOURNAL_BLOCKS);
	(void)printf("  inode bitmap:       %" PRIu64 " + %" PRIu64 " blocks\n",
	    g->imap_start, g->imap_blocks);
	(void)printf("  data bitmap:        %" PRIu64 " + %" PRIu64 " blocks\n",
	    g->zmap_start, g->zmap_blocks);
	(void)printf("  inode table:        %" PRIu64 " + %" PRIu64 " blocks\n",
	    g->inode_table_start, g->inode_table_blocks);
	(void)printf("  state block:        %" PRIu64 "\n", g->state_block);
	(void)printf("  data area:          %" PRIu64 "..%" PRIu64
	    " (%" PRIu64 " blocks)\n", g->data_start,
	    g->backup_block - 1, g->data_blocks);
	(void)printf("  root inode/block:   %u/%" PRIu64 "\n",
	    MFS4_ROOT_INODE, g->data_start);
	(void)printf("  backup superblock:  %" PRIu64 "\n", g->backup_block);
}

static int
mfs4_format(int fd, const struct mfs4_geometry *g)
{
	uint8_t block[MFS4_BLOCK_SIZE];
	uint8_t super[MFS4_SUPER_SIZE];
	uint8_t inode[MFS4_INODE_SIZE];
	uint8_t uuid[16];
	uint64_t i, inode_block, inode_offset;
	int64_t now;

	if (mfs4_read_uuid(uuid) < 0)
		return -1;
	now = (int64_t)time(NULL);

	/* Clear boot/reserved and the complete fixed-size journal area. */
	memset(block, 0, sizeof(block));
	for (i = 0; i < MFS4_IMAP_START; i++) {
		if (mfs4_write_block(fd, i, block) < 0)
			return -1;
	}
	/* Invalidate a previous backup superblock before publishing this one. */
	if (mfs4_write_block(fd, g->backup_block, block) < 0)
		return -1;

	/* Inode bitmap: inode zero is reserved, inode one is the root. */
	for (i = 0; i < g->imap_blocks; i++) {
		mfs4_make_imap(block, i, g->total_inodes);
		if (mfs4_write_block(fd, g->imap_start + i, block) < 0)
			return -1;
	}

	/* Data bitmap covers only [data_start, backup_block). */
	for (i = 0; i < g->zmap_blocks; i++) {
		mfs4_make_zmap(block, i, g->data_blocks);
		if (mfs4_write_block(fd, g->zmap_start + i, block) < 0)
			return -1;
	}

	memset(block, 0, sizeof(block));
	for (i = 0; i < g->inode_table_blocks; i++) {
		if (mfs4_write_block(fd, g->inode_table_start + i, block) < 0)
			return -1;
	}

	/* Inode zero remains empty; inode one is the root directory. */
	mfs4_make_inode(inode, (uint32_t)g->data_start, now);
	inode_block = g->inode_table_start +
	    (MFS4_ROOT_INODE * MFS4_INODE_SIZE) / MFS4_BLOCK_SIZE;
	inode_offset = (MFS4_ROOT_INODE * MFS4_INODE_SIZE) % MFS4_BLOCK_SIZE;
	memset(block, 0, sizeof(block));
	memcpy(block + inode_offset, inode, sizeof(inode));
	if (mfs4_write_block(fd, inode_block, block) < 0)
		return -1;

	mfs4_make_state(block);
	if (mfs4_write_block(fd, g->state_block, block) < 0)
		return -1;
	mfs4_make_root_dir(block);
	if (mfs4_write_block(fd, g->data_start, block) < 0)
		return -1;

	mfs4_make_super(super, g, uuid, now);

	/* Publish valid superblocks only after all referenced metadata exists. */
	memset(block, 0, sizeof(block));
	memcpy(block + MFS4_SUPER_PRIMARY_OFFSET, super, sizeof(super));
	if (mfs4_write_block(fd, 0, block) < 0)
		return -1;
	memset(block, 0, sizeof(block));
	memcpy(block, super, sizeof(super));
	if (mfs4_write_block(fd, g->backup_block, block) < 0)
		return -1;
	if (fsync(fd) < 0)
		return -1;
	return 0;
}

int
mfs4_mkfs_main(int argc, char **argv)
{
	const char *target = NULL, *value;
	uint64_t blocks = 0, inodes = 0, capacity_bytes = 0, capacity_blocks;
	uint64_t bytes_needed;
	int have_blocks = 0, have_inodes = 0, dry_run = 0, verbose = 0;
	int force = 0;
	int fd, i, open_flags;
	struct stat st;
	struct mfs4_geometry geometry;
	char error[160];

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0) {
			if (++i < argc && target == NULL)
				target = argv[i++];
			if (i != argc)
				errx(1, "too many operands");
			break;
		}
		if (mfs4_option_value(argc, argv, &i, "-V", &value)) {
			if (strcmp(value, "4") != 0)
				errx(1, "MFS4 formatter requires -V 4");
		} else if (mfs4_option_value(argc, argv, &i, "-i", &value)) {
			inodes = mfs4_parse_u64("-i", value);
			have_inodes = 1;
		} else if (mfs4_option_value(argc, argv, &i, "-b", &value)) {
			blocks = mfs4_parse_u64("-b", value);
			have_blocks = 1;
		} else if (mfs4_option_value(argc, argv, &i, "-B", &value)) {
			if (mfs4_parse_u64("-B", value) != MFS4_BLOCK_SIZE)
				errx(1, "MFS4 requires a 4096-byte block size");
		} else if (strcmp(argv[i], "-n") == 0) {
			dry_run = 1;
		} else if (strcmp(argv[i], "-v") == 0) {
			verbose++;
		} else if (strcmp(argv[i], "-f") == 0) {
			force = 1;
		} else if (argv[i][0] == '-') {
			errx(1, "unknown MFS4 mkfs option: %s", argv[i]);
		} else if (target == NULL) {
			target = argv[i];
		} else {
			errx(1, "too many operands");
		}
	}

	if (target == NULL)
		errx(1, "usage: mkfs.mfs -V 4 [-n] [-f] [-B 4096] [-i inodes] [-b blocks] special");

	open_flags = dry_run ? O_RDONLY : O_RDWR;
	fd = open(target, open_flags | O_BINARY);
	if (fd < 0)
		err(1, "cannot open %s", target);
	if (fstat(fd, &st) < 0)
		err(1, "cannot stat %s", target);

	if (S_ISREG(st.st_mode)) {	/* Regular image file. */
		if (st.st_size < 0)
			errx(1, "negative image size");
		capacity_bytes = (uint64_t)st.st_size;
	} else {
#if defined(__minix)
		if (minix_sizeup(target, &capacity_bytes) < 0)
			err(1, "cannot determine capacity of %s", target);
#else
		{
			off_t end = lseek(fd, 0, SEEK_END);
			if (end >= 0)
				capacity_bytes = (uint64_t)end;
		}
#endif
	}
	capacity_blocks = capacity_bytes / MFS4_BLOCK_SIZE;
	if (!have_blocks) {
		if (capacity_blocks == 0)
			errx(1, "cannot determine device size; specify -b blocks");
		blocks = capacity_blocks;
	} else if (capacity_blocks != 0 && blocks > capacity_blocks) {
		errx(1, "requested size exceeds device capacity");
	}

	if (!have_inodes) {
		/* A conservative default: one inode per 16 filesystem blocks. */
		inodes = blocks / MFS4_DEFAULT_INODES_PER_BLOCK;
		if (inodes < 16)
			inodes = 16;
	}
	if (mfs4_compute_geometry(blocks, inodes, &geometry, error,
	    sizeof(error)) < 0)
		errx(1, "invalid MFS4 geometry: %s", error);
	if (blocks > UINT64_MAX / MFS4_BLOCK_SIZE)
		errx(1, "filesystem byte size overflows");
	bytes_needed = blocks * MFS4_BLOCK_SIZE;
	if (capacity_bytes != 0 && bytes_needed > capacity_bytes)
		errx(1, "filesystem byte size exceeds device capacity");

	mfs4_print_geometry(&geometry);
	if (dry_run) {
		(void)printf("  mode:               dry-run (no writes performed)\n");
		(void)close(fd);
		return 0;
	}

	/*
	 * Formatting destroys the existing contents. Require an affirmative
	 * interactive confirmation unless the caller explicitly supplies -f.
	 * EOF, an empty line, and every response other than y/yes fail closed.
	 */
	(void)printf("About to format %s as MFS4 (%" PRIu64
	    " bytes, %" PRIu64 " 4096-byte blocks).\n",
	    target, bytes_needed, blocks);
	(void)printf("WARNING: all existing data on this target will be destroyed.\n");
	if (!force) {
		char response[32];

		(void)printf("Continue? [y/N] ");
		(void)fflush(stdout);
		if (fgets(response, sizeof(response), stdin) == NULL) {
			(void)close(fd);
			errx(1, "format cancelled (no confirmation received; use -f to force)");
		}
		response[strcspn(response, "\r\n")] = '\0';
		if (strcmp(response, "y") != 0 &&
		    strcmp(response, "Y") != 0 &&
		    strcmp(response, "yes") != 0 &&
		    strcmp(response, "YES") != 0) {
			(void)close(fd);
			errx(1, "format cancelled");
		}
	} else {
		(void)printf("Force mode: confirmation skipped by -f.\n");
	}

	(void)close(fd);
	check_mtab(target);
	fd = open(target, O_RDWR | O_BINARY);
	if (fd < 0)
		err(1, "cannot open %s for writing", target);
	if (mfs4_format(fd, &geometry) < 0)
		err(1, "failed to write MFS4 filesystem to %s", target);
	if (close(fd) < 0)
		err(1, "close %s", target);
	if (verbose)
		(void)fprintf(stderr, "MFS4 filesystem created on %s\n", target);
	return 0;
}
