/*
 * MFS4 fsck dispatch and block-device adapter.
 *
 * The legacy checker remains responsible for MFS2/MFS3. This file is selected
 * only when an MFS4 primary or backup superblock magic is present.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/ioc_disk.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <minix/mfs4_disk.h>

#include "mfs4_fsck_core.h"
#include "fsck_mfs4.h"

struct fsck_mfs4_device {
	int fd;
	int regular_file;
};

static int
fsck_mfs4_seek(int fd, uint64_t offset)
{
	off_t where;

	if (offset > (uint64_t)LLONG_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	where = (off_t)offset;
	if ((uint64_t)where != offset || lseek(fd, where, SEEK_SET) == (off_t)-1)
		return -1;
	return 0;
}

static int
fsck_mfs4_read_all(int fd, void *buffer, size_t length)
{
	uint8_t *p = buffer;
	ssize_t n;

	while (length != 0) {
		n = read(fd, p, length);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			if (n == 0)
				errno = EIO;
			return -1;
		}
		p += n;
		length -= (size_t)n;
	}
	return 0;
}

static int
fsck_mfs4_write_all(int fd, const void *buffer, size_t length)
{
	const uint8_t *p = buffer;
	ssize_t n;

	while (length != 0) {
		n = write(fd, p, length);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			if (n == 0)
				errno = EIO;
			return -1;
		}
		p += n;
		length -= (size_t)n;
	}
	return 0;
}

static int
fsck_mfs4_read_block(void *cookie, uint64_t block, void *data)
{
	struct fsck_mfs4_device *device = cookie;
	uint64_t offset;

	if (block > UINT64_MAX / MFS4_BLOCK_SIZE)
		return EOVERFLOW;
	offset = block * MFS4_BLOCK_SIZE;
	if (fsck_mfs4_seek(device->fd, offset) != 0)
		return errno;
	if (fsck_mfs4_read_all(device->fd, data, MFS4_BLOCK_SIZE) != 0)
		return errno;
	return 0;
}

static int
fsck_mfs4_write_block(void *cookie, uint64_t block, const void *data)
{
	struct fsck_mfs4_device *device = cookie;
	uint64_t offset;

	if (block > UINT64_MAX / MFS4_BLOCK_SIZE)
		return EOVERFLOW;
	offset = block * MFS4_BLOCK_SIZE;
	if (fsck_mfs4_seek(device->fd, offset) != 0)
		return errno;
	if (fsck_mfs4_write_all(device->fd, data, MFS4_BLOCK_SIZE) != 0)
		return errno;
	return 0;
}

static int
fsck_mfs4_flush(void *cookie)
{
	struct fsck_mfs4_device *device = cookie;

	if (device->regular_file)
		return fsync(device->fd) == 0 ? 0 : errno;
	if (ioctl(device->fd, DIOCFLUSH, 0) != 0)
		return errno;
	return 0;
}

static int
fsck_mfs4_has_magic(int fd, uint64_t offset)
{
	uint8_t magic[MFS4_SUPER_MAGIC_SIZE];

	if (fsck_mfs4_seek(fd, offset) != 0 ||
	    fsck_mfs4_read_all(fd, magic, sizeof(magic)) != 0)
		return 0;
	return memcmp(magic, MFS4_SUPER_MAGIC, sizeof(magic)) == 0;
}

static int
fsck_mfs4_is_volume(int fd, uint64_t *device_blocks)
{
	off_t end;
	int primary;

	*device_blocks = 0;
	primary = fsck_mfs4_has_magic(fd, MFS4_SUPER_PRIMARY_OFFSET);
	end = lseek(fd, (off_t)0, SEEK_END);
	if (end > 0 && (uint64_t)end % MFS4_BLOCK_SIZE == 0)
		*device_blocks = (uint64_t)end / MFS4_BLOCK_SIZE;
	if (primary)
		return 1;
	if (*device_blocks > 0 &&
	    fsck_mfs4_has_magic(fd, (*device_blocks - 1) * MFS4_BLOCK_SIZE))
		return 1;
	(void)lseek(fd, (off_t)0, SEEK_SET);
	return 0;
}

static int
fsck_mfs4_repair_mode(int argc, char **argv, int *no_write)
{
	int repair_mode = MFS4_FSCK_CHECK_ONLY;
	int i;

	*no_write = 0;
	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-' || argv[i][1] == '\0' ||
		    argv[i][2] != '\0')
			continue;
		switch (argv[i][1]) {
		case 'n':
			*no_write = 1;
			break;
		case 'a':
		case 'p':
			if (repair_mode < MFS4_FSCK_REPAIR_SAFE)
				repair_mode = MFS4_FSCK_REPAIR_SAFE;
			break;
		case 'y':
			repair_mode = MFS4_FSCK_REPAIR_ALL;
			break;
		case 'r':
			return -1;
		case 'd':
		case 'l':
		case 'c':
		case 'i':
		case 'z':
		case 's':
			return -1;
		default:
			break;
		}
	}
	if (*no_write && repair_mode != MFS4_FSCK_CHECK_ONLY)
		return -1;
	return repair_mode;
}

int
fsck_mfs4_dispatch(int argc, char **argv, const char *path)
{
	struct fsck_mfs4_device device;
	struct mfs4_fsck_io io;
	struct mfs4_fsck_report report;
	uint64_t device_blocks;
	int no_write, repair_mode, flags, r, saved_errno;

	device.fd = open(path, O_RDONLY);
	if (device.fd < 0) {
		/* Let the legacy checker print its traditional diagnostic. */
		return FSCK_MFS4_NOT_MFS4;
	}
	if (!fsck_mfs4_is_volume(device.fd, &device_blocks)) {
		(void)close(device.fd);
		return FSCK_MFS4_NOT_MFS4;
	}

	repair_mode = fsck_mfs4_repair_mode(argc, argv, &no_write);
	if (repair_mode < 0) {
		fprintf(stderr,
		    "%s: MFS4 accepts -n (check only), -a/-p (safe repair), or "
		    "-y (all supported repairs); other legacy selection flags are unsupported\n",
		    path);
		(void)close(device.fd);
		return 2;
	}
	if (no_write || repair_mode == MFS4_FSCK_CHECK_ONLY)
		flags = O_RDONLY;
	else
		flags = O_RDWR;
	(void)close(device.fd);
	device.fd = open(path, flags);
	if (device.fd < 0) {
		perror(path);
		return 1;
	}
	{
		struct stat st;
		if (fstat(device.fd, &st) != 0) {
			perror(path);
			(void)close(device.fd);
			return 1;
		}
		device.regular_file = S_ISREG(st.st_mode);
	}

	memset(&io, 0, sizeof(io));
	io.cookie = &device;
	io.read_block = fsck_mfs4_read_block;
	io.write_block = fsck_mfs4_write_block;
	io.flush = fsck_mfs4_flush;

	r = mfs4_fsck_run(&io, device_blocks, repair_mode, &report);
	saved_errno = r;
	if (r == EBUSY) {
		fprintf(stderr,
		    "%s: committed MFS4 journal transaction pending; "
		    "use -a/-p/-y to replay and repair, -n made no changes\n", path);
	} else if (r == EOPNOTSUPP) {
		fprintf(stderr,
		    "%s: unsupported MFS4 feature; volume was not modified\n",
		    path);
	} else if (r == EIO && report.unreferenced_data_blocks != 0 &&
	    repair_mode == MFS4_FSCK_REPAIR_SAFE) {
		fprintf(stderr,
		    "%s: %" PRIu64 " allocated data blocks are unreferenced; "
		    "they were preserved. Back up the image and use -y only if "
		    "these blocks may be discarded\n",
		    path, report.unreferenced_data_blocks);
	} else if (r != 0) {
		fprintf(stderr, "%s: MFS4 consistency check failed: %s\n",
		    path, strerror(r));
	}
	if (r == 0) {
		printf("%s: MFS4: %" PRIu64 " blocks, %" PRIu32
		    " inodes (%" PRIu32 " allocated), %" PRIu64
		    " data blocks (%" PRIu64 " referenced), %" PRIu32
		    " orphans; journal %s; state %s\n",
		    path, report.total_blocks, report.total_inodes,
		    report.used_inodes, report.data_blocks,
		    report.used_data_blocks, report.orphan_inodes,
		    report.journal_replayed ? "replayed" :
		    (report.journal_pending ? "pending" : "empty"),
		    report.clean ? "clean" : "dirty");
		if (report.superblock_degraded)
			printf("%s: warning: only one superblock copy is valid; "
			    "use -a/-p/-y to restore its peer\n", path);
		if (report.repairs_made != 0)
			printf("%s: applied %" PRIu64 " repair operation(s)\n",
			    path, report.repairs_made);
		if (!report.clean && report.orphan_inodes != 0)
			printf("%s: orphan recovery remains pending; volume left dirty "
			    "for writable MFS mount recovery\n", path);
	}
	if (close(device.fd) != 0 && r == 0)
		return 1;
	if (saved_errno == 0)
		return 0;
	return 1;
}
