#include "fs.h"
#include "inode.h"
#include "super.h"
#include <minix/vfsif.h>
#include <minix/bdev.h>

/*===========================================================================*
 *				fs_mount				     *
 *===========================================================================*/
int fs_mount(dev_t dev, unsigned int flags, struct fsdriver_node *root_node,
	unsigned int *res_flags)
{
/* This function reads the superblock of the partition, gets the root inode
 * and sends back the details of them.
 */
  struct inode *root_ip;
  int r, readonly;

  fs_dev = dev;
  readonly = (flags & REQ_RDONLY) ? 1 : 0;

  /* Open the device the file system lives on. */
  if (bdev_open(fs_dev, readonly ? BDEV_R_BIT : (BDEV_R_BIT|BDEV_W_BIT) ) !=
		OK) {
        return(EINVAL);
  }
  
  /* Fill in the super block. */
  superblock.s_dev = fs_dev;	/* read_super() needs to know which dev */
  r = read_super(&superblock);

  /* Is it recognized as a Minix filesystem? */
  if (r != OK) {
	superblock.s_dev = NO_DEV;
	bdev_close(fs_dev);
	return(r);
  }

  /* clean check: if rw and not clean, switch to readonly */
  if (superblock.s_version != V4 &&
      !(superblock.s_flags & MFSFLAG_CLEAN) && !readonly) {
	if(bdev_close(fs_dev) != OK)
		panic("couldn't bdev_close after found unclean FS");
	readonly = 1;

	if (bdev_open(fs_dev, BDEV_R_BIT) != OK) {
		panic("couldn't bdev_open after found unclean FS");
		return(EINVAL);
  	}
	printf("MFS: WARNING: FS 0x%llx unclean, mounting readonly\n", (unsigned long long)fs_dev);
  }

  if ((r = lmfs_set_blocksize_err(superblock.s_block_size)) != OK) {
	superblock.s_dev = NO_DEV;
	(void)bdev_close(fs_dev);
	return r;
  }

  if (superblock.s_version == V4 &&
      (r = mfs4_journal_mount(&superblock, readonly)) != OK) {
	printf("MFS: MFS4 journal mount refused (%d)\n", r);
	superblock.s_dev = NO_DEV;
	(void)bdev_close(fs_dev);
	return r;
  }

  /* Compute the current number of used zones, and report it to libminixfs.
   * Note that libminixfs really wants numbers of *blocks*, but this MFS
   * implementation dropped support for differing zone/block sizes a while ago.
   */
  used_zones = superblock.s_zones - count_free_bits(&superblock, ZMAP);

  lmfs_set_blockusage(superblock.s_zones, used_zones);
  
  /* Get the root inode of the mounted file system. */
  if( (root_ip = get_inode(fs_dev, ROOT_INODE)) == NULL)  {
	printf("MFS: couldn't get root inode\n");
	superblock.s_dev = NO_DEV;
	bdev_close(fs_dev);
	return(EINVAL);
  }
  
  if(root_ip->i_mode == 0) {
	printf("%s:%d zero mode for root inode?\n", __FILE__, __LINE__);
	put_inode(root_ip);
	superblock.s_dev = NO_DEV;
	bdev_close(fs_dev);
	return(EINVAL);
  }

  superblock.s_rd_only = readonly;
  
  /* Root inode properties */
  root_node->fn_ino_nr = root_ip->i_num;
  root_node->fn_mode = root_ip->i_mode;
  root_node->fn_size = root_ip->i_size;
  root_node->fn_uid = root_ip->i_uid;
  root_node->fn_gid = root_ip->i_gid;
  root_node->fn_dev = NO_DEV;

  *res_flags = (superblock.s_version == V4) ?
      (RES_HASPEEK | RES_64BIT) : RES_NOFLAGS;

  /* Mark it dirty */
  if (!superblock.s_rd_only && superblock.s_version == V4) {
	if ((r = mfs4_journal_unmount()) != OK) {
		if (get_inode(fs_dev, ROOT_INODE) == NULL)
			printf("MFS: unable to repin root inode after MFS4 journal error\\n");
		return r;
	}
  }

  if (!superblock.s_rd_only && superblock.s_version != V4) {
	unsigned int old_flags = superblock.s_flags;

	superblock.s_flags &= ~MFSFLAG_CLEAN;
	if ((r = write_super(&superblock)) != OK) {
		/* Keep failed metadata in LMFS for a possible retry; do not
		 * invalidate the device cache on this failed mount.
		 */
		put_inode(root_ip);
		superblock.s_dev = NO_DEV;
		(void)bdev_close(fs_dev);
		superblock.s_flags = old_flags;
		return r;
	}
  }

  return(r);
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
int fs_unmount(void)
{
/* Unmount a file system only after all metadata has reached the device.
 * Keep the root inode pinned if an error prevents VFS from completing unmount.
 */
  int count, r, close_error, restore_error;
  struct inode *rip, *root_ip;

  count = 0;
  for (rip = &inode[0]; rip < &inode[NR_INODES]; rip++)
	if (rip->i_count > 0 && rip->i_dev == fs_dev)
		count += rip->i_count;
  if (count != 1)
	printf("MFS: file system has %d in-use inodes!\n", count);

  if ((root_ip = find_inode(fs_dev, ROOT_INODE)) == NULL)
	return EIO;

  put_inode(root_ip);

  /* put_inode() may have written the last dirty inode into LMFS. */
  if ((r = fs_sync()) != OK) {
	if (get_inode(fs_dev, ROOT_INODE) == NULL)
		printf("MFS: unable to repin root inode after sync error\n");
	return r;
  }

  /* MFS4's clean bit is itself protected by the redo journal. */
  if (!superblock.s_rd_only && superblock.s_version == V4) {
	if ((r = mfs4_journal_unmount()) != OK) {
		if (get_inode(fs_dev, ROOT_INODE) == NULL)
			printf("MFS: unable to repin root inode after MFS4 journal error\n");
		return r;
	}
  }

  /* Mark it clean only after the preceding writeback has succeeded. */
  if (!superblock.s_rd_only && superblock.s_version != V4) {
	unsigned int old_flags = superblock.s_flags;

	superblock.s_flags |= MFSFLAG_CLEAN;
	if ((r = write_super(&superblock)) != OK) {
		/* Replace the failed clean image in LMFS with a dirty superblock
		 * image before returning, so a later retry cannot flush CLEAN.
		 */
		superblock.s_flags = old_flags;
		restore_error = write_super(&superblock);
		if (restore_error != OK) {
			/* Do not accept further mutations if the dirty marker could
			 * not be restored after a failed clean-marker write.
			 */
			superblock.s_rd_only = 1;
			printf("MFS: unable to restore dirty superblock (%d)\n",
			    restore_error);
		}
		if (get_inode(fs_dev, ROOT_INODE) == NULL)
			printf("MFS: unable to repin root inode after superblock error\n");
		return r;
	}
  }

  close_error = bdev_close(fs_dev);
  if (close_error != OK) {
	/* The unmount did not complete, so revoke the clean indication. */
	if (!superblock.s_rd_only && superblock.s_version == V4) {
		r = mfs4_journal_mark_dirty();
		if (r != OK)
			printf("MFS: unable to restore MFS4 dirty state (%d)\n", r);
	}
	if (!superblock.s_rd_only && superblock.s_version != V4) {
		superblock.s_flags &= ~MFSFLAG_CLEAN;
		r = write_super(&superblock);
		if (r != OK)
			printf("MFS: unable to restore dirty flag after close error (%d)\n",
			    r);
	}
	/* Keep the still-mounted filesystem read-only after a failed close. */
	superblock.s_rd_only = 1;
	if (get_inode(fs_dev, ROOT_INODE) == NULL)
		printf("MFS: unable to repin root inode after close error\n");
	return close_error;
  }

  /* Only invalidate cached blocks once all writeback and close operations
   * have completed successfully.
   */
  lmfs_invalidate(fs_dev);
  superblock.s_dev = NO_DEV;

  return OK;
}

