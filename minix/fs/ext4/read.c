/* Created (MFS based):
 *   February 2010 (Evgeniy Ivanov)
 */

#include "fs.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "buf.h"
#include "inode.h"
#include "super.h"
#include <sys/param.h>
#include <sys/dirent.h>
#include <assert.h>


static struct buf *rahead(struct inode *rip, block64_t baseblock, u64_t
	position, unsigned bytes_ahead);
static int rw_chunk(struct inode *rip, u64_t position, unsigned off,
	size_t chunk, unsigned left, int call, struct fsdriver_data *data,
	unsigned buf_off, unsigned int block_size, int *completed);

/*===========================================================================*
 *				fs_readwrite				     *
 *===========================================================================*/
ssize_t fs_readwrite(ino_t ino_nr, struct fsdriver_data *data, size_t nrbytes,
	off_t position, int call)
{
  int r;
  int regular;
  off_t f_size, bytes_left;
  size_t off, cum_io, block_size, chunk;
  mode_t mode_word;
  int completed;
  struct inode *rip;

  r = OK;

  /* The mount mode is enforced here as well as by VFS. */
  if (call == FSC_WRITE && superblock->s_rd_only)
	return(EROFS);

  /* Find the inode referred */
  if ((rip = find_inode(fs_dev, ino_nr)) == NULL)
	return(EINVAL);

  mode_word = rip->i_mode & I_TYPE;
  regular = (mode_word == I_REGULAR);

  /* Determine blocksize */
  block_size = rip->i_sp->s_block_size;
  f_size = rip->i_size;
  if (f_size < 0) f_size = MAX_FILE_POS;

  if (call == FSC_WRITE) {
	/* Check in advance to see if file will grow too big. */
	if (position > (off_t) (rip->i_sp->s_max_size - nrbytes))
		return(EFBIG);
  }

  cum_io = 0;
  /* Split the transfer into chunks that don't span two blocks. */
  while (nrbytes != 0) {
	off = (unsigned int) (position % block_size);/* offset in blk*/
	chunk = block_size - off;
	if (chunk > nrbytes)
		chunk = nrbytes;

	if (call == FSC_READ) {
		bytes_left = f_size - position;
		if (position >= f_size) break;        /* we are beyond EOF */
		if (chunk > bytes_left) chunk = (int) bytes_left;
	}

	/* Read or write 'chunk' bytes. */
	r = rw_chunk(rip, (u64_t)position, off, chunk,
		nrbytes, call, data, cum_io, block_size, &completed);

	if (r != OK) break;

	/* Update counters and pointers. */
	nrbytes -= chunk;     /* bytes yet to be read */
	cum_io += chunk;      /* bytes read so far */
	position += (off_t) chunk;    /* position within the file */
  }

  /* On write, update file size and access time. */
  if (call == FSC_WRITE) {
	if (regular || mode_word == I_DIRECTORY) {
		if (position > f_size) rip->i_size = position;
        }
  }

  rip->i_seek = NO_SEEK;

  if (r != OK)
	return r;

  if (call == FSC_READ) rip->i_update |= ATIME;
  if (call == FSC_WRITE) rip->i_update |= CTIME | MTIME;
  if (!rip->i_sp->s_rd_only)
	rip->i_dirt = IN_DIRTY;          /* inode is thus now dirty */

  return(cum_io);
}


/*===========================================================================*
 *				rw_chunk													 *
 *				rip: pointer to inode for file to be rd/wr					 *
 *				position: position within file to read or write				 *
 *				off: off within the current block							 *
 *				chunk: number of bytes to read or write						 *
 *				left: max number of bytes wanted after position				 *
 *				call: FSC_READ, FSC_WRITE, or FSC_PEEK						 *
 *				data: structure for (remote) user buffer					 *
 *				buf_off: offset in user buffer								 *
 *				block_size: block size of FS operating on					 *
 *				completed: number of bytes copied							 *
 *===========================================================================*/
