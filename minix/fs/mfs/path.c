/* This file contains the procedures that look up path names in the directory
 * system and determine the inode number that goes with a given path name.
 */
 
#include "fs.h"
#include <assert.h>
#include <string.h>
#include "buf.h"
#include "inode.h"
#include "super.h"

#include <minix/mfs4_disk.h>
#include "mfs4_journal.h"

static ino_t
mfs4_dirent_inode(const struct inode *dir, const struct direct *dp)
{
	if (dir->i_sp->s_version == V4)
		return (ino_t)mfs4_get_le32((const uint8_t *)dp +
		    MFS4_DIRENT_INODE);
	return (ino_t)conv4(dir->i_sp->s_native, (int)dp->mfs_d_ino);
}

static void
mfs4_set_dirent_inode(const struct inode *dir, struct direct *dp,
	ino_t inum)
{
	if (dir->i_sp->s_version == V4)
		mfs4_put_le32((uint8_t *)dp + MFS4_DIRENT_INODE,
		    (uint32_t)inum);
	else
		dp->mfs_d_ino = conv4(dir->i_sp->s_native, (int)inum);
}


/*===========================================================================*
 *                             fs_lookup				     *
 *===========================================================================*/
int fs_lookup(ino_t dir_nr, char *name, struct fsdriver_node *node,
	int *is_mountpt)
{
  struct inode *dirp, *rip;

  /* Find the starting inode. */
  if ((dirp = find_inode(fs_dev, dir_nr)) == NULL)
	return EINVAL;

  /* Look up the directory entry. */
  if ((rip = advance(dirp, name)) == NULL)
	return err_code;

  /* On success, leave the resulting inode open and return its details. */
  node->fn_ino_nr = rip->i_num;
  node->fn_mode = rip->i_mode;
  node->fn_size = rip->i_size;
  node->fn_uid = rip->i_uid;
  node->fn_gid = rip->i_gid;
  /* This is only valid for block and character specials. But it doesn't
   * cause any harm to always set the device field. */
  node->fn_dev = (dev_t) rip->i_zone[0];

  *is_mountpt = rip->i_mountpoint;

  return OK;
}


/*===========================================================================*
 *				advance					     *
 *===========================================================================*/
struct inode *advance(
  struct inode *dirp,		/* inode for directory to be searched */
  const char *string		/* component name to look for */
)
{
/* Given a directory and a component of a path, look up the component in
 * the directory, find the inode, open it, and return a pointer to its inode
 * slot.
 */
  ino_t numb;
  struct inode *rip;

  assert(dirp != NULL);

  /* If 'string' is empty, return an error. */
  if (string[0] == '\0') {
  	err_code = ENOENT;
	return(NULL);
  }

  /* If dir has been removed return ENOENT. */
  if (dirp->i_nlinks == NO_LINK) {
	err_code = ENOENT;
	return(NULL);
  }

  /* If 'string' is not present in the directory, signal error. */
  if ( (err_code = search_dir(dirp, string, &numb, LOOK_UP)) != OK) {
	return(NULL);
  }

  /* The component has been found in the directory.  Get inode. */
  if ( (rip = get_inode(dirp->i_dev, (int) numb)) == NULL)  {
	assert(err_code != OK);
	return(NULL);
  }

  assert(err_code == OK);
  return(rip);
}


/*===========================================================================*
 *				search_dir				     *
 *===========================================================================*/
