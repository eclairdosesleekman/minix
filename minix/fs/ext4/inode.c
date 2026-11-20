/* This file manages the inode table.  There are procedures to allocate and
 * deallocate inodes, acquire, erase, and release them, and read and write
 * them from the disk.
 *
 * The entry points into this file are
 *   get_inode:       search inode table for a given inode; if not there,
 *                 read it
 *   put_inode:       indicate that an inode is no longer needed in memory
 *   update_times: update atime, ctime, and mtime
 *   rw_inode:       read a disk block and extract an inode, or corresp. write
 *   dup_inode:       indicate that someone else is using an inode table entry
 *   find_inode:   retrieve pointer to inode in inode cache
 *
 * Created (MFS based):
 *   February 2010 (Evgeniy Ivanov)
 */

#include "fs.h"
#include <string.h>
#include "buf.h"
#include "inode.h"
#include "super.h"
#include <minix/vfsif.h>

static void icopy(struct inode *rip, d_inode *dip, int direction, int
	norm, u64_t ext4_i_blocks);
static void addhash_inode(struct inode *node);
static void unhash_inode(struct inode *node);


/*===========================================================================*
 *                fs_putnode                                                 *
 *===========================================================================*/
int fs_putnode(ino_t ino_nr, unsigned int count)
{
/* Find the inode specified by the request message and decrease its counter.*/

  struct inode *rip;

  rip = find_inode(fs_dev, ino_nr);

  if (!rip) {
	printf("%s:%d put_inode: inode #%llu dev: %llx not found\n", __FILE__,
		__LINE__, (unsigned long long)ino_nr, (unsigned long long)fs_dev);
	panic("fs_putnode failed");
  }

  if (count > rip->i_count) {
	printf("%s:%d put_inode: count too high: %d > %d\n", __FILE__,
		__LINE__, count, rip->i_count);
	panic("fs_putnode failed");
  }

  /* Decrease reference counter, but keep one reference;
   * it will be consumed by put_inode().
   */
  rip->i_count -= count - 1;
  put_inode(rip);

  return(OK);
}


/*===========================================================================*
 *                init_inode_cache                                           *
 *===========================================================================*/
void init_inode_cache(void)
{
  struct inode *rip;
  struct inodelist *rlp;

  inode_cache_hit = 0;
  inode_cache_miss = 0;

  /* init free/unused list */
  TAILQ_INIT(&unused_inodes);

  /* init hash lists */
  for (rlp = &hash_inodes[0]; rlp < &hash_inodes[INODE_HASH_SIZE]; ++rlp)
	LIST_INIT(rlp);

  /* add free inodes to unused/free list */
  for (rip = &inode[0]; rip < &inode[NR_INODES]; ++rip) {
	rip->i_num = NO_ENTRY;
	TAILQ_INSERT_HEAD(&unused_inodes, rip, i_unused);
  }
}


/*===========================================================================*
 *                addhash_inode                                              *
 *===========================================================================*/
static void addhash_inode(struct inode *node)
{
  int hashi = node->i_num & INODE_HASH_MASK;

  /* insert into hash table */
  LIST_INSERT_HEAD(&hash_inodes[hashi], node, i_hash);
}


/*===========================================================================*
 *                unhash_inode                                               *
 *===========================================================================*/
static void unhash_inode(struct inode *node)
{
  /* remove from hash table */
  LIST_REMOVE(node, i_hash);
}


/*===========================================================================*
 *                get_inode                                                  *
 *===========================================================================*/
struct inode *get_inode(
  dev_t dev,          /* device on which inode resides */
  ino_t numb         /* inode number (ANSI: may not be unshort) */
)
{
/* Find the inode in the hash table. If it is not there, get a free inode
 * load it from the disk if it's necessary and put on the hash list
 */
  register struct inode *rip;
  int hashi;
  int i, r;

  hashi = (int) numb & INODE_HASH_MASK;

  /* Search inode in the hash table */
  LIST_FOREACH(rip, &hash_inodes[hashi], i_hash) {
	if (rip->i_num == numb && rip->i_dev == dev) {
		/* If unused, remove it from the unused/free list */
		if (rip->i_count == 0) {
			inode_cache_hit++;
			TAILQ_REMOVE(&unused_inodes, rip, i_unused);
		}
		++rip->i_count;
		return(rip);
	}
  }

  inode_cache_miss++;

  /* Inode is not on the hash, get a free one */
  if (TAILQ_EMPTY(&unused_inodes)) {
	err_code = ENFILE;
	return(NULL);
  }
  rip = TAILQ_FIRST(&unused_inodes);

  /* If not free unhash it */
  if (rip->i_num != NO_ENTRY)
	unhash_inode(rip);

  /* Inode is not unused any more */
  TAILQ_REMOVE(&unused_inodes, rip, i_unused);

  /* Load the inode. */
  rip->i_dev = dev;
  rip->i_num = numb;
  rip->i_count = 1;
  if (dev != NO_DEV) {
	if ((r = rw_inode(rip, READING)) != OK) {
		/* A bad inode checksum is an I/O error, not a server panic. */
		rip->i_count = 0;
		rip->i_num = NO_ENTRY;
		rip->i_dev = NO_DEV;
		rip->i_sp = NULL;
		rip->i_dirt = IN_CLEAN;
		rip->i_update = 0;
		TAILQ_INSERT_HEAD(&unused_inodes, rip, i_unused);
		err_code = r;
		return(NULL);
	}
  }
  rip->i_update = 0;        /* all the times are initially up-to-date */
  rip->i_last_dpos = 0;     /* no dentries searched for yet */
  rip->i_bsearch = NO_BLOCK;
  rip->i_last_pos_bl_alloc = 0;
  rip->i_last_dentry_size = 0;
  rip->i_mountpoint= FALSE;

  rip->i_preallocation = opt.use_prealloc;
  rip->i_prealloc_count = rip->i_prealloc_index = 0;

  if (rip->i_sp != NULL && rip->i_sp->s_rd_only) {
	for (i = 0; i < EXT2_PREALLOC_BLOCKS; i++)
		rip->i_prealloc_blocks[i] = NO_BLOCK;
  } else {
	for (i = 0; i < EXT2_PREALLOC_BLOCKS; i++) {
		if (rip->i_prealloc_blocks[i] != NO_BLOCK) {
			free_block(rip->i_sp, rip->i_prealloc_blocks[i]);
			rip->i_prealloc_blocks[i] = NO_BLOCK;
			ext2_debug("Warning: Unexpected preallocated block.");
		}
	}
  }

  /* Add to hash */
  addhash_inode(rip);

  return(rip);
}