static int rw_chunk(register struct inode *rip, u64_t position, unsigned off,
					size_t chunk, unsigned left, int call,
					struct fsdriver_data *data, unsigned buf_off,
					unsigned int block_size, int *completed)
{
/* Read or write (part of) a block. */

  struct buf *bp = NULL;
  register int r = OK;
  int n;
  block64_t b;
  dev_t dev;
  ino_t ino = VMC_NO_INODE;
  u64_t ino_off = rounddown(position, block_size);

  *completed = 0;

  err_code = OK;
  b = read_map(rip, (off_t)position, 0);
  if (err_code != OK)
	return(err_code);
  dev = rip->i_dev;
  ino = rip->i_num;
  assert(ino != VMC_NO_INODE);

  if (b == NO_BLOCK) {
	if (call == FSC_READ) {
		/* Reading from a nonexistent block.  Must read as all zeros.*/
		r = fsdriver_zero(data, buf_off, chunk);
		if(r != OK) {
			printf("ext2fs: fsdriver_zero failed\n");
		}
		return r;
	} else if (call == FSC_PEEK) {
		/* Peeking a nonexistent block. Report to VM. */
		lmfs_zero_block_ino(dev, ino, ino_off);
		return OK;
	} else {
               /* Writing to a nonexistent block.
                * Create and enter in inode.
                */
		if ((bp = new_block(rip, (off_t) ex64lo(position))) == NULL)
			return(err_code);
        }
  } else if (call != FSC_WRITE) {
	/* Extents are not necessarily physically contiguous. */
	if (rip->i_sp->s_is_ext4) {
		if ((r = lmfs_get_block_ino(&bp, dev, b, NORMAL, ino,
		    ino_off)) != OK)
			return(r);
	} else {
		bp = rahead(rip, b, position, left);
	}
  } else {
	/* Normally an existing block to be partially overwritten is first read
	 * in.  However, a full block need not be read in.  If it is already in
	 * the cache, acquire it, otherwise just acquire a free buffer.
         */
	n = (chunk == block_size ? NO_READ : NORMAL);
	if (off == 0 && (off_t) ex64lo(position) >= rip->i_size)
		n = NO_READ;
	assert(ino != VMC_NO_INODE);
	assert(!(ino_off % block_size));
	if ((r = lmfs_get_block_ino(&bp, dev, b, n, ino, ino_off)) != OK)
		panic("ext2: error getting block (%llu,%llu): %d", (unsigned long long)dev, (unsigned long long)b, r);
  }

  /* In all cases, bp now points to a valid buffer. */
  if (bp == NULL)
	panic("bp not valid in rw_chunk, this can't happen");

  if (call == FSC_WRITE && chunk != block_size &&
      (off_t) ex64lo(position) >= rip->i_size && off == 0) {
	zero_block(bp);
  }

  if (call == FSC_READ) {
	/* Copy a chunk from the block buffer to user space. */
	r = fsdriver_copyout(data, buf_off, b_data(bp)+off, chunk);
  } else if (call == FSC_WRITE) {
	/* Copy a chunk from user space to the block buffer. */
	r = fsdriver_copyin(data, buf_off, b_data(bp)+off, chunk);
	lmfs_markdirty(bp);
  }

  put_block(bp);

  return(r);
}


/*===========================================================================*
 *				read_map													 *
 *				rip: prt to inode to map from								 *
 *				position: position in file whose blk wanted					 *
 *===========================================================================*/
/*
 * ext4 extent records are packed, little-endian on-disk structures. Read
 * them with memcpy to avoid alignment assumptions.
 */
