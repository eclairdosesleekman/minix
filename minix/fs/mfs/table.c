
/* This file contains the table used to map file system calls onto the
 * routines that perform them.
 */

#define _TABLE

#include "fs.h"
#include "inode.h"
#include "buf.h"
#include "super.h"
#include <sys/time.h>
#include <minix/mfs4_disk.h>

/*
 * One VFS mutation is one MFS4 metadata transaction.  All MFS4 metadata
 * serializers (inode, bitmap, indirect block and directory) join this
 * transaction through the shadow-image API.  MFS3 continues to call the
 * original entry points directly.
 */
static int
mfs4_mutation_begin(void)
{
	if (superblock.s_version != V4)
		return OK;
	return mfs4_tx_begin();
}

static int
mfs4_mutation_end(int result)
{
	int r;

	if (superblock.s_version != V4)
		return result;
	if (result != OK) {
		mfs4_tx_abort();
		return result;
	}
	r = mfs4_tx_commit();
	if (r != OK)
		return r;

	/* A successful close/unlink/truncate may make the list head reclaimable.
	 * Reclaim only after the triggering namespace/inode transaction commits.
	 */
	r = mfs4_orphan_process(0);
	if (r != OK) {
		superblock.s_rd_only = 1;
		printf("MFS: MFS4 orphan cleanup failed (%d); forcing read-only\n",
		    r);
		return r;
	}
	return result;
}

/*
 * Keep each ordinary file write below the 64-image journal limit.  Eight
 * blocks plus an unaligned first block touch at most nine logical data blocks;
 * the corresponding inode, bitmap and indirect metadata fit comfortably
 * within one fixed journal transaction.  Return only bytes whose chunk has
 * crossed its commit point.
 */
#define MFS4_WRITE_TX_CHUNK_BYTES (8U * MFS4_BLOCK_SIZE)

static ssize_t
mfs4_write_request(ino_t ino_nr, struct fsdriver_data *data, size_t bytes,
	off_t pos, int call)
{
	size_t committed = 0, chunk;
	ssize_t result;
	int r;

	if (superblock.s_version != V4 || call != FSC_WRITE)
		return fs_readwrite(ino_nr, data, bytes, pos, call);
	if (bytes == 0)
		return 0;
	if (data == NULL || pos < 0 || bytes > data->size)
		return -EINVAL;
	if ((uint64_t)pos > MFS4_MAX_FILE_SIZE ||
	    (uint64_t)bytes > MFS4_MAX_FILE_SIZE - (uint64_t)pos)
		return -EFBIG;

	while (committed < bytes) {
		chunk = bytes - committed;
		if (chunk > MFS4_WRITE_TX_CHUNK_BYTES)
			chunk = MFS4_WRITE_TX_CHUNK_BYTES;

		if ((r = mfs4_mutation_begin()) != OK)
			return committed != 0 ? (ssize_t)committed : -r;
		result = fs_readwrite_offset(ino_nr, data, chunk,
		    pos + (off_t)committed, call, committed);
		if (result < 0) {
			mfs4_tx_abort();
			return committed != 0 ? (ssize_t)committed : result;
		}
		if (result == 0) {
			mfs4_tx_abort();
			return (ssize_t)committed;
		}
		if ((r = mfs4_tx_commit()) != OK)
			return committed != 0 ? (ssize_t)committed : -r;

		committed += (size_t)result;
		if ((size_t)result < chunk)
			break;
	}
	return (ssize_t)committed;
}

static int
mfs4_putnode_request(ino_t ino_nr, unsigned int count)
{
	int r = mfs4_mutation_begin();
	if (r != OK) return r;
	return mfs4_mutation_end(fs_putnode(ino_nr, count));
}

static int
mfs4_trunc_request(ino_t ino_nr, off_t start, off_t end)
{
	int r = mfs4_mutation_begin();
	if (r != OK) return r;
	return mfs4_mutation_end(fs_trunc(ino_nr, start, end));
}

static int
mfs4_create_request(ino_t dir_nr, char *name, mode_t mode, uid_t uid,
	gid_t gid, struct fsdriver_node *node)
{
	int r = mfs4_mutation_begin();
	if (r != OK) return r;
	return mfs4_mutation_end(fs_create(dir_nr, name, mode, uid, gid, node));
}

static int
mfs4_mkdir_request(ino_t dir_nr, char *name, mode_t mode, uid_t uid, gid_t gid)
{
	int r = mfs4_mutation_begin();
	if (r != OK) return r;
	return mfs4_mutation_end(fs_mkdir(dir_nr, name, mode, uid, gid));
}

static int
mfs4_mknod_request(ino_t dir_nr, char *name, mode_t mode, uid_t uid,
	gid_t gid, dev_t dev)
{
	int r = mfs4_mutation_begin();
	if (r != OK) return r;
	return mfs4_mutation_end(fs_mknod(dir_nr, name, mode, uid, gid, dev));
}

static int
mfs4_link_request(ino_t dir_nr, char *name, ino_t ino_nr)
{
	int r = mfs4_mutation_begin();
	if (r != OK) return r;
	return mfs4_mutation_end(fs_link(dir_nr, name, ino_nr));
}

static int
mfs4_unlink_request(ino_t dir_nr, char *name, int call)
{
	int r = mfs4_mutation_begin();
	if (r != OK) return r;
	return mfs4_mutation_end(fs_unlink(dir_nr, name, call));
}

