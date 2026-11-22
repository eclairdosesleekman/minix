/* Created (MFS based):
 *   February 2010 (Evgeniy Ivanov)
 */

#include "fs.h"
#include "inode.h"
#include "super.h"


/*===========================================================================*
 *				fs_chmod				     *
 *===========================================================================*/
int fs_chmod(ino_t ino_nr, mode_t *mode)
{
/* Perform the chmod(name, mode) system call. */
  register struct inode *rip;

  /* Temporarily open the file. */
  if( (rip = get_inode(fs_dev, ino_nr)) == NULL)
	  return(EINVAL);

  /* Now make the change. Clear setgid bit if file is not in caller's grp */
  rip->i_mode = (rip->i_mode & ~ALL_MODES) | (*mode & ALL_MODES);
  rip->i_update |= CTIME;
  rip->i_dirt = IN_DIRTY;

  /* Return full new mode to caller. */
  *mode = rip->i_mode;

  put_inode(rip);
  return(OK);
}


/*===========================================================================*
 *				fs_chown				     *
 *===========================================================================*/
int fs_chown(ino_t ino_nr, uid_t uid, gid_t gid, mode_t *mode)
{
  register struct inode *rip;

  /* Temporarily open the file. */
  if( (rip = get_inode(fs_dev, ino_nr)) == NULL)
	  return(EINVAL);

  if (rip->i_sp->s_is_ext4 &&
      rip->i_sp->s_creator_os == EXT4_OS_LINUX) {
	rip->i_uid = (u32_t)uid;
	rip->i_gid = (u32_t)gid;
  } else {
	/* Keep the ext2/non-Linux ext4 on-disk 16-bit ID format. */
	rip->i_uid = (u16_t)uid;
	rip->i_gid = (u16_t)gid;
  }
  rip->i_mode &= ~(I_SET_UID_BIT | I_SET_GID_BIT);
  rip->i_update |= CTIME;
  rip->i_dirt = IN_DIRTY;

  /* Update caller on current mode, as it may have changed. */
  *mode = rip->i_mode;
  put_inode(rip);

  return(OK);
}