static block64_t ext4_extent_map(struct inode *rip, u32_t logical)
{
	const unsigned char *node = (const unsigned char *)rip->i_block;
	size_t node_size = sizeof(rip->i_block);
	struct buf *bp = NULL;
	block64_t visited[EXT4_EXTENT_MAX_DEPTH];
	unsigned int visited_count = 0;
	int expected_depth = -1;

	for (;;) {
		u16_t magic, entries, max_entries, depth;
		unsigned int i;
		const unsigned char *entry;

		if (node_size < 12)
			goto corrupt;
		memcpy(&magic, node, 2);
		memcpy(&entries, node + 2, 2);
		memcpy(&max_entries, node + 4, 2);
		memcpy(&depth, node + 6, 2);
		magic = (u16_t)conv2(le_CPU, magic);
		entries = (u16_t)conv2(le_CPU, entries);
		max_entries = (u16_t)conv2(le_CPU, max_entries);
		depth = (u16_t)conv2(le_CPU, depth);

		if (magic != EXT4_EXTENT_MAGIC ||
		    depth > EXT4_EXTENT_MAX_DEPTH ||
		    (expected_depth >= 0 && depth != expected_depth) ||
		    max_entries == 0 ||
		    max_entries > (node_size - 12) / 12 ||
		    entries > max_entries)
			goto corrupt;

		/* External extent nodes carry a checksum after their max array. */
		if (bp != NULL &&
		    HAS_RO_COMPAT_FEATURE(rip->i_sp, RO_COMPAT_METADATA_CSUM)) {
			size_t tail_offset = 12 + (size_t)max_entries * 12;
			u32_t stored, calculated;

			if (tail_offset > node_size - sizeof(stored))
				goto corrupt;
			memcpy(&stored, node + tail_offset, sizeof(stored));
			stored = (u32_t)conv4(le_CPU, stored);
			calculated = ext4_crc32c(ext4_inode_csum_seed(rip),
			    node, tail_offset);
			if (stored != calculated)
				goto corrupt;
		}

		if (depth == 0) {
			u64_t previous_end = 0;

			for (i = 0; i < entries; i++) {
				u32_t first, physical_lo;
				u16_t encoded_len, physical_hi;
				u64_t physical, length, logical_end;
				int unwritten;
				unsigned int j;

				entry = node + 12 + i * 12;
				memcpy(&first, entry, 4);
				memcpy(&encoded_len, entry + 4, 2);
				memcpy(&physical_hi, entry + 6, 2);
				memcpy(&physical_lo, entry + 8, 4);
				first = (u32_t)conv4(le_CPU, first);
				encoded_len = (u16_t)conv2(le_CPU, encoded_len);
				physical_hi = (u16_t)conv2(le_CPU, physical_hi);
				physical_lo = (u32_t)conv4(le_CPU, physical_lo);

				unwritten = encoded_len > 0x8000;
				length = unwritten ? encoded_len - 0x8000 :
				    encoded_len;
				logical_end = (u64_t)first + length;
				physical = ((u64_t)physical_hi << 32) | physical_lo;
				if (length == 0 ||
				    logical_end > ((u64_t)UINT32_MAX + 1) ||
				    (i != 0 && first < previous_end) ||
				    !ext4_validate_data_range(rip, physical, length))
					goto corrupt;
				for (j = 0; j < visited_count; j++) {
					if (visited[j] >= physical &&
					    visited[j] - physical < length)
						goto corrupt;
				}
				previous_end = logical_end;

				if ((u64_t)logical >= first &&
				    (u64_t)logical < logical_end) {
					if (unwritten) {
						if (bp != NULL)
							put_block(bp);
						return(NO_BLOCK);
					}
					physical += (u64_t)logical - first;
					if (bp != NULL)
						put_block(bp);
					return((block64_t)physical);
				}
			}
			if (bp != NULL)
				put_block(bp);
			return(NO_BLOCK);
		}

		/* Select the last index whose logical start does not exceed target. */
		{
			block64_t child = 0;
			u32_t previous_key = 0;
			int found = FALSE;

			if (entries == 0) {
				if (bp != NULL)
					put_block(bp);
				return(NO_BLOCK);
			}
			for (i = 0; i < entries; i++) {
				u32_t logical_first, child_lo;
				u16_t child_hi;

				entry = node + 12 + i * 12;
				memcpy(&logical_first, entry, 4);
				memcpy(&child_lo, entry + 4, 4);
				memcpy(&child_hi, entry + 8, 2);
				logical_first = (u32_t)conv4(le_CPU, logical_first);
				child_lo = (u32_t)conv4(le_CPU, child_lo);
				child_hi = (u16_t)conv2(le_CPU, child_hi);
				if (i != 0 && logical_first <= previous_key)
					goto corrupt;
				previous_key = logical_first;
				if (i == 0 && logical < logical_first) {
					if (bp != NULL)
						put_block(bp);
					return(NO_BLOCK);
				}
				if (logical_first > logical)
					break;
				child = ((u64_t)child_hi << 32) | child_lo;
				found = TRUE;
			}
			if (!found ||
			    !ext4_validate_data_range(rip, child, 1) ||
			    visited_count >= EXT4_EXTENT_MAX_DEPTH)
				goto corrupt;
			for (i = 0; i < visited_count; i++) {
				if (visited[i] == child)
					goto corrupt;
			}
			visited[visited_count++] = child;
			if (bp != NULL) {
				put_block(bp);
				bp = NULL;
			}
			bp = get_block(rip->i_dev, child, NORMAL);
			if (bp == NULL)
				goto corrupt;
			node = (const unsigned char *)b_data(bp);
			node_size = rip->i_sp->s_block_size;
			expected_depth = depth - 1;
		}
	}

corrupt:
	if (bp != NULL)
		put_block(bp);
	err_code = EIO;
	return(NO_BLOCK);
}

