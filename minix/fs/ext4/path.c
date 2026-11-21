/* This file contains the procedures that look up path names in the directory
 * system and determine the inode number that goes with a given path name.
 *
 * Created (MFS based):
 *   February 2010 (Evgeniy Ivanov)
 */

#include "fs.h"
#include <assert.h>
#include <string.h>
#include <sys/param.h>
#include "buf.h"
#include "inode.h"
#include "super.h"


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
  node->fn_dev = (dev_t) rip->i_block[0];

  *is_mountpt = rip->i_mountpoint;

  return OK;
}


/*===========================================================================*
 *				advance														 *
 *				dirp: inode for directory to be searched					 *
 *				string: component name to look for							 *
 *===========================================================================*/
struct inode *advance(struct inode *dirp, const char *string)
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
  if (dirp->i_links_count == NO_LINK) {
	err_code = ENOENT;
	return(NULL);
  }

  /* If 'string' is not present in the directory, signal error. */
  if ( (err_code = search_dir(dirp, string, &numb, LOOK_UP, 0)) != OK) {
	return(NULL);
  }

  /* The component has been found in the directory.  Get inode. */
  if ( (rip = get_inode(dirp->i_dev, (int) numb)) == NULL)  {
	assert(err_code != OK);
	return(NULL);
  }

  return(rip);
}


/*
 * Search the two independent dirent areas of an ext4 inline directory.
 * "." and ".." are implicit: only the parent inode number is stored in
 * i_block[0..4]. Inline directory mutation is deliberately unsupported.
 */
static int
ext4_search_inline_dir(struct inode *dir, const char *string, ino_t *numb,
	int flag)
{
	const unsigned char *areas[2];
	unsigned int lengths[2], area, offset, rec_len, name_len;
	struct ext2_disk_dir_desc *dp;
	ino_t child;
	int empty;

	if (flag == ENTER || flag == DELETE)
		return(EOPNOTSUPP);
	if (dir->i_inline_size < EXT4_INLINE_DATA_MIN ||
	    dir->i_inline_size > EXT4_INLINE_DATA_MAX ||
	    dir->i_inline_size != EXT4_INLINE_DATA_MIN +
	    dir->i_inline_value_size ||
	    dir->i_inline_parent == 0 ||
	    dir->i_inline_parent > dir->i_sp->s_inodes_count)
		return(EIO);

	if (flag == LOOK_UP) {
		if (strcmp(string, ".") == 0) {
			*numb = dir->i_num;
			return(OK);
		}
		if (strcmp(string, "..") == 0) {
			*numb = (ino_t)dir->i_inline_parent;
			return(OK);
		}
	}

	areas[0] = dir->i_inline_data + sizeof(u32_t);
	lengths[0] = EXT4_INLINE_DATA_MIN - sizeof(u32_t);
	areas[1] = dir->i_inline_data + EXT4_INLINE_DATA_MIN;
	lengths[1] = dir->i_inline_value_size;
	empty = TRUE;

	for (area = 0; area < 2; area++) {
		if (lengths[area] == 0)
			continue;
		if (lengths[area] < MIN_DIR_ENTRY_SIZE ||
		    (lengths[area] & (DIR_ENTRY_ALIGN - 1)) != 0)
			return(EIO);
		for (offset = 0; offset < lengths[area]; offset += rec_len) {
			dp = (struct ext2_disk_dir_desc *)(areas[area] + offset);
			if (!ext4_valid_dirent(dp, offset, lengths[area]))
				return(EIO);
			rec_len = ext4_dir_rec_len(dp, lengths[area]);
			if (rec_len > lengths[area] - offset)
				return(EIO);
			if (dp->d_ino == NO_ENTRY)
				continue;

			name_len = dp->d_name_len;
			if (name_len > EXT2_NAME_MAX || name_len > NAME_MAX)
				return(EIO);
			if ((name_len == 1 && dp->d_name[0] == '.') ||
			    (name_len == 2 && dp->d_name[0] == '.' &&
			    dp->d_name[1] == '.'))
				return(EIO);

			child = (ino_t)conv4(le_CPU, dp->d_ino);
			if (child == NO_ENTRY ||
			    child > dir->i_sp->s_inodes_count)
				return(EIO);

			empty = FALSE;
			if (flag == LOOK_UP &&
			    ansi_strcmp(dp->d_name, string, name_len) == 0) {
				*numb = child;
				return(OK);
			}
			if (flag == IS_EMPTY)
				return(ENOTEMPTY);
		}
	}

	return(flag == IS_EMPTY ? OK : ENOENT);
}