/*===========================================================================*
 *                find_inode                                                 *
 *===========================================================================*/
struct inode *find_inode(
  dev_t dev,          /* device on which inode resides */
  ino_t numb         /* inode number (ANSI: may not be unshort) */
)
{
/* Find the inode specified by the inode and device number. */
  struct inode *rip;
  int hashi;

  hashi = (int) numb & INODE_HASH_MASK;

  /* Search inode in the hash table */
  LIST_FOREACH(rip, &hash_inodes[hashi], i_hash) {
	if (rip->i_count > 0 && rip->i_num == numb && rip->i_dev == dev) {
		return(rip);
	}
  }

  return(NULL);
}


/*===========================================================================*
 *                put_inode                                                  *
 *===========================================================================*/
void put_inode(
  register struct inode *rip     /* pointer to inode to be released */
)
{
/* The caller is no longer using this inode. If no one else is using it either
 * write it back to the disk immediately. If it has no links, truncate it and
 * return it to the pool of available inodes.
 */
  int truncate_error, keep_allocated;


  if (rip == NULL)
	return;    /* checking here is easier than in caller */

  if (rip->i_count < 1)
	panic("put_inode: i_count already below 1: %d", rip->i_count);

  if (--rip->i_count == 0) {    /* i_count == 0 means no one is using it now */
	keep_allocated = FALSE;
	if (rip->i_links_count == NO_LINK &&
	    (rip->i_sp == NULL || !rip->i_sp->s_rd_only)) {
		/* i_nlinks == NO_LINK means free the inode. */
		/* return all the disk blocks */

		/*
		 * Do not release the inode bitmap if data/extent reclamation fails.
		 * Doing so would let a new inode reuse blocks still referenced by
		 * this inode. Special files have no data mapping to truncate.
		 */
		truncate_error = truncate_inode(rip, (off_t) 0);
		/*
		 * The old ext2 path treats truncate's EINVAL for special files as
		 * expected because device nodes have no data mapping to truncate.
		 */
		if (truncate_error == EINVAL &&
		    ((rip->i_mode & I_TYPE) == I_CHAR_SPECIAL ||
		    (rip->i_mode & I_TYPE) == I_BLOCK_SPECIAL))
			truncate_error = OK;
		if (truncate_error == OK && rip->i_sp != NULL &&
		    rip->i_sp->s_is_ext4)
			truncate_error = ext4_release_xattr_block(rip);
		if (truncate_error != OK) {
			printf("ext4: keeping unlinked inode %llu allocated after "
			    "truncate error %d\n",
			    (unsigned long long)rip->i_num, truncate_error);
			keep_allocated = TRUE;
			rip->i_dirt = IN_DIRTY;
		} else {
			/* free inode clears I_TYPE field, since it's used there */
			rip->i_dirt = IN_DIRTY;
			free_inode(rip);
		}
	}

	rip->i_mountpoint = FALSE;
	if (rip->i_dirt == IN_DIRTY &&
	    (rip->i_sp == NULL || !rip->i_sp->s_rd_only))
		rw_inode(rip, WRITING);

	if (rip->i_sp == NULL || !rip->i_sp->s_rd_only)
		discard_preallocated_blocks(rip); /* Return blocks to the filesystem */

	if (rip->i_links_count == NO_LINK && !keep_allocated) {
		/* free, put at the front of the LRU list */
		unhash_inode(rip);
		rip->i_num = NO_ENTRY;
		TAILQ_INSERT_HEAD(&unused_inodes, rip, i_unused);
	} else {
		/* unused, put at the back of the LRU (cache it) */
		TAILQ_INSERT_TAIL(&unused_inodes, rip, i_unused);
	}
  }
}