/*
 * Read an ext4 legacy direct/indirect mapping only after validating every
 * pointer block and resulting data block. The format has twelve direct
 * pointers followed by single, double, and triple-indirect roots.
 */
static block64_t
ext4_indirect_map(struct inode *rip, u64_t block_pos)
{
	struct buf *bp;
	block64_t block, data_block;
	u64_t excess;
	unsigned int addresses, addresses2;
	int index;

	addresses = rip->i_sp->s_block_size / BLOCK_ADDRESS_BYTES;
	if (addresses == 0)
		goto corrupt;
	addresses2 = addresses * addresses;

	if (block_pos < EXT2_NDIR_BLOCKS) {
		data_block = rip->i_block[block_pos];
		if (data_block == NO_BLOCK ||
		    ext4_validate_data_range(rip, data_block, 1))
			return(data_block);
		goto corrupt;
	}

	if (block_pos < EXT2_NDIR_BLOCKS + addresses) {
		block = rip->i_block[EXT2_IND_BLOCK];
		excess = block_pos - EXT2_NDIR_BLOCKS;
		if (block == NO_BLOCK)
			return(NO_BLOCK);
		if (!ext4_validate_data_range(rip, block, 1))
			goto corrupt;
		bp = get_block(rip->i_dev, block, NORMAL);
		if (bp == NULL)
			goto corrupt;
		data_block = rd_indir(bp, (int)excess);
		put_block(bp);
		if (data_block == NO_BLOCK ||
		    ext4_validate_data_range(rip, data_block, 1))
			return(data_block);
		goto corrupt;
	}

	if (block_pos >= EXT2_NDIR_BLOCKS + addresses + addresses2 +
	    (u64_t)addresses2 * addresses)
		return(NO_BLOCK);

	block = rip->i_block[EXT2_DIND_BLOCK];
	if (block_pos >= EXT2_NDIR_BLOCKS + addresses + addresses2) {
		excess = block_pos - EXT2_NDIR_BLOCKS - addresses - addresses2;
		block = rip->i_block[EXT2_TIND_BLOCK];
		if (block == NO_BLOCK)
			return(NO_BLOCK);
		if (!ext4_validate_data_range(rip, block, 1))
			goto corrupt;
		bp = get_block(rip->i_dev, block, NORMAL);
		if (bp == NULL)
			goto corrupt;
		index = (int)(excess / addresses2);
		block = rd_indir(bp, index);
		put_block(bp);
		if (block == NO_BLOCK)
			return(NO_BLOCK);
		if (!ext4_validate_data_range(rip, block, 1))
			goto corrupt;
		excess %= addresses2;
	} else {
		excess = block_pos - EXT2_NDIR_BLOCKS - addresses;
		if (block == NO_BLOCK)
			return(NO_BLOCK);
	}

	if (!ext4_validate_data_range(rip, block, 1))
		goto corrupt;
	bp = get_block(rip->i_dev, block, NORMAL);
	if (bp == NULL)
		goto corrupt;
	index = (int)(excess / addresses);
	block = rd_indir(bp, index);
	put_block(bp);
	if (block == NO_BLOCK)
		return(NO_BLOCK);
	if (!ext4_validate_data_range(rip, block, 1))
		goto corrupt;

	bp = get_block(rip->i_dev, block, NORMAL);
	if (bp == NULL)
		goto corrupt;
	index = (int)(excess % addresses);
	data_block = rd_indir(bp, index);
	put_block(bp);
	if (data_block == NO_BLOCK ||
	    ext4_validate_data_range(rip, data_block, 1))
		return(data_block);

corrupt:
	err_code = EIO;
	return(NO_BLOCK);
}

