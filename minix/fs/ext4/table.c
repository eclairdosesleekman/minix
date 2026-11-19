/* ext4 file-system driver table. */

#define _TABLE

#include "fs.h"
#include "inode.h"
#include "buf.h"
#include "super.h"

static ssize_t ext4_write(ino_t ino_nr, struct fsdriver_data *data,
    size_t bytes, off_t pos, int call)
{
	return fs_readwrite(ino_nr, data, bytes, pos, call);
}

static ssize_t ext4_bwrite(dev_t dev, struct fsdriver_data *data,
    size_t bytes, off_t pos, int call)
{
	(void)dev; (void)data; (void)bytes; (void)pos; (void)call;
	return EROFS;
}

static int ext4_trunc(ino_t ino_nr, off_t start, off_t end)
{
	(void)ino_nr; (void)start; (void)end;
	return EROFS;
}
static int ext4_create(ino_t dir_nr, char *name, mode_t mode, uid_t uid,
    gid_t gid, struct fsdriver_node *node)
{
	(void)dir_nr; (void)name; (void)mode; (void)uid; (void)gid; (void)node;
	return EROFS;
}
static int ext4_mkdir(ino_t dir_nr, char *name, mode_t mode, uid_t uid,
    gid_t gid)
{
	(void)dir_nr; (void)name; (void)mode; (void)uid; (void)gid;
	return EROFS;
}
static int ext4_mknod(ino_t dir_nr, char *name, mode_t mode, uid_t uid,
    gid_t gid, dev_t dev)
{
	(void)dir_nr; (void)name; (void)mode; (void)uid; (void)gid; (void)dev;
	return EROFS;
}
static int ext4_link(ino_t dir_nr, char *name, ino_t ino_nr)
{
	(void)dir_nr; (void)name; (void)ino_nr;
	return EROFS;
}
static int ext4_unlink(ino_t dir_nr, char *name, int call)
{
	(void)dir_nr; (void)name; (void)call;
	return EROFS;
}
static int ext4_rename(ino_t old_dir_nr, char *old_name, ino_t new_dir_nr,
    char *new_name)
{
	(void)old_dir_nr; (void)old_name; (void)new_dir_nr; (void)new_name;
	return EROFS;
}
static int ext4_slink(ino_t dir_nr, char *name, uid_t uid, gid_t gid,
    struct fsdriver_data *data, size_t bytes)
{
	(void)dir_nr; (void)name; (void)uid; (void)gid; (void)data; (void)bytes;
	return EROFS;
}
static int ext4_chown(ino_t ino_nr, uid_t uid, gid_t gid, mode_t *mode)
{
	(void)ino_nr; (void)uid; (void)gid; (void)mode;
	return EROFS;
}
static int ext4_chmod(ino_t ino_nr, mode_t *mode)
{
	(void)ino_nr; (void)mode;
	return EROFS;
}
static int ext4_utime(ino_t ino_nr, struct timespec *atime,
    struct timespec *mtime)
{
	(void)ino_nr; (void)atime; (void)mtime;
	return EROFS;
}
static void ext4_sync(void)
{
	/* fs_sync flushes data-only mounts and is a no-op for read-only mounts. */
	fs_sync();
}
static void ext4_bflush(dev_t dev)
{
	(void)dev;
	if (superblock != NULL && superblock->s_data_write_only)
		fs_sync();
}

struct fsdriver ext4_table = {
	.fdr_mount	= fs_mount,
	.fdr_unmount	= fs_unmount,
	.fdr_lookup	= fs_lookup,
	.fdr_putnode	= fs_putnode,
	.fdr_read	= fs_readwrite,
	.fdr_write	= ext4_write,
	.fdr_peek	= fs_readwrite,
	.fdr_getdents	= fs_getdents,
	.fdr_trunc	= ext4_trunc,
	.fdr_seek	= fs_seek,
	.fdr_create	= ext4_create,
	.fdr_mkdir	= ext4_mkdir,
	.fdr_mknod	= ext4_mknod,
	.fdr_link	= ext4_link,
	.fdr_unlink	= ext4_unlink,
	.fdr_rmdir	= ext4_unlink,
	.fdr_rename	= ext4_rename,
	.fdr_slink	= ext4_slink,
	.fdr_rdlink	= fs_rdlink,
	.fdr_stat	= fs_stat,
	.fdr_chown	= ext4_chown,
	.fdr_chmod	= ext4_chmod,
	.fdr_utime	= ext4_utime,
	.fdr_mountpt	= fs_mountpt,
	.fdr_statvfs	= fs_statvfs,
	.fdr_sync	= ext4_sync,
	.fdr_driver	= lmfs_driver,
	.fdr_bread	= lmfs_bio,
	.fdr_bwrite	= ext4_bwrite,
	.fdr_bpeek	= lmfs_bio,
	.fdr_bflush	= ext4_bflush
};