/*===========================================================================*
 *                update_times                                               *
 *===========================================================================*/
void update_times(
  register struct inode *rip     /* pointer to inode to be read/written */
)
{
/* Various system calls are required by the standard to update atime, ctime,
 * or mtime.  Since updating a time requires sending a message to the clock
 * task--an expensive business--the times are marked for update by setting
 * bits in i_update. When a stat, fstat, or sync is done, or an inode is
 * released, update_times() may be called to actually fill in the times.
 */

  time_t cur_time;
  struct super_block *sp;

  sp = rip->i_sp;         /* get pointer to super block. */
  if (sp->s_rd_only)
	return;             /* no updates for read-only file systems */

  cur_time = clock_time(NULL);
  if (rip->i_update & ATIME) {
	rip->i_atime = cur_time;
	rip->i_atime_nsec = 0;
  }
  if (rip->i_update & CTIME) {
	rip->i_ctime = cur_time;
	rip->i_ctime_nsec = 0;
  }
  if (rip->i_update & MTIME) {
	rip->i_mtime = cur_time;
	rip->i_mtime_nsec = 0;
  }
  rip->i_update = 0;          /* they are all up-to-date now */
}

/*===========================================================================*
 *                rw_inode                                                   *
 *===========================================================================*/
/*
 * Convert the in-core 512-byte sector count to ext4's on-disk i_blocks
 * representation. The huge_file feature provides a 48-bit counter; values
 * beyond 48 bits of sectors are stored in filesystem-block units and require
 * EXT4_HUGE_FILE_FL on the inode.
 */
static int
ext4_encode_inode_blocks(struct inode *rip, u64_t *disk_blocks)
{
	struct super_block *sp = rip->i_sp;
	u64_t blocks;

	blocks = rip->i_blocks;

	if (!HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_HUGE_FILE)) {
		if (blocks > 0xffffffffULL)
			return(EFBIG);
		rip->i_flags &= ~EXT4_HUGE_FILE_FL;
		*disk_blocks = blocks;
		return(OK);
	}

	/* The 48-bit field can first represent 512-byte sectors directly. */
	if (blocks <= 0xffffffffffffULL) {
		rip->i_flags &= ~EXT4_HUGE_FILE_FL;
		*disk_blocks = blocks;
		return(OK);
	}

	/*
	 * For still larger files, Linux stores filesystem blocks instead of
	 * 512-byte sectors and marks the inode with HUGE_FILE. Allocation
	 * accounting is block-granular, so the conversion must be exact.
	 */
	if (sp->s_sectors_in_block == 0 ||
	    blocks % sp->s_sectors_in_block != 0)
		return(EINVAL);
	blocks /= sp->s_sectors_in_block;
	if (blocks > 0xffffffffffffULL)
		return(EFBIG);

	rip->i_flags |= EXT4_HUGE_FILE_FL;
	*disk_blocks = blocks;
	return(OK);
}


/*
 * Linux ext4 stores extended timestamps after the original 128-byte inode.
 * Each *_extra word contains the two high seconds bits in bits 0..1 and
 * nanoseconds in bits 2..31. i_extra_isize bounds each optional field.
 */
#define EXT4_INODE_EXTRA_ISIZE_OFF 128
#define EXT4_INODE_CTIME_EXTRA_OFF 132
#define EXT4_INODE_MTIME_EXTRA_OFF 136
#define EXT4_INODE_ATIME_EXTRA_OFF 140
#define EXT4_INODE_CRTIME_OFF      144
#define EXT4_INODE_CRTIME_EXTRA_OFF 148

struct ext4_encoded_time {
	u32_t seconds;
	u32_t extra;
	int has_extra;
};

struct ext4_encoded_times {
	struct ext4_encoded_time atime, ctime, mtime, crtime;
	int enabled;
	int has_crtime;
};

static u16_t
ext4_inode_read16(const unsigned char *raw, size_t offset)
{
	u16_t value;

	memcpy(&value, raw + offset, sizeof(value));
	return((u16_t)conv2(le_CPU, value));
}

static u32_t
ext4_inode_read32(const unsigned char *raw, size_t offset)
{
	u32_t value;

	memcpy(&value, raw + offset, sizeof(value));
	return((u32_t)conv4(le_CPU, value));
}

static void
ext4_inode_store32(unsigned char *raw, size_t offset, u32_t value)
{
	value = (u32_t)conv4(le_CPU, value);
	memcpy(raw + offset, &value, sizeof(value));
}

static int
ext4_inode_extra_isize(struct super_block *sp, const unsigned char *raw,
	size_t inode_size, u16_t *extra_isize)
{
	*extra_isize = 0;
	if (!sp->s_is_ext4 || sp->s_creator_os != EXT4_OS_LINUX ||
	    inode_size == EXT2_GOOD_OLD_INODE_SIZE)
		return(OK);
	if (inode_size < EXT4_INODE_EXTRA_ISIZE_OFF + sizeof(u16_t))
		return(EIO);

	*extra_isize = ext4_inode_read16(raw, EXT4_INODE_EXTRA_ISIZE_OFF);
	if (*extra_isize > inode_size - EXT2_GOOD_OLD_INODE_SIZE ||
	    (*extra_isize & 3) != 0)
		return(EIO);
	return(OK);
}