block64_t read_map(register struct inode *rip, off_t position,
	int opportunistic)
{
	struct buf *bp;
	int mindex;
	block64_t b;
	u64_t excess, block_pos;
	static char first_time = TRUE;
	static long addr_in_block;
	static long addr_in_block2;
	static long doub_ind_s;
	static long triple_ind_s;
	static long out_range_s;
	int iomode;

	iomode = opportunistic ? PEEK : NORMAL;
	if (position < 0)
		return(NO_BLOCK);
	block_pos = (u64_t)position / rip->i_sp->s_block_size;
	if (rip->i_sp->s_is_ext4 &&
	    (rip->i_flags & EXT4_EXTENTS_FL)) {
		if (block_pos > ((u32_t)~0U)) {
			err_code = EIO;
			return(NO_BLOCK);
		}
		return(ext4_extent_map(rip, (u32_t)block_pos));
	}

	if (first_time) {
		addr_in_block = rip->i_sp->s_block_size / BLOCK_ADDRESS_BYTES;
		addr_in_block2 = addr_in_block * addr_in_block;
		doub_ind_s = EXT2_NDIR_BLOCKS + addr_in_block;
		triple_ind_s = doub_ind_s + addr_in_block2;
		out_range_s = triple_ind_s + addr_in_block2 * addr_in_block;
		first_time = FALSE;
	}

	if (rip->i_sp->s_is_ext4)
		return(ext4_indirect_map(rip, block_pos));

	if (block_pos < EXT2_NDIR_BLOCKS)
		return(rip->i_block[block_pos]);
	if (block_pos < (u64_t)doub_ind_s) {
		b = rip->i_block[EXT2_NDIR_BLOCKS];
		mindex = (int)(block_pos - EXT2_NDIR_BLOCKS);
	} else if (block_pos >= (u64_t)out_range_s) {
		return(NO_BLOCK);
	} else {
		excess = block_pos - doub_ind_s;
		b = rip->i_block[EXT2_DIND_BLOCK];
		if (block_pos >= (u64_t)triple_ind_s) {
			b = rip->i_block[EXT2_TIND_BLOCK];
			if (b == NO_BLOCK) return(NO_BLOCK);
			bp = get_block(rip->i_dev, b, NORMAL);
			excess = block_pos - triple_ind_s;
			mindex = (int)(excess / addr_in_block2);
			b = rd_indir(bp, mindex);
			put_block(bp);
			excess %= addr_in_block2;
		}
		if (b == NO_BLOCK) return(NO_BLOCK);
		bp = get_block(rip->i_dev, b, iomode);
		if (bp == NULL) return(NO_BLOCK);
		mindex = (int)(excess / addr_in_block);
		b = rd_indir(bp, mindex);
		put_block(bp);
		mindex = (int)(excess % addr_in_block);
	}
	if (b == NO_BLOCK) return(NO_BLOCK);
	bp = get_block(rip->i_dev, b, iomode);
	if (bp == NULL) return(NO_BLOCK);
	b = rd_indir(bp, mindex);
	put_block(bp);
	return(b);
}