static int
mfs4_rename_request(ino_t old_dir_nr, char *old_name, ino_t new_dir_nr,
	char *new_name)
{
	int r = mfs4_mutation_begin();
	if (r != OK) return r;
	return mfs4_mutation_end(fs_rename(old_dir_nr, old_name, new_dir_nr,
	    new_name));
}

static int
mfs4_slink_request(ino_t dir_nr, char *name, uid_t uid, gid_t gid,
	struct fsdriver_data *data, size_t bytes)
{
	int r = mfs4_mutation_begin();
	if (r != OK) return r;
	return mfs4_mutation_end(fs_slink(dir_nr, name, uid, gid, data, bytes));
}

static int
mfs4_chown_request(ino_t ino_nr, uid_t uid, gid_t gid, mode_t *mode)
{
	int r = mfs4_mutation_begin();
	if (r != OK) return r;
	return mfs4_mutation_end(fs_chown(ino_nr, uid, gid, mode));
}

static int
mfs4_chmod_request(ino_t ino_nr, mode_t *mode)
{
	int r = mfs4_mutation_begin();
	if (r != OK) return r;
	return mfs4_mutation_end(fs_chmod(ino_nr, mode));
}

static int
mfs4_utime_request(ino_t ino_nr, struct timespec *atime,
	struct timespec *mtime)
{
	int r = mfs4_mutation_begin();
	if (r != OK) return r;
	return mfs4_mutation_end(fs_utime(ino_nr, atime, mtime));
}


static struct inode *
mfs4_xattr_get_inode(ino_t ino_nr)
{
	if (superblock.s_version != V4) {
		err_code = EOPNOTSUPP;
		return NULL;
	}
	return get_inode(fs_dev, ino_nr);
}

static ssize_t
mfs4_getxattr_request(ino_t ino_nr, const char *name,
	struct fsdriver_data *data, size_t size)
{
	struct inode *rip;
	ssize_t r;

	if ((rip = mfs4_xattr_get_inode(ino_nr)) == NULL)
		return -(err_code != OK ? err_code : EIO);
	r = mfs4_getxattr(rip, name, data, size);
	put_inode(rip);
	return r;
}

static int
mfs4_setxattr_request(ino_t ino_nr, const char *name,
	struct fsdriver_data *data, size_t size, int flags)
{
	struct inode *rip;
	int r;

	if ((rip = mfs4_xattr_get_inode(ino_nr)) == NULL)
		return err_code != OK ? err_code : EIO;
	if (superblock.s_rd_only)
		r = EROFS;
	else
		r = mfs4_setxattr(rip, name, data, size, flags);
	put_inode(rip);
	return r;
}

static ssize_t
mfs4_listxattr_request(ino_t ino_nr, struct fsdriver_data *data, size_t size)
{
	struct inode *rip;
	ssize_t r;

	if ((rip = mfs4_xattr_get_inode(ino_nr)) == NULL)
		return -(err_code != OK ? err_code : EIO);
	r = mfs4_listxattr(rip, data, size);
	put_inode(rip);
	return r;
}

static int
mfs4_removexattr_request(ino_t ino_nr, const char *name)
{
	struct inode *rip;
	int r;

	if ((rip = mfs4_xattr_get_inode(ino_nr)) == NULL)
		return err_code != OK ? err_code : EIO;
	if (superblock.s_rd_only)
		r = EROFS;
	else
		r = mfs4_removexattr(rip, name);
	put_inode(rip);
	return r;
}

struct fsdriver mfs_table = {
	.fdr_mount	= fs_mount,
	.fdr_unmount_err = fs_unmount,
	.fdr_lookup	= fs_lookup,
	.fdr_putnode	= mfs4_putnode_request,
	.fdr_read	= fs_readwrite,
	.fdr_write	= mfs4_write_request,
	.fdr_peek	= fs_readwrite,
	.fdr_getdents	= fs_getdents,
	.fdr_trunc	= mfs4_trunc_request,
	.fdr_seek	= fs_seek,
	.fdr_create	= mfs4_create_request,
	.fdr_mkdir	= mfs4_mkdir_request,
	.fdr_mknod	= mfs4_mknod_request,
	.fdr_link	= mfs4_link_request,
	.fdr_unlink	= mfs4_unlink_request,
	.fdr_rmdir	= mfs4_unlink_request,
	.fdr_rename	= mfs4_rename_request,
	.fdr_slink	= mfs4_slink_request,
	.fdr_rdlink	= fs_rdlink,
	.fdr_stat	= fs_stat,
	.fdr_getxattr	= mfs4_getxattr_request,
	.fdr_setxattr	= mfs4_setxattr_request,
	.fdr_listxattr	= mfs4_listxattr_request,
	.fdr_removexattr = mfs4_removexattr_request,
	.fdr_chown	= mfs4_chown_request,
	.fdr_chmod	= mfs4_chmod_request,
	.fdr_utime	= mfs4_utime_request,
	.fdr_mountpt	= fs_mountpt,
	.fdr_statvfs	= fs_statvfs,
	.fdr_sync_err	= fs_sync,
	.fdr_driver	= lmfs_driver,
	.fdr_bread	= lmfs_bio,
	.fdr_bwrite	= lmfs_bio,
	.fdr_bpeek	= lmfs_bio,
	.fdr_bflush_err = lmfs_bflush_err
};