static int
ext4_inode_extra_fits(u16_t extra_isize, size_t inode_size, size_t end_offset)
{
	return(end_offset <= inode_size &&
	    end_offset >= EXT2_GOOD_OLD_INODE_SIZE &&
	    extra_isize >= end_offset - EXT2_GOOD_OLD_INODE_SIZE);
}

static int
ext4_decode_one_time(const unsigned char *raw, size_t inode_size,
	u16_t extra_isize, size_t seconds_offset, size_t extra_offset,
	time_t *seconds, long *nanoseconds)
{
	u32_t extra;

	*seconds = (time_t)(int32_t)ext4_inode_read32(raw, seconds_offset);
	*nanoseconds = 0;
	if (extra_offset == 0 ||
	    !ext4_inode_extra_fits(extra_isize, inode_size, extra_offset + 4))
		return(OK);

	extra = ext4_inode_read32(raw, extra_offset);
	*seconds += (time_t)((u64_t)(extra & 3U) << 32);
	*nanoseconds = (long)(extra >> 2);
	if (*nanoseconds >= 1000000000L)
		return(EIO);
	return(OK);
}

static int
ext4_decode_inode_times(struct inode *rip, const unsigned char *raw,
	size_t inode_size)
{
	u16_t extra_isize;
	int r;

	rip->i_atime_nsec = 0;
	rip->i_ctime_nsec = 0;
	rip->i_mtime_nsec = 0;
	rip->i_crtime = 0;
	rip->i_crtime_nsec = 0;
	rip->i_ext4_extra_isize = 0;

	if (!rip->i_sp->s_is_ext4 ||
	    rip->i_sp->s_creator_os != EXT4_OS_LINUX)
		return(OK);

	/* EA inodes repurpose the legacy timestamp words for xattr metadata. */
	if (rip->i_flags & EXT4_EA_INODE_FL) {
		rip->i_atime = rip->i_ctime = rip->i_mtime = 0;
		return(OK);
	}

	r = ext4_inode_extra_isize(rip->i_sp, raw, inode_size,
	    &extra_isize);
	if (r != OK)
		return(r);
	rip->i_ext4_extra_isize = extra_isize;

	r = ext4_decode_one_time(raw, inode_size, extra_isize, 8,
	    EXT4_INODE_ATIME_EXTRA_OFF, &rip->i_atime, &rip->i_atime_nsec);
	if (r != OK)
		return(r);
	r = ext4_decode_one_time(raw, inode_size, extra_isize, 12,
	    EXT4_INODE_CTIME_EXTRA_OFF, &rip->i_ctime, &rip->i_ctime_nsec);
	if (r != OK)
		return(r);
	r = ext4_decode_one_time(raw, inode_size, extra_isize, 16,
	    EXT4_INODE_MTIME_EXTRA_OFF, &rip->i_mtime, &rip->i_mtime_nsec);
	if (r != OK)
		return(r);

	if (ext4_inode_extra_fits(extra_isize, inode_size,
	    EXT4_INODE_CRTIME_OFF + 4)) {
		r = ext4_decode_one_time(raw, inode_size, extra_isize,
		    EXT4_INODE_CRTIME_OFF, EXT4_INODE_CRTIME_EXTRA_OFF,
		    &rip->i_crtime, &rip->i_crtime_nsec);
		if (r != OK)
			return(r);
	}
	return(OK);
}

static int
ext4_encode_one_time(time_t seconds, long nanoseconds, int has_extra,
	struct ext4_encoded_time *encoded)
{
	time_t low_seconds;
	u32_t epoch;

	if (nanoseconds < 0 || nanoseconds >= 1000000000L)
		return(EINVAL);

	if (!has_extra) {
		if (seconds < (time_t)-2147483648LL ||
		    seconds > (time_t)2147483647LL || nanoseconds != 0)
			return(EOVERFLOW);
		encoded->seconds = (u32_t)(int32_t)seconds;
		encoded->extra = 0;
		encoded->has_extra = FALSE;
		return(OK);
	}

	/* ext4's signed 34-bit seconds range is -2^31 through 0x37fffffff. */
	if (seconds < (time_t)-2147483648LL ||
	    seconds > (time_t)15032385535LL)
		return(EOVERFLOW);
	epoch = seconds < 0 ? 0 :
	    (u32_t)(((u64_t)seconds + 0x80000000ULL) >> 32);
	low_seconds = seconds - (time_t)((u64_t)epoch << 32);
	if (low_seconds < (time_t)-2147483648LL ||
	    low_seconds > (time_t)2147483647LL)
		return(EOVERFLOW);

	encoded->seconds = (u32_t)(int32_t)low_seconds;
	encoded->extra = (epoch & 3U) | ((u32_t)nanoseconds << 2);
	encoded->has_extra = TRUE;
	return(OK);
}