struct buf *get_block_map(register struct inode *rip, u64_t position)
{
	struct buf *bp;
	int r, block_size;
	block64_t b;
	err_code = OK;
	b = read_map(rip, (off_t)position, 0);
	if (err_code != OK || b == NO_BLOCK)
		return NULL;
	block_size = get_block_size(rip->i_dev);
	position = rounddown(position, block_size);
	assert(rip->i_num != VMC_NO_INODE);
	if ((r = lmfs_get_block_ino(&bp, rip->i_dev, b, NORMAL, rip->i_num,
	    position)) != OK)
		panic("ext2: error getting block (%llu,%llu): %d",
		    (unsigned long long)rip->i_dev, (unsigned long long)b, r);
	return bp;
}

/*===========================================================================*
 *				rd_indir													 *
 *				bp: pointer to indirect block								 *
 *				mindex: index into *bp										 *
 *===========================================================================*/
block64_t rd_indir(struct buf *bp, int mindex)
{
  if (bp == NULL)
	panic("rd_indir() on NULL");
  /* TODO: use conv call */
  return conv4(le_CPU, b_ind(bp)[mindex]);
}


/*===========================================================================*
 *				rahead														 *
 *				rip : pointer to inode for file to be read					 *
 *				baselock: block at current position							 *
 *				position: position within file								 *
 *				bytes ahead: bytes beyond position for immediate use		 *
 *===========================================================================*/
static struct buf *rahead(register struct inode *rip, block64_t baseblock,
						  u64_t position, unsigned bytes_ahead)
{
/* Fetch a block from the cache or the device.  If a physical read is
 * required, prefetch as many more blocks as convenient into the cache.
 * This usually covers bytes_ahead and is at least BLOCKS_MINIMUM.
 * The device driver may decide it knows better and stop reading at a
 * cylinder boundary (or after an error).  Rw_scattered() puts an optional
 * flag on all reads to allow this.
 */
/* Minimum number of blocks to prefetch. */
# define BLOCKS_MINIMUM		32
  int r, read_q_size;
  unsigned int blocks_ahead, fragment, block_size;
  block64_t block, blocks_left;
  off_t ind1_pos;
  dev_t dev;
  struct buf *bp = NULL;
  static block64_t read_q[LMFS_MAX_PREFETCH];
  u64_t position_running;

  dev = rip->i_dev;
  assert(dev != NO_DEV);
  block_size = get_block_size(dev);

  block = baseblock;

  fragment = position % block_size;
  position -= fragment;
  position_running = position;
  bytes_ahead += fragment;
  blocks_ahead = (bytes_ahead + block_size - 1) / block_size;

  r = lmfs_get_block_ino(&bp, dev, block, PEEK, rip->i_num, position);
  if (r == OK)
	return(bp);
  if (r != ENOENT)
	panic("ext2: error getting block (%llu,%llu): %d", (unsigned long long)dev, (unsigned long long)block, r);

  /* The best guess for the number of blocks to prefetch:  A lot.
   * It is impossible to tell what the device looks like, so we don't even
   * try to guess the geometry, but leave it to the driver.
   *
   * The floppy driver can read a full track with no rotational delay, and it
   * avoids reading partial tracks if it can, so handing it enough buffers to
   * read two tracks is perfect.  (Two, because some diskette types have
   * an odd number of sectors per track, so a block may span tracks.)
   *
   * The disk drivers don't try to be smart.  With todays disks it is
   * impossible to tell what the real geometry looks like, so it is best to
   * read as much as you can.  With luck the caching on the drive allows
   * for a little time to start the next read.
   *
   * The current solution below is a bit of a hack, it just reads blocks from
   * the current file position hoping that more of the file can be found.  A
   * better solution must look at the already available
   * indirect blocks (but don't call read_map!).
   */

  blocks_left = (block64_t) (rip->i_size-(off_t)position+(block_size-1)) /
                                                                block_size;

  /* Go for the first indirect block if we are in its neighborhood. */
  ind1_pos = (EXT2_NDIR_BLOCKS) * block_size;
  if ((off_t)position <= ind1_pos && rip->i_size > ind1_pos) {
	blocks_ahead++;
	blocks_left++;
  }

  /* Read at least the minimum number of blocks, but not after a seek. */
  if (blocks_ahead < BLOCKS_MINIMUM && rip->i_seek == NO_SEEK)
	blocks_ahead = BLOCKS_MINIMUM;

  /* Can't go past end of file. */
  if (blocks_ahead > blocks_left) blocks_ahead = blocks_left;

  /* No more than the maximum request. */
  if (blocks_ahead > LMFS_MAX_PREFETCH) blocks_ahead = LMFS_MAX_PREFETCH;

  read_q_size = 0;

  /* Acquire block buffers. */
  for (;;) {
  	block64_t thisblock;
	read_q[read_q_size++] = block;

	if (--blocks_ahead == 0) break;

	block++;
	position_running += block_size;

	thisblock = read_map(rip, (off_t)position_running, 1);
	if (thisblock != NO_BLOCK) {
		r = lmfs_get_block_ino(&bp, dev, thisblock, PEEK, rip->i_num,
		    position_running);
		block = thisblock;
	} else
		r = lmfs_get_block(&bp, dev, block, PEEK);

	if (r == OK) {
		/* Oops, block already in the cache, get out. */
		put_block(bp);
		break;
	}
	if (r != ENOENT)
		panic("ext2: error getting block (%llu,%llu): %d", (unsigned long long)dev, (unsigned long long)block,
		    r);
  }
  lmfs_prefetch(dev, read_q, read_q_size);

  r = lmfs_get_block_ino(&bp, dev, baseblock, NORMAL, rip->i_num, position);
  if (r != OK)
	panic("ext2: error getting block (%llu,%llu): %d", (unsigned long long)dev, (unsigned long long)baseblock, r);
  return bp;
}


