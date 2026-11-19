/* Created (MFS based):
 *   February 2010 (Evgeniy Ivanov)
 */

#include "fs.h"
#include "buf.h"
#include "inode.h"
#include "super.h"
#include <stdlib.h>
#include <string.h>
#include <minix/vfsif.h>
#include <minix/bdev.h>


/*===========================================================================*
 *				fs_mount				     *
 *===========================================================================*/
int fs_mount(dev_t dev, unsigned int flags, struct fsdriver_node *root_node,
	unsigned int *res_flags)
{
	struct inode *root_ip;
	struct super_block *sp;
	u32_t unsupported;
	int writable;

	/* This server only supports data overwrites, not metadata updates. */
	writable = !(flags & REQ_RDONLY);
	fs_dev = dev;
	if (bdev_open(fs_dev, BDEV_R_BIT | (writable ? BDEV_W_BIT : 0)) != OK)
		return(EINVAL);

	if ((superblock = malloc(sizeof(*superblock))) == NULL)
		panic("Can't allocate memory for ext4 superblock.");
	memset(superblock, 0, sizeof(*superblock));
	sp = superblock;
	sp->s_dev = fs_dev;
	if (read_super(sp) != OK)
		goto bad_mount;
	/* Keep all filesystem metadata read-only, even for data-write mounts. */
	sp->s_rd_only = TRUE;
	sp->s_data_write_only = FALSE;

	if (sp->s_rev_level != EXT2_DYNAMIC_REV) {
		printf("ext4: dynamic-revision superblock required\\n");
		goto bad_mount;
	}
	if (!HAS_INCOMPAT_FEATURE(sp, INCOMPAT_EXTENTS) ||
	    !HAS_INCOMPAT_FEATURE(sp, INCOMPAT_FILETYPE)) {
		printf("ext4: extent and directory-filetype features are required\\n");
		goto bad_mount;
	}
	unsupported = sp->s_feature_incompat & ~SUPPORTED_INCOMPAT_FEATURES;
	if (unsupported != 0) {
		printf("ext4: unsupported incompatible features 0x%x\\n",
		    unsupported);
		goto bad_mount;
	}
	unsupported = sp->s_feature_ro_compat & ~SUPPORTED_RO_COMPAT_FEATURES;
	if (unsupported != 0) {
		printf("ext4: unsupported read-only-compatible features 0x%x\\n",
		    unsupported);
		goto bad_mount;
	}
	if (writable && HAS_COMPAT_FEATURE(sp, COMPAT_HAS_JOURNAL)) {
		printf("ext4: writable data-only mounts require no journal\\n");
		goto bad_mount;
	}
	if (sp->s_feature_compat & COMPAT_ORPHAN_FILE) {
		printf("ext4: orphan-file feature is not supported\\n");
		goto bad_mount;
	}
	if (sp->s_last_orphan != 0) {
		printf("ext4: orphan list is not empty; run fsck first\\n");
		goto bad_mount;
	}
	if (!(sp->s_state & EXT2_VALID_FS) ||
	    (sp->s_state & EXT2_ERROR_FS)) {
		printf("ext4: filesystem is not clean; run fsck first\\n");
		goto bad_mount;
	}

	lmfs_set_blocksize(sp->s_block_size);
	lmfs_set_blockusage(sp->s_blocks_count,
	    sp->s_blocks_count - sp->s_free_blocks_count);
	if ((root_ip = get_inode(fs_dev, ROOT_INODE)) == NULL) {
		printf("ext4: couldn't load root inode\\n");
		goto bad_mount;
	}
	if (root_ip->i_mode == 0 ||
	    (root_ip->i_mode & I_TYPE) != I_DIRECTORY) {
		printf("ext4: root inode is not a directory\\n");
		put_inode(root_ip);
		goto bad_mount;
	}
	root_node->fn_ino_nr = root_ip->i_num;
	root_node->fn_mode = root_ip->i_mode;
	root_node->fn_size = root_ip->i_size;
	root_node->fn_uid = root_ip->i_uid;
	root_node->fn_gid = root_ip->i_gid;
	root_node->fn_dev = NO_DEV;
	sp->s_data_write_only = writable;
	*res_flags = RES_64BIT;
	return(OK);

bad_mount:
	sp->s_dev = NO_DEV;
	bdev_close(fs_dev);
	return(EINVAL);
}

/*===========================================================================*
 *				fs_mountpt				     *
 *===========================================================================*/
int fs_mountpt(ino_t ino_nr)
{
/* This function looks up the mount point, it checks the condition whether
 * the partition can be mounted on the inode or not.
 */
  register struct inode *rip;
  int r = OK;
  mode_t bits;

  /* Temporarily open the file. */
  if( (rip = get_inode(fs_dev, ino_nr)) == NULL)
	  return(EINVAL);

  if(rip->i_mountpoint) r = EBUSY;

  /* It may not be special. */
  bits = rip->i_mode & I_TYPE;
  if (bits == I_BLOCK_SPECIAL || bits == I_CHAR_SPECIAL) r = ENOTDIR;

  put_inode(rip);

  if(r == OK) rip->i_mountpoint = TRUE;

  return(r);
}


/*===========================================================================*
 *				fs_unmount				     *
 *===========================================================================*/
void fs_unmount(void)
{
/* Unmount a file system by device number. */
  int count;
  struct inode *rip, *root_ip;

  /* See if the mounted device is busy.  Only 1 inode using it should be
   * open --the root inode-- and that inode only 1 time.  This is an integrity
   * check only: VFS expects the unmount to succeed either way.
   */
  count = 0;
  for (rip = &inode[0]; rip < &inode[NR_INODES]; rip++)
	  if (rip->i_count > 0 && rip->i_dev == fs_dev) count += rip->i_count;
  if (count != 1)
	printf("ext2: file system has %d in-use inodes!\n", count);

  if ((root_ip = find_inode(fs_dev, ROOT_INODE)) == NULL)
	panic("ext2: couldn't find root inode");

  /* Sync fs data before checking count. In some cases VFS can force unmounting
   * and it will damage unsynced FS. We don't sync before checking root_ip since
   * if it is missing then something strange happened with FS, so it's better
   * to not use possibly corrupted data for syncing.
   */
  if (!superblock->s_rd_only || superblock->s_data_write_only) {
	/* Flush data blocks before closing a data-write-only mount. */
	fs_sync();
  }

  put_inode(root_ip);

  if (!superblock->s_rd_only) {
	superblock->s_wtime = clock_time(NULL);
	superblock->s_state = EXT2_VALID_FS;
	write_super(superblock); /* Commit info, we just set above */
  }

  /* Close the device the file system lives on. */
  bdev_close(fs_dev);

  /* Throw all blocks out of the VM cache, to prevent corruption later. */
  lmfs_invalidate(fs_dev);

  /* Finish off the unmount. */
  superblock->s_dev = NO_DEV;
}
