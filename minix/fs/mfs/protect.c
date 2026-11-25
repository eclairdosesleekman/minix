#include "fs.h"
#include "inode.h"
#include "super.h"
#include "mfs4_acl.h"


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
 
  if(rip->i_sp->s_rd_only) {
  	put_inode(rip);
	return EROFS;
  }

  /* Update the access ACL in the same transaction as mode bits. */
  {
	  mode_t old_mode = rip->i_mode;
	  rip->i_mode = (rip->i_mode & ~ALL_MODES) | (*mode & ALL_MODES);
	  rip->i_update |= CTIME;
	  IN_MARKDIRTY(rip);
	  if (rip->i_sp->s_version == V4) {
		  int r = mfs4_acl_chmod(rip, *mode);
		  if (r == OK)
			  r = rw_inode(rip, WRITING);
		  if (r != OK) {
			  rip->i_mode = old_mode;
			  put_inode(rip);
			  return r;
		  }
	  }
  }

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

  /* MFS3 cannot represent IDs wider than its 16-bit disk fields. */
  if (rip->i_sp->s_version == V3 &&
      (uid > 0xffffU || gid > 0xffffU)) {
      put_inode(rip);
      return(EOVERFLOW);
  }

  rip->i_uid = uid;
  rip->i_gid = gid;
  rip->i_mode &= ~(I_SET_UID_BIT | I_SET_GID_BIT);
  rip->i_update |= CTIME;
  IN_MARKDIRTY(rip);
  if (rip->i_sp->s_version == V4) {
	  int r = rw_inode(rip, WRITING);
	  if (r != OK) { put_inode(rip); return r; }
  }

  /* Update caller on current mode, as it may have changed. */
  *mode = rip->i_mode;
  put_inode(rip);
  
  return(OK);
}