/*===========================================================================*
 *				get_dtype				     *
 *===========================================================================*/
static unsigned int get_dtype(struct ext2_disk_dir_desc *dp)
{
/* Return the type of the file identified by the given directory entry. */

  if (!HAS_INCOMPAT_FEATURE(superblock, INCOMPAT_FILETYPE))
	return DT_UNKNOWN;

  switch (dp->d_file_type) {
  case EXT2_FT_REG_FILE:	return DT_REG;
  case EXT2_FT_DIR:		return DT_DIR;
  case EXT2_FT_SYMLINK:		return DT_LNK;
  case EXT2_FT_BLKDEV:		return DT_BLK;
  case EXT2_FT_CHRDEV:		return DT_CHR;
  case EXT2_FT_FIFO:		return DT_FIFO;
  case EXT2_FT_SOCK:		return DT_SOCK;
  case EXT2_FT_UNKNOWN:		return DT_UNKNOWN;
  default:			return DT_UNKNOWN;
  }
}

/*===========================================================================*
 *				fs_getdents				     *
 *===========================================================================*/
ssize_t fs_getdents(ino_t ino_nr, struct fsdriver_data *data, size_t bytes,
	off_t *posp)
{
#define GETDENTS_BUFSIZE (sizeof(struct dirent) + EXT2_NAME_MAX + 1)
#define GETDENTS_ENTRIES	8
  static char getdents_buf[GETDENTS_BUFSIZE * GETDENTS_ENTRIES];
  struct fsdriver_dentry fsdentry;
  struct inode *rip;
  int r, done;
  unsigned int block_size, len, block_off, rec_len;
  off_t pos, off, block_pos, new_pos, ent_pos;
  struct buf *bp;
  struct ext2_disk_dir_desc *d_desc;
  ino_t child_nr;

  /* Check whether the position is properly aligned */
  pos = *posp;
  if (pos < 0 || (unsigned int) pos % DIR_ENTRY_ALIGN)
	return(ENOENT);

  if ((rip = get_inode(fs_dev, ino_nr)) == NULL)
	return(EINVAL);

  block_size = rip->i_sp->s_block_size;
  off = (pos % block_size);             /* Offset in block */
  block_pos = pos - off;
  done = FALSE;       /* Stop processing directory blocks when done is set */

  fsdriver_dentry_init(&fsdentry, data, bytes, getdents_buf,
	sizeof(getdents_buf));

  /* The default position for the next request is EOF. If the user's buffer
   * fills up before EOF, new_pos will be modified. */
  new_pos = rip->i_size;

  r = 0;

  for (; block_pos < rip->i_size; block_pos += block_size) {
	off_t temp_pos = block_pos;
        /* Since directories don't have holes, 'bp' cannot be NULL. */
        bp = get_block_map(rip, block_pos);     /* get a dir block */
        if (bp == NULL) {
		int e = err_code != OK ? err_code : EIO;
		put_inode(rip);
		return(e);
	}

	/* Verify the checksum before interpreting directory entries. */
	if (!ext4_verify_dir_block(rip,
	    (const unsigned char *)b_data(bp),
	    (unsigned int)(block_pos / block_size))) {
		put_block(bp);
		put_inode(rip);
		return(EIO);
	}

	/* Search a directory block. */
	d_desc = (struct ext2_disk_dir_desc*) &b_data(bp);

	/* Position at the first entry at or after the requested offset. */
	block_off = 0;
	while (block_off < block_size) {
		if (!ext4_valid_dirent(d_desc, block_off, block_size)) {
			put_block(bp);
			put_inode(rip);
			return(EIO);
		}
		rec_len = ext4_dir_rec_len(d_desc, block_size);
		if (temp_pos + rec_len > pos || block_off + rec_len >= block_size)
			break;
		temp_pos += rec_len;
		block_off += rec_len;
		d_desc = (struct ext2_disk_dir_desc *)((char *)d_desc + rec_len);
	}

	for (; block_off < block_size;
	     block_off += rec_len, d_desc = (struct ext2_disk_dir_desc *)((char *)d_desc + rec_len)) {
		if (!ext4_valid_dirent(d_desc, block_off, block_size)) {
			put_block(bp);
			put_inode(rip);
			return(EIO);
		}
		rec_len = ext4_dir_rec_len(d_desc, block_size);

		if (d_desc->d_ino == 0)
			continue; /* Entry is not in use */

		len = d_desc->d_name_len;
		if (len > NAME_MAX || len > EXT2_NAME_MAX) {
			put_block(bp);
			put_inode(rip);
			return(EIO);
		}

		/* Need the position of this entry in the directory */
		ent_pos = block_pos + ((char *)d_desc - b_data(bp));

		child_nr = (ino_t) conv4(le_CPU, d_desc->d_ino);
		if (child_nr == NO_ENTRY ||
		    child_nr > rip->i_sp->s_inodes_count) {
			put_block(bp);
			put_inode(rip);
			return(EIO);
		}
		r = fsdriver_dentry_add(&fsdentry, child_nr, d_desc->d_name,
			len, get_dtype(d_desc));

		/* If the user buffer is full, or an error occurred, stop. */
		if (r <= 0) {
			done = TRUE;

			/* Record the position of this entry, it is the
			 * starting point of the next request (unless the
			 * position is modified with lseek).
			 */
			new_pos = ent_pos;
			break;
		}

		/* Skip the dx root-info/index area after ".." in block zero. */
		if (block_pos == 0 && (rip->i_flags & EXT2_INDEX_FL) &&
		    len == 2 && d_desc->d_name[0] == '.' &&
		    d_desc->d_name[1] == '.')
			break;
	}

	put_block(bp);
	if (done)
		break;
  }

  if (r >= 0 && (r = fsdriver_dentry_finish(&fsdentry)) >= 0) {
	*posp = new_pos;
	rip->i_update |= ATIME;
	if (!rip->i_sp->s_rd_only)
		rip->i_dirt = IN_DIRTY;
  }

  put_inode(rip);               /* release the inode */
  return(r);
}