static int
ext4_prepare_inode_times(struct inode *rip, const unsigned char *raw,
	size_t inode_size, struct ext4_encoded_times *encoded)
{
	struct super_block *sp = rip->i_sp;
	u16_t extra_isize = 0;
	int r;

	memset(encoded, 0, sizeof(*encoded));
	if (!sp->s_is_ext4 || (rip->i_flags & EXT4_EA_INODE_FL))
		return(OK);

	r = ext4_inode_extra_isize(sp, raw, inode_size, &extra_isize);
	if (r != OK)
		return(r);
	rip->i_ext4_extra_isize = extra_isize;

	encoded->atime.has_extra = ext4_inode_extra_fits(extra_isize,
	    inode_size, EXT4_INODE_ATIME_EXTRA_OFF + 4);
	encoded->ctime.has_extra = ext4_inode_extra_fits(extra_isize,
	    inode_size, EXT4_INODE_CTIME_EXTRA_OFF + 4);
	encoded->mtime.has_extra = ext4_inode_extra_fits(extra_isize,
	    inode_size, EXT4_INODE_MTIME_EXTRA_OFF + 4);

	r = ext4_encode_one_time(rip->i_atime, rip->i_atime_nsec,
	    encoded->atime.has_extra, &encoded->atime);
	if (r != OK)
		return(r);
	r = ext4_encode_one_time(rip->i_ctime, rip->i_ctime_nsec,
	    encoded->ctime.has_extra, &encoded->ctime);
	if (r != OK)
		return(r);
	r = ext4_encode_one_time(rip->i_mtime, rip->i_mtime_nsec,
	    encoded->mtime.has_extra, &encoded->mtime);
	if (r != OK)
		return(r);

	if (sp->s_creator_os == EXT4_OS_LINUX &&
	    ext4_inode_extra_fits(extra_isize, inode_size,
	    EXT4_INODE_CRTIME_OFF + 4)) {
		encoded->crtime.has_extra = ext4_inode_extra_fits(extra_isize,
		    inode_size, EXT4_INODE_CRTIME_EXTRA_OFF + 4);
		r = ext4_encode_one_time(rip->i_crtime, rip->i_crtime_nsec,
		    encoded->crtime.has_extra, &encoded->crtime);
		if (r != OK)
			return(r);
		encoded->has_crtime = TRUE;
	} else if (sp->s_creator_os == EXT4_OS_LINUX) {
		/* This inode format has no creation-time field. */
		rip->i_crtime = 0;
		rip->i_crtime_nsec = 0;
	}
	encoded->enabled = TRUE;
	return(OK);
}

static void
ext4_apply_inode_times(d_inode *dip, struct ext4_encoded_times *encoded)
{
	unsigned char *raw = (unsigned char *)dip;

	if (!encoded->enabled)
		return;
	ext4_inode_store32(raw, 8, encoded->atime.seconds);
	ext4_inode_store32(raw, 12, encoded->ctime.seconds);
	ext4_inode_store32(raw, 16, encoded->mtime.seconds);
	if (encoded->atime.has_extra)
		ext4_inode_store32(raw, EXT4_INODE_ATIME_EXTRA_OFF,
		    encoded->atime.extra);
	if (encoded->ctime.has_extra)
		ext4_inode_store32(raw, EXT4_INODE_CTIME_EXTRA_OFF,
		    encoded->ctime.extra);
	if (encoded->mtime.has_extra)
		ext4_inode_store32(raw, EXT4_INODE_MTIME_EXTRA_OFF,
		    encoded->mtime.extra);
	if (encoded->has_crtime) {
		ext4_inode_store32(raw, EXT4_INODE_CRTIME_OFF,
		    encoded->crtime.seconds);
		if (encoded->crtime.has_extra)
			ext4_inode_store32(raw, EXT4_INODE_CRTIME_EXTRA_OFF,
			    encoded->crtime.extra);
	}
}


/*
 * Decode the read-only subset of ext4 inline data implemented here.
 * The first 60 bytes live in i_block; any remainder is the value of the
 * inode-body xattr "system.data". e_value_offs is relative to the first
 * xattr entry (immediately after the four-byte ibody header).
 */