/*===========================================================================*
 *				search_dir				     *
 *				ldir_ptr: ptr to inode for dir to search
 *				string: component to search for
 *				numb: pointer to inode number
 *				flag: LOOK_UP, ENTER, DELETE or IS_EMPTY
 *				ftype: used when ENTER and INCOMPAT_FILETYPE
 *===========================================================================*/
int search_dir(register struct inode *ldir_ptr, const char *string,
			   ino_t *numb, int flag, int ftype)
{
/* This function searches the directory whose inode is pointed to by 'ldip':
 * if (flag == ENTER)  enter 'string' in the directory with inode # '*numb';
 * if (flag == DELETE) delete 'string' from the directory;
 * if (flag == LOOK_UP) search for 'string' and return inode # in 'numb';
 * if (flag == IS_EMPTY) return OK if only . and .. in dir else ENOTEMPTY;
 */
  register struct ext2_disk_dir_desc  *dp = NULL;
  register struct ext2_disk_dir_desc  *prev_dp = NULL;
  register struct buf *bp = NULL;
  int i, r, e_hit, t, match;
  off_t pos;
  unsigned new_slots;
  int extended = 0;
  int required_space = 0;
  int string_len = 0;
  unsigned int block_size, block_limit, block_off, rec_len, entry_rec_len;

  /* If 'ldir_ptr' is not a pointer to a dir inode, error. */
  if ( (ldir_ptr->i_mode & I_TYPE) != I_DIRECTORY)  {
	return(ENOTDIR);
  }

  block_size = ldir_ptr->i_sp->s_block_size;
  new_slots = 0;
  e_hit = FALSE;
  match = 0;    	/* set when a string match occurs */
  pos = 0;

  if ((string_len = strlen(string)) > EXT2_NAME_MAX)
	return(ENAMETOOLONG);

  if (ldir_ptr->i_sp->s_is_ext4 &&
      HAS_INCOMPAT_FEATURE(ldir_ptr->i_sp, INCOMPAT_INLINE_DATA) &&
      (ldir_ptr->i_flags & EXT4_INLINE_DATA_FL))
	return(ext4_search_inline_dir(ldir_ptr, string, numb, flag));

  if (flag == ENTER) {
	required_space = MIN_DIR_ENTRY_SIZE + string_len;
	required_space += (required_space & 0x03) == 0 ? 0 :
			     (DIR_ENTRY_ALIGN - (required_space & 0x03) );

	if (ldir_ptr->i_last_dpos < ldir_ptr->i_size &&
	    ldir_ptr->i_last_dentry_size <= required_space)
		pos = ldir_ptr->i_last_dpos;
  }

  for (; pos < ldir_ptr->i_size; pos += block_size) {
	/* Since directories don't have holes, 'b' cannot be NO_BLOCK. */
	if(!(bp = get_block_map(ldir_ptr, rounddown(pos, block_size))))
		return(err_code != OK ? err_code : EIO);

	if (!ext4_verify_dir_block(ldir_ptr,
	    (const unsigned char *)b_data(bp),
	    (unsigned int)(pos / block_size))) {
		put_block(bp);
		return(EIO);
	}

	block_limit = block_size;
	if (HAS_RO_COMPAT_FEATURE(ldir_ptr->i_sp,
	    RO_COMPAT_METADATA_CSUM) &&
	    !(ldir_ptr->i_flags & EXT2_INDEX_FL))
		block_limit -= 12; /* ext4 directory checksum tail */

	prev_dp = NULL; /* New block - new first dentry, so no prev. */

	/*
	 * Search the block using the decoded record length. Never use the
	 * on-disk 16-bit value for pointer arithmetic: 0xffff represents a
	 * complete 64 KiB directory block.
	 */
	for (block_off = 0, dp = (struct ext2_disk_dir_desc*) &b_data(bp);
	     block_off < block_limit;) {
		/* Validate the record before touching its name or advancing. */
		if (!ext4_valid_dirent(dp, block_off, block_size)) {
			put_block(bp);
			return(EIO);
		}

		rec_len = ext4_dir_rec_len(dp, block_size);
		if (rec_len > block_limit - block_off) {
			put_block(bp);
			return(EIO);
		}

		/* Match occurs if string found. */
		if (flag != ENTER && dp->d_ino != NO_ENTRY) {
			if (flag == IS_EMPTY) {
				/* If this test succeeds, dir is not empty. */
				if (ansi_strcmp(dp->d_name, ".", dp->d_name_len) != 0 &&
				    ansi_strcmp(dp->d_name, "..", dp->d_name_len) != 0) match = 1;
			} else {
				if (ansi_strcmp(dp->d_name, string, dp->d_name_len) == 0){
					match = 1;
				}
			}
		}

		if (match) {
			/* LOOK_UP or DELETE found what it wanted. */
			r = OK;
			if (flag == IS_EMPTY) r = ENOTEMPTY;
			else if (flag == DELETE) {
				if (dp->d_name_len >= sizeof(ino_t)) {
					/* Save d_ino for recovery. */
					t = dp->d_name_len - sizeof(ino_t);
					memcpy(&dp->d_name[t], &dp->d_ino, sizeof(dp->d_ino));
				}
				dp->d_ino = NO_ENTRY;	/* erase entry */

				/* If we don't support HTree (directory index),
				 * which is fully compatible ext2 feature,
				 * we should reset EXT2_INDEX_FL, when modify
				 * linked directory structure.
				 *
				 * @TODO: actually we could just reset it for
				 * each directory, but I added if() to not
				 * forget about it later, when add HTree
				 * support.
				 */
				if (!HAS_COMPAT_FEATURE(ldir_ptr->i_sp,
							COMPAT_DIR_INDEX))
					ldir_ptr->i_flags &= ~EXT2_INDEX_FL;
				if (pos < ldir_ptr->i_last_dpos) {
					ldir_ptr->i_last_dpos = pos;
					ldir_ptr->i_last_dentry_size = rec_len;
				}
				ldir_ptr->i_update |= CTIME | MTIME;
				ldir_ptr->i_dirt = IN_DIRTY;
				/* Now we have cleared dentry, if it's not
				 * the first one, merge it with previous one.
				 * Since we assume, that existing dentry must be
				 * correct, there is no way to spann a data block.
				 */
				if (prev_dp) {
					unsigned int temp = ext4_dir_rec_len(prev_dp, block_size);
					temp += rec_len;
					ext4_set_dir_rec_len(prev_dp, temp, block_size);
				}
				r = ext4_update_dir_block_csum(ldir_ptr,
				    (unsigned char *)b_data(bp));
				if (r != OK) {
					put_block(bp);
					return(r);
				}
				lmfs_markdirty(bp);
			} else {
				/* 'flag' is LOOK_UP */
				*numb = (ino_t) conv4(le_CPU, dp->d_ino);
				if (*numb == NO_ENTRY ||
				    *numb > ldir_ptr->i_sp->s_inodes_count) {
					put_block(bp);
					return(EIO);
				}
			}
			put_block(bp);
			return(r);
		}

		/* Check for free slot for the benefit of ENTER. */
		if (flag == ENTER && dp->d_ino == NO_ENTRY) {
			/* we found a free slot, check if it has enough space */
			if (required_space <= rec_len) {
				e_hit = TRUE;	/* we found a free slot */
				break;
			}
		}
		/* Can we shrink dentry? */
		if (flag == ENTER &&
		    required_space <= DIR_ENTRY_SHRINK(dp, block_size)) {
			/* Split the record and use its tail as a free slot. */
			unsigned int new_slot_size = rec_len;
			unsigned int actual_size = DIR_ENTRY_ACTUAL_SIZE(dp);

			new_slot_size -= actual_size;
			ext4_set_dir_rec_len(dp, actual_size, block_size);
			dp = (struct ext2_disk_dir_desc *)
			    ((char *)dp + actual_size);
			ext4_set_dir_rec_len(dp, new_slot_size, block_size);
			/* if we fail before writing real ino */
			dp->d_ino = NO_ENTRY;
			e_hit = TRUE;	/* we found a free slot */
			break;
		}

		prev_dp = dp;

		/* In an indexed ext4 directory, the dx root info follows ".."
		 * in logical block zero. It is index metadata, not a dirent.
		 * Leaf blocks are still scanned linearly by this read-only server.
		 */
		if (flag == LOOK_UP && pos == 0 &&
		    (ldir_ptr->i_flags & EXT2_INDEX_FL) &&
		    ansi_strcmp(dp->d_name, "..", dp->d_name_len) == 0)
			break;

		block_off += rec_len;
		dp = (struct ext2_disk_dir_desc *)((char *)dp + rec_len);
	}

	/* The whole block has been searched or ENTER has a free slot. */
	if (e_hit) break;	/* e_hit set if ENTER can be performed now */
	put_block(bp);		 /* otherwise, continue searching dir */
  }

  /* The whole directory has now been searched. */
  if (flag != ENTER) {
	return(flag == IS_EMPTY ? OK : ENOENT);
  }

  /* When ENTER next time, start searching for free slot from
   * i_last_dpos. It gives solid performance improvement.
   */
  ldir_ptr->i_last_dpos = pos;
  ldir_ptr->i_last_dentry_size = required_space;

  /* This call is for ENTER.  If no free slot has been found so far, try to
   * extend directory.
   */
  if (e_hit == FALSE) { /* directory is full and no room left in last block */
	new_slots++;		/* increase directory size by 1 entry */
	if ( (bp = new_block(ldir_ptr, ldir_ptr->i_size)) == NULL)
		return(err_code);
	dp = (struct ext2_disk_dir_desc*) &b_data(bp);
	if (HAS_RO_COMPAT_FEATURE(ldir_ptr->i_sp,
	    RO_COMPAT_METADATA_CSUM)) {
		unsigned char *tail;

		/* ext4 leaf blocks reserve the final 12 bytes for the checksum. */
		tail = (unsigned char *)b_data(bp) + block_size - 12;
		memset(tail, 0, 12);
		ext4_set_dir_rec_len((struct ext2_disk_dir_desc *)tail, 12,
		    block_size);
		tail[7] = 0xde;
		ext4_set_dir_rec_len(dp, block_size - 12, block_size);
	} else {
		ext4_set_dir_rec_len(dp, block_size, block_size);
	}
	dp->d_name_len = DIR_ENTRY_MAX_NAME_LEN(dp, block_size); /* for failure */
	extended = 1;
  }

  /* 'bp' now points to a directory block with space. 'dp' points to slot. */
  dp->d_name_len = string_len;
  for (i = 0; i < NAME_MAX && i < dp->d_name_len && string[i]; i++)
	dp->d_name[i] = string[i];
  dp->d_ino = (int) conv4(le_CPU, *numb);
  if (HAS_INCOMPAT_FEATURE(ldir_ptr->i_sp, INCOMPAT_FILETYPE)) {
	/* Convert ftype (from inode.i_mode) to dp->d_file_type */
	if (ftype == I_REGULAR)
		dp->d_file_type = EXT2_FT_REG_FILE;
	else if (ftype == I_DIRECTORY)
		dp->d_file_type = EXT2_FT_DIR;
	else if (ftype == I_SYMBOLIC_LINK)
		dp->d_file_type = EXT2_FT_SYMLINK;
	else if (ftype == I_BLOCK_SPECIAL)
		dp->d_file_type = EXT2_FT_BLKDEV;
	else if (ftype == I_CHAR_SPECIAL)
		dp->d_file_type = EXT2_FT_CHRDEV;
	else if (ftype == I_NAMED_PIPE)
		dp->d_file_type = EXT2_FT_FIFO;
	else
		dp->d_file_type = EXT2_FT_UNKNOWN;
  }
  entry_rec_len = ext4_dir_rec_len(dp, block_size);
  r = ext4_update_dir_block_csum(ldir_ptr,
      (unsigned char *)b_data(bp));
  if (r != OK) {
	put_block(bp);
	return(r);
  }
  lmfs_markdirty(bp);
  put_block(bp);
  ldir_ptr->i_update |= CTIME | MTIME;	/* mark mtime for update later */
  ldir_ptr->i_dirt = IN_DIRTY;

  if (new_slots == 1) {
	ldir_ptr->i_size += (off_t)(extended ? block_size : entry_rec_len);
	/* Send the change to disk if the directory is extended. */
	if (extended) rw_inode(ldir_ptr, WRITING);
  }
  return(OK);

}
