#include "fs.h"
#include "inode.h"
#include "clean.h"

/*===========================================================================*
 *				fs_sync					     *
 *===========================================================================*/
int fs_sync(void)
{
/* Flush inode metadata first and the block cache last. Return cache write
 * errors to VFS so fsync() cannot report success after a failed write.
 */
  struct inode *rip;
  int r;

  /*
   * Also retry dirty, currently-unused cache entries. put_inode() writes
   * eagerly, but a failed legacy codec write must remain visible to sync.
   */
  for (rip = &inode[0]; rip < &inode[NR_INODES]; rip++) {
      if (rip->i_dev != fs_dev || rip->i_num == NO_ENTRY ||
          !IN_ISDIRTY(rip))
          continue;
      if ((r = rw_inode(rip, WRITING)) != OK)
          return(r);
  }

  return lmfs_flushall_err();
}