static int
ext4_decode_inline_data(struct inode *rip, const unsigned char *raw,
	size_t inode_size)
{
	size_t header_offset, entries_offset, pos, entries_end, area_size;
	size_t entry_size, value_offset;
	u32_t value_inum = 0, value_size = 0;
	u16_t value_offs = 0;
	int found = FALSE, terminated = FALSE;
	unsigned int name_len, name_index;

	rip->i_inline_size = 0;
	memset(rip->i_inline_data, 0, sizeof(rip->i_inline_data));
	if (!(rip->i_flags & EXT4_INLINE_DATA_FL))
		return(OK);

	if (!HAS_INCOMPAT_FEATURE(rip->i_sp, INCOMPAT_INLINE_DATA) ||
	    rip->i_sp->s_creator_os != EXT4_OS_LINUX)
		return(EIO);

	/* Inline directories have two separate dirent areas and a special
	 * parent-inode prefix; they are deliberately not treated as file bytes.
	 */
	if ((rip->i_mode & I_TYPE) == I_DIRECTORY)
		return(EOPNOTSUPP);
	if ((rip->i_mode & I_TYPE) != I_REGULAR &&
	    (rip->i_mode & I_TYPE) != I_SYMBOLIC_LINK)
		return(EOPNOTSUPP);
	if (rip->i_flags & EXT4_EA_INODE_FL)
		return(EIO);

	if (inode_size < 128 || rip->i_ext4_extra_isize == 0 ||
	    rip->i_ext4_extra_isize > inode_size - 128)
		return(EIO);
	header_offset = 128 + rip->i_ext4_extra_isize;
	if (header_offset > inode_size - 8 ||
	    ext4_inode_read32(raw, header_offset) != EXT4_XATTR_MAGIC)
		return(EIO);

	entries_offset = header_offset + 4;
	pos = entries_offset;
	entries_end = 0;
	while (pos <= inode_size - 4) {
		name_len = raw[pos];
		name_index = raw[pos + 1];
		if (name_len == 0 && name_index == 0) {
			/* ext4's four-byte terminator must be entirely zero. */
			if (ext4_inode_read16(raw, pos + 2) != 0)
				return(EIO);
			terminated = TRUE;
			entries_end = pos + 4;
			break;
		}
		if (inode_size - pos < EXT4_XATTR_ENTRY_SIZE)
			return(EIO);
		entry_size = (EXT4_XATTR_ENTRY_SIZE + name_len + 3U) & ~3U;
		if (entry_size > inode_size - pos)
			return(EIO);

		if (name_index == EXT4_XATTR_INDEX_SYSTEM && name_len == 4 &&
		    memcmp(raw + pos + EXT4_XATTR_ENTRY_SIZE, "data", 4) == 0) {
			if (found)
				return(EIO);
			found = TRUE;
			value_offs = ext4_inode_read16(raw, pos + 2);
			value_inum = ext4_inode_read32(raw, pos + 4);
			value_size = ext4_inode_read32(raw, pos + 8);
		}
		pos += entry_size;
	}
	if (!terminated || !found)
		return(EIO);
	if (value_inum != 0)
		return(EOPNOTSUPP);
	if (value_size > EXT4_INLINE_DATA_MAX - EXT4_INLINE_DATA_MIN)
		return(EOPNOTSUPP);

	area_size = inode_size - entries_offset;
	value_offset = value_offs;
	if (value_size != 0 &&
	    (value_offset < entries_end - entries_offset ||
	    value_offset > area_size || value_size > area_size - value_offset))
		return(EIO);
	if (rip->i_size < 0 ||
	    (u64_t)rip->i_size > EXT4_INLINE_DATA_MIN + value_size)
		return(EIO);

	/* i_block is byte-oriented inline content, not extent pointers. */
	memcpy(rip->i_inline_data, raw + 40, EXT4_INLINE_DATA_MIN);
	if (value_size != 0)
		memcpy(rip->i_inline_data + EXT4_INLINE_DATA_MIN,
		    raw + entries_offset + value_offset, value_size);
	rip->i_inline_size = EXT4_INLINE_DATA_MIN + value_size;
	return(OK);
}


