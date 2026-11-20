/* Created (MFS based):
 *   February 2010 (Evgeniy Ivanov)
 */

#include "fs.h"
#include "inode.h"
#include <sys/time.h>
#include <sys/stat.h>


/*===========================================================================*
 *				fs_utime				     *
 *===========================================================================*/
int fs_utime(ino_t ino_nr, struct timespec *atime, struct timespec *mtime)
{
  register struct inode *rip;

  /* Validate explicit nanosecond values before changing inode state. */
  if ((atime->tv_nsec != UTIME_NOW && atime->tv_nsec != UTIME_OMIT &&
      (atime->tv_nsec < 0 || atime->tv_nsec >= 1000000000L)) ||
      (mtime->tv_nsec != UTIME_NOW && mtime->tv_nsec != UTIME_OMIT &&
      (mtime->tv_nsec < 0 || mtime->tv_nsec >= 1000000000L)))
	return(EINVAL);

  /* Temporarily open the file. */
  if( (rip = get_inode(fs_dev, ino_nr)) == NULL)
        return(EINVAL);

  /*
   * Inode timestamp seconds are signed 32-bit without the matching ext4
   * *_extra field. Refuse unrepresentable dates before changing the inode.
   */
  if (rip->i_sp->s_is_ext4 &&
      atime->tv_nsec != UTIME_NOW && atime->tv_nsec != UTIME_OMIT &&
      (atime->tv_sec < (time_t)-2147483648LL ||
      atime->tv_sec > (time_t)2147483647LL) &&
      rip->i_ext4_extra_isize < 16) {
	put_inode(rip);
	return(EOVERFLOW);
  }
  if (rip->i_sp->s_is_ext4 &&
      mtime->tv_nsec != UTIME_NOW && mtime->tv_nsec != UTIME_OMIT &&
      (mtime->tv_sec < (time_t)-2147483648LL ||
      mtime->tv_sec > (time_t)2147483647LL) &&
      rip->i_ext4_extra_isize < 12) {
	put_inode(rip);
	return(EOVERFLOW);
  }

  rip->i_update = CTIME;	/* discard any stale ATIME and MTIME flags */

  switch (atime->tv_nsec) {
  case UTIME_NOW:
	rip->i_update |= ATIME;
	break;
  case UTIME_OMIT: /* do not touch */
	break;
  default:
	rip->i_atime = atime->tv_sec;
	rip->i_atime_nsec = (rip->i_sp->s_is_ext4 &&
	    rip->i_ext4_extra_isize >= 16) ? atime->tv_nsec : 0;
	break;
  }

  switch (mtime->tv_nsec) {
  case UTIME_NOW:
	rip->i_update |= MTIME;
	break;
  case UTIME_OMIT: /* do not touch */
	break;
  default:
	rip->i_mtime = mtime->tv_sec;
	rip->i_mtime_nsec = (rip->i_sp->s_is_ext4 &&
	    rip->i_ext4_extra_isize >= 12) ? mtime->tv_nsec : 0;
	break;
  }

  rip->i_dirt = IN_DIRTY;

  put_inode(rip);
  return(OK);
}