int search_dir(
  register struct inode *ldir_ptr, /* ptr to inode for dir to search */
  const char *string,		 /* component to search for */
  ino_t *numb,			 /* pointer to inode number */
  int flag			 /* LOOK_UP, ENTER, DELETE or IS_EMPTY */
)
{
/* This function searches the directory whose inode is pointed to by 'ldip':
 * if (flag == ENTER)  enter 'string' in the directory with inode # '*numb';
 * if (flag == DELETE) delete 'string' from the directory;
 * if (flag == LOOK_UP) search for 'string' and return inode # in 'numb';
 * if (flag == IS_EMPTY) return OK if only . and .. in dir else ENOTEMPTY;
 *
 * This function, and this function alone, implements name truncation,
 * by simply considering only the first MFS_NAME_MAX bytes from 'string'.
 */
  register struct direct *dp = NULL;
  register struct direct *dp_end = NULL;
  register struct buf *bp = NULL;
  int i, r, e_hit, t, match;
  off_t pos;
  unsigned new_slots, old_slots;
  struct super_block *sp;
  int extended = 0;
  uint8_t *dir_image = NULL;
  const uint8_t *shadow_image;
  uint64_t disk_block;

  /* If 'ldir_ptr' is not a pointer to a dir inode, error. */
  if ( (ldir_ptr->i_mode & I_TYPE) != I_DIRECTORY)  {
	return(ENOTDIR);
   }

  if((flag == DELETE || flag == ENTER) && ldir_ptr->i_sp->s_rd_only)
	return EROFS;
  
  /* Step through the directory one block at a time. */
  old_slots = (unsigned) (ldir_ptr->i_size/DIR_ENTRY_SIZE);
  new_slots = 0;
  e_hit = FALSE;
  match = 0;			/* set when a string match occurs */

  pos = 0;
  if (flag == ENTER && ldir_ptr->i_last_dpos < ldir_ptr->i_size) {
	pos = ldir_ptr->i_last_dpos;
	new_slots = (unsigned) (pos/DIR_ENTRY_SIZE);
  }

  for (; pos < ldir_ptr->i_size; pos += ldir_ptr->i_sp->s_block_size) {
	assert(ldir_ptr->i_dev != NO_DEV);

	/* Since directories don't have holes, 'b' cannot be NO_BLOCK. */
	bp = get_block_map(ldir_ptr, pos);

	assert(ldir_ptr->i_dev != NO_DEV);
	assert(bp != NULL);

	/*
	 * MFS4 directory entries are journal metadata.  Use the active shadow
	 * image for both reads and writes so a callback sees its own earlier
	 * directory changes.  Mutating access without an outer transaction is
	 * rejected rather than falling back to a dirty home buffer.
	 */
	dir_image = NULL;
	if (ldir_ptr->i_sp->s_version == V4) {
		disk_block = read_map(ldir_ptr, pos, 0);
		if (disk_block == NO_BLOCK) {
			put_block(bp);
			return EIO;
		}
		r = mfs4_tx_find_meta(disk_block, &shadow_image);
		if (r == OK) {
			dir_image = (uint8_t *)(void *)shadow_image;
		} else if (r != ENOENT) {
			put_block(bp);
			return r;
		} else if (flag == ENTER || flag == DELETE) {
			if (!mfs4_tx_is_active()) {
				put_block(bp);
				return EINVAL;
			}
			if ((r = mfs4_tx_get_meta(disk_block,
			    MFS4_JMETA_DIRECTORY, &dir_image)) != OK) {
				put_block(bp);
				return r;
			}
		}
	}

	/* Search either the VFS buffer or the transaction's byte image. */
	if (dir_image != NULL) {
		dp = (struct direct *)(void *)dir_image;
		dp_end = dp + (MFS4_BLOCK_SIZE / MFS4_DIRENT_SIZE);
	} else {
		dp = &b_dir(bp)[0];
		dp_end = &b_dir(bp)[NR_DIR_ENTRIES(
		    ldir_ptr->i_sp->s_block_size)];
	}
	for (; dp < dp_end; dp++) {
		if (++new_slots > old_slots) { /* not found, but room left */
			if (flag == ENTER) e_hit = TRUE;
			break;
		}

		/* Match occurs if string found. */
		if (flag != ENTER && mfs4_dirent_inode(ldir_ptr, dp) != NO_ENTRY) {
			if (flag == IS_EMPTY) {
				/* If this test succeeds, dir is not empty. */
				if (strcmp(dp->mfs_d_name, "." ) != 0 &&
				    strcmp(dp->mfs_d_name, "..") != 0)
					match = 1;
			} else {
				if (strncmp(dp->mfs_d_name, string,
					sizeof(dp->mfs_d_name)) == 0){
					match = 1;
				}
			}
		}

		if (match) {
			/* LOOK_UP or DELETE found what it wanted. */
			r = OK;
			if (flag == IS_EMPTY) r = ENOTEMPTY;
			else if (flag == DELETE) {
				/* Save d_ino for recovery. */
				t = MFS_NAME_MAX - sizeof(ino_t);
				if (ldir_ptr->i_sp->s_version == V4) {
					mfs4_put_le32((uint8_t *)dp +
					    MFS4_DIRENT_NAME + t,
					    (uint32_t)mfs4_dirent_inode(ldir_ptr, dp));
				} else {
					*((ino_t *) &dp->mfs_d_name[t]) =
					    mfs4_dirent_inode(ldir_ptr, dp);
				}
				mfs4_set_dirent_inode(ldir_ptr, dp, NO_ENTRY);
				if (ldir_ptr->i_sp->s_version != V4)
					MARKDIRTY(bp);
				ldir_ptr->i_update |= CTIME | MTIME;
				IN_MARKDIRTY(ldir_ptr);
				if (pos < ldir_ptr->i_last_dpos)
					ldir_ptr->i_last_dpos = pos;
			} else {
				sp = ldir_ptr->i_sp;	/* 'flag' is LOOK_UP */
				*numb = (ino_t) conv4(sp->s_native,
						      (int) dp->mfs_d_ino);
			}
			put_block(bp);
			return(r);
		}

		/* Check for free slot for the benefit of ENTER. */
		if (flag == ENTER &&
		    mfs4_dirent_inode(ldir_ptr, dp) == 0) {
			e_hit = TRUE;	/* we found a free slot */
			break;
		}
	}

	/* The whole block has been searched or ENTER has a free slot. */
	if (e_hit) break;	/* e_hit set if ENTER can be performed now */
	put_block(bp);		/* otherwise, continue searching dir */
  }

  /* The whole directory has now been searched. */
  if (flag != ENTER) {
  	return(flag == IS_EMPTY ? OK : ENOENT);
  }

  /* When ENTER next time, start searching for free slot from
   * i_last_dpos. It gives some performance improvement (3-5%).
   */
  ldir_ptr->i_last_dpos = pos;

  /* This call is for ENTER.  If no free slot has been found so far, try to
   * extend directory.
   */
  if (e_hit == FALSE) { /* directory is full and no room left in last block */
	new_slots++;		/* increase directory size by 1 entry */
	if (new_slots == 0) return(EFBIG); /* dir size limited by slot count */
	if ( (bp = new_block(ldir_ptr, ldir_ptr->i_size)) == NULL)
		return(err_code);
	dp = &b_dir(bp)[0];
	extended = 1;
  }

  /* 'bp' now points to a directory block with space. 'dp' points to slot. */
  (void) memset(dp->mfs_d_name, 0, (size_t) MFS_NAME_MAX); /* clear entry */
  for (i = 0; i < MFS_NAME_MAX && string[i]; i++) dp->mfs_d_name[i] = string[i];
  sp = ldir_ptr->i_sp; 
  dp->mfs_d_ino = conv4(sp->s_native, (int) *numb);
  MARKDIRTY(bp);
  put_block(bp);
  ldir_ptr->i_update |= CTIME | MTIME;	/* mark mtime for update later */
  IN_MARKDIRTY(ldir_ptr);
  if (new_slots > old_slots) {
	ldir_ptr->i_size = (off_t) new_slots * DIR_ENTRY_SIZE;
	/* Send the change to disk if the directory is extended. */
	if (extended) rw_inode(ldir_ptr, WRITING);
  }
  return(OK);
}