int rw_inode(
  register struct inode *rip,         /* pointer to inode to be read/written */
  int rw_flag                         /* READING or WRITING */
)
{
/* An entry in the inode table is to be copied to or from the disk. */

  register struct buf *bp;
  register struct super_block *sp;
  register struct group_desc *gd;
  register d_inode *dip;
  u32_t block_group_number;
  block64_t b, xattr_block;
  u16_t xattr_block_hi;
  u64_t disk_blocks = 0;
  struct ext4_encoded_times encoded_times;
  int r;
  size_t offset;

  /* Get the block where the inode resides. */
  sp = get_super(rip->i_dev);     /* get pointer to super block */
  rip->i_sp = sp;        /* inode must contain super block pointer */

  block_group_number = (rip->i_num - 1) / sp->s_inodes_per_group;

  gd = get_group_desc(block_group_number);

  if (gd == NULL)
	panic("can't get group_desc to read/write inode");

  offset = ((rip->i_num - 1) % sp->s_inodes_per_group) * EXT2_INODE_SIZE(sp);
  /* offset requires shifting, since each block contains several inodes,
   * e.g. inode 2 is stored in bklock 0.
   */
  b = gd->inode_table + (offset >> sp->s_blocksize_bits);
  bp = get_block(rip->i_dev, b, NORMAL);

  offset &= (sp->s_block_size - 1);
  dip = (d_inode*) (b_data(bp) + offset);

  /* Refuse to interpret an inode whose metadata checksum is invalid. */
  if (rw_flag == READING &&
      HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_METADATA_CSUM) &&
      !ext4_verify_inode_csum(sp, rip->i_num,
      (const unsigned char *)dip, EXT2_INODE_SIZE(sp))) {
	put_block(bp);
	return(EIO);
  }

  /*
   * Linux ext4 stores i_file_acl as a 48-bit block pointer when 64bit is
   * enabled. Validate every nonzero pointer, even without metadata_csum:
   * the external xattr header still has structural invariants that protect
   * later xattr reference-count and release operations.
   */
  if (rw_flag == READING && sp->s_is_ext4 &&
      sp->s_creator_os == EXT4_OS_LINUX) {
	xattr_block = (block64_t)(u32_t)conv4(le_CPU, dip->i_file_acl);
	if (HAS_INCOMPAT_FEATURE(sp, INCOMPAT_64BIT)) {
		xattr_block_hi = (u16_t)conv2(le_CPU,
		    dip->osd2.linux2.i_pad1);
		xattr_block |= (block64_t)xattr_block_hi << 32;
	}
	if (xattr_block != 0 &&
	    !ext4_validate_xattr_block(sp, rip->i_dev, xattr_block)) {
		printf("ext4: invalid xattr block for inode %llu\n",
		    (unsigned long long)rip->i_num);
		put_block(bp);
		return(EIO);
	}
  }

  /* Do the read or write. */
  if (rw_flag == WRITING) {
	if (rip->i_update)
		update_times(rip);    /* times need updating */
	if (sp->s_is_ext4) {
		r = ext4_encode_inode_blocks(rip, &disk_blocks);
		if (r != OK) {
			put_block(bp);
			return(r);
		}
		r = ext4_prepare_inode_times(rip,
		    (const unsigned char *)dip, EXT2_INODE_SIZE(sp),
		    &encoded_times);
		if (r != OK) {
			put_block(bp);
			return(r);
		}
	}
  }

  icopy(rip, dip, rw_flag, TRUE, disk_blocks);
  if (rw_flag == READING && sp->s_is_ext4) {
	r = ext4_decode_inode_times(rip, (const unsigned char *)dip,
	    EXT2_INODE_SIZE(sp));
	if (r != OK) {
		put_block(bp);
		return(r);
	}
	r = ext4_decode_inline_data(rip, (const unsigned char *)dip,
	    EXT2_INODE_SIZE(sp));
	if (r != OK) {
		put_block(bp);
		return(r);
	}
  } else if (rw_flag == WRITING && sp->s_is_ext4) {
	ext4_apply_inode_times(dip, &encoded_times);
  }
  if (rw_flag == WRITING && sp->s_rd_only == FALSE)
	lmfs_markdirty(bp);

  put_block(bp);
  rip->i_dirt = IN_CLEAN;
  return(OK);
}


/*===========================================================================*
 *				icopy					     *
 *===========================================================================*/
