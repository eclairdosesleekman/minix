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

  /* Temporarily open the file. */
  if( (rip = get_inode(fs_dev, ino_nr)) == NULL)
        return(EINVAL);

  /*
   * MFS3's historical in-memory timestamp fields were u32_t. Accept only
   * values representable by that raw unsigned 32-bit field; do not wrap
   * negative or larger seconds during the legacy conversion.
   * UTIME_NOW/UTIME_OMIT are commands in tv_nsec, not timestamps.
   */
  if (rip->i_sp->s_version == V3 &&
      ((atime->tv_nsec != UTIME_NOW && atime->tv_nsec != UTIME_OMIT &&
        (atime->tv_sec < 0 || atime->tv_sec > 0xffffffffLL)) ||
       (mtime->tv_nsec != UTIME_NOW && mtime->tv_nsec != UTIME_OMIT &&
        (mtime->tv_sec < 0 || mtime->tv_sec > 0xffffffffLL)))) {
      put_inode(rip);
      return(EOVERFLOW);
  }

  rip->i_update = CTIME; /* discard any stale ATIME and MTIME flags */

  switch (atime->tv_nsec) {
  case UTIME_NOW:
	rip->i_update |= ATIME;
	break;
  case UTIME_OMIT: /* do not touch */
	break;
  default:
	/* MFS does not support subsecond resolution, so we round down. */
	rip->i_atime = atime->tv_sec;
	break;
  }

  switch (mtime->tv_nsec) {
  case UTIME_NOW:
	rip->i_update |= MTIME;
	break;
  case UTIME_OMIT: /* do not touch */
	break;
  default:
	/* MFS does not support subsecond resolution, so we round down. */
	rip->i_mtime = mtime->tv_sec;
	break;
  }

  IN_MARKDIRTY(rip);

  put_inode(rip);
  return(OK);
}

