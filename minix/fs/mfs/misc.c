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
      if (superblock.s_version == V4 && superblock.s_rd_only) {
          /* Never persist partially-mutated memory after a failed tx. */
          rip->i_update = 0;
          IN_MARKCLEAN(rip);
          continue;
      }
      if ((r = rw_inode(rip, WRITING)) != OK)
          return(r);
  }

  if (superblock.s_version == V4)
      return mfs4_journal_sync();

  return lmfs_flushall_err();
}
