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

  for (rip = &inode[0]; rip < &inode[NR_INODES]; rip++) {
	if (rip->i_count > 0 && IN_ISDIRTY(rip))
		rw_inode(rip, WRITING);
  }

  return lmfs_flushall_err();
}