static void icopy(
  register struct inode *rip,	/* pointer to the in-core inode struct */
  register d_inode *dip,	/* pointer to the on-disk struct */
  int direction,		/* READING (from disk) or WRITING (to disk) */
  int norm,			/* TRUE = do not swap bytes; FALSE = swap */
  u64_t ext4_i_blocks		/* encoded ext4 48-bit block count */
)
{
  int i;

  if (direction == READING) {
	/* Copy inode to the in-core table, swapping bytes if need be. */
	rip->i_mode    = conv2(norm,dip->i_mode);
	rip->i_uid     = (u32_t)conv2(norm, dip->i_uid);
	if (rip->i_sp->s_is_ext4 &&
	    rip->i_sp->s_creator_os == EXT4_OS_LINUX)
		rip->i_uid |= (u32_t)conv2(norm,
		    dip->osd2.linux2.l_i_uid_high) << 16;
	if (rip->i_sp->s_is_ext4 &&
	    ((rip->i_mode & I_TYPE) == I_REGULAR ||
	    ((rip->i_mode & I_TYPE) == I_DIRECTORY &&
	    HAS_INCOMPAT_FEATURE(rip->i_sp, INCOMPAT_LARGEDIR)))) {
		/*
		 * ext4 stores the upper 32 bits of large directory sizes in
		 * i_dir_acl/i_size_high when INCOMPAT_LARGEDIR is enabled.
		 */
		rip->i_size = (off_t)(((u64_t)(u32_t)conv4(norm,
		    dip->i_dir_acl) << 32) |
		    (u32_t)conv4(norm, dip->i_size));
	} else {
		rip->i_size = (off_t)(u32_t)conv4(norm, dip->i_size);
	}
	rip->i_atime   = (time_t)(int32_t)(u32_t)conv4(norm,dip->i_atime);
	rip->i_ctime   = (time_t)(int32_t)(u32_t)conv4(norm,dip->i_ctime);
	rip->i_mtime   = (time_t)(int32_t)(u32_t)conv4(norm,dip->i_mtime);
	rip->i_dtime   = conv4(norm,dip->i_dtime);
	rip->i_gid     = (u32_t)conv2(norm, dip->i_gid);
	if (rip->i_sp->s_is_ext4 &&
	    rip->i_sp->s_creator_os == EXT4_OS_LINUX)
		rip->i_gid |= (u32_t)conv2(norm,
		    dip->osd2.linux2.l_i_gid_high) << 16;
	rip->i_links_count  = conv2(norm,dip->i_links_count);
	rip->i_flags	= conv4(norm,dip->i_flags);
	rip->i_blocks	= (u64_t)(u32_t)conv4(norm,dip->i_blocks);
	if (HAS_RO_COMPAT_FEATURE(rip->i_sp, RO_COMPAT_HUGE_FILE)) {
		u16_t blocks_high;

		/*
		 * ext4 reuses the first 16 bits of osd2 for i_blocks_high.
		 * The combined value is in 512-byte sectors unless the inode's
		 * HUGE_FILE flag says it is in filesystem blocks.
		 */
		/* Linux l_i_blocks_high is the first 16 bits of osd2. */
		memcpy(&blocks_high, &dip->osd2, sizeof(blocks_high));
		rip->i_blocks |= (u64_t)(u16_t)conv2(norm, blocks_high) << 32;
		if (rip->i_flags & EXT4_HUGE_FILE_FL)
			rip->i_blocks *= rip->i_sp->s_sectors_in_block;
	}
	/* Minix doesn't touch osd1 and osd2 either, so just copy. */
	memcpy(&rip->osd1, &dip->osd1, sizeof(rip->osd1));
	for (i = 0; i < EXT2_N_BLOCKS; i++)
		rip->i_block[i] = conv4(norm, dip->i_block[i]);
	rip->i_generation = conv4(norm,dip->i_generation);
	rip->i_file_acl	= conv4(norm,dip->i_file_acl);
	rip->i_dir_acl  = conv4(norm,dip->i_dir_acl);
	rip->i_faddr	= conv4(norm,dip->i_faddr);
	memcpy(&rip->osd2, &dip->osd2, sizeof(rip->osd2));
  } else {
	/* Copying inode to disk from the in-core table. */
	dip->i_mode    = conv2(norm,rip->i_mode);
	dip->i_uid     = conv2(norm, (u16_t)rip->i_uid);
	if (rip->i_sp->s_is_ext4 &&
	    (rip->i_mode & I_TYPE) == I_REGULAR) {
		u64_t size = (u64_t)rip->i_size;
		dip->i_size = conv4(norm, (u32_t)size);
		dip->i_dir_acl = conv4(norm, (u32_t)(size >> 32));
	} else {
		dip->i_size = conv4(norm, rip->i_size);
	}
	dip->i_atime   = conv4(norm,(u32_t)rip->i_atime);
	dip->i_ctime   = conv4(norm,(u32_t)rip->i_ctime);
	dip->i_mtime   = conv4(norm,(u32_t)rip->i_mtime);
	dip->i_dtime   = conv4(norm,rip->i_dtime);
	dip->i_gid     = conv2(norm, (u16_t)rip->i_gid);
	dip->i_links_count  = conv2(norm,rip->i_links_count);
	dip->i_blocks	= conv4(norm, rip->i_sp->s_is_ext4 ?
	    (u32_t)ext4_i_blocks : (u32_t)rip->i_blocks);
	dip->i_flags	= conv4(norm,rip->i_flags);
	/* Minix doesn't touch osd1 and osd2 either, so just copy. */
	memcpy(&dip->osd1, &rip->osd1, sizeof(dip->osd1));
	for (i = 0; i < EXT2_N_BLOCKS; i++)
		dip->i_block[i] = conv4(norm, rip->i_block[i]);
	dip->i_generation  = conv4(norm,rip->i_generation);
	dip->i_file_acl = conv4(norm,rip->i_file_acl);
	dip->i_dir_acl	= conv4(norm,rip->i_dir_acl);
	dip->i_faddr	= conv4(norm,rip->i_faddr);
	memcpy(&dip->osd2, &rip->osd2, sizeof(dip->osd2));
	if (rip->i_sp->s_is_ext4 &&
	    rip->i_sp->s_creator_os == EXT4_OS_LINUX) {
		/*
		 * Linux stores the upper 16 bits of UID/GID in osd2. Keep
		 * the adjacent high block-count and xattr-block fields intact.
		 */
		dip->osd2.linux2.l_i_uid_high = conv2(norm,
		    (u16_t)(rip->i_uid >> 16));
		dip->osd2.linux2.l_i_gid_high = conv2(norm,
		    (u16_t)(rip->i_gid >> 16));
	}
	if (rip->i_sp->s_is_ext4 &&
	    HAS_RO_COMPAT_FEATURE(rip->i_sp, RO_COMPAT_HUGE_FILE)) {
		u16_t blocks_high;

		blocks_high = (u16_t)conv2(norm,
		    (u16_t)(ext4_i_blocks >> 32));
		/* Linux stores i_blocks_high in the first 16 bits of osd2. */
		memcpy(&dip->osd2, &blocks_high, sizeof(blocks_high));
	}
  }
}


/*===========================================================================*
 *                dup_inode                                                  *
 *===========================================================================*/
void dup_inode(
  struct inode *ip         /* The inode to be duplicated. */
)
{
/* This routine is a simplified form of get_inode() for the case where
 * the inode pointer is already known.
 */
  ip->i_count++;
}
