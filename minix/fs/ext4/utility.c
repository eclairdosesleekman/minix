/* Created (MFS based):
 *   February 2010 (Evgeniy Ivanov)
 */

#include "fs.h"
#include "buf.h"
#include "inode.h"
#include "super.h"


/*===========================================================================*
 *				get_block				     *
 *===========================================================================*/
struct buf *get_block(dev_t dev, block64_t block, int how)
{
/* Wrapper routine for lmfs_get_block(). This ext2 implementation does not deal
 * well with block read errors pretty much anywhere. To prevent corruption due
 * to unchecked error conditions, we panic upon an I/O failure here.
 */
  struct buf *bp;
  int r;

  if ((r = lmfs_get_block(&bp, dev, block, how)) != OK && r != ENOENT)
	panic("ext4: error getting block (%llu,%llu): %d", (unsigned long long)dev, (unsigned long long)block, r);

  assert(r == OK || how == PEEK);

  return (r == OK) ? bp : NULL;
}



/*
 * ext4 metadata_csum uses the reflected Castagnoli CRC32C polynomial.
 * The seed and return value match the Linux crc32c() API, without a final
 * complement, so checksum streams can be updated incrementally.
 */
u32_t
ext4_crc32c(u32_t crc, const void *data, size_t len)
{
	const unsigned char *p = data;
	unsigned int bit;

	while (len-- != 0) {
		crc ^= *p++;
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^
			    (0x82f63b78U & (u32_t)-(int)(crc & 1));
	}
	return(crc);
}

static u16_t
ext4_csum_read16(const unsigned char *p)
{
	u16_t value;

	memcpy(&value, p, sizeof(value));
	return((u16_t)conv2(le_CPU, value));
}

static u32_t
ext4_csum_read32(const unsigned char *p)
{
	u32_t value;

	memcpy(&value, p, sizeof(value));
	return((u32_t)conv4(le_CPU, value));
}

/* Per-inode seed: filesystem UUID seed, inode number, then generation. */
u32_t
ext4_inode_csum_seed(struct inode *rip)
{
	u32_t inum, generation, crc;

	inum = (u32_t)conv4(le_CPU, (u32_t)rip->i_num);
	generation = (u32_t)conv4(le_CPU, rip->i_generation);
	crc = ext4_crc32c(rip->i_sp->s_csum_seed, &inum, sizeof(inum));
	return(ext4_crc32c(crc, &generation, sizeof(generation)));
}

/*
 * Validate a physical data or extent-tree block range before following it.
 * Metadata blocks owned by any group (including flex_bg placements) and the
 * inode's external xattr block must never be exposed as file data.
 */
int
ext4_validate_data_range(struct inode *rip, block64_t first, u64_t length)
{
	struct super_block *sp = rip->i_sp;
	struct group_desc *gd;
	block64_t end, xattr_block, range_first, range_end;
	u64_t max_block, remainder, extension;
	unsigned int group;

	if (length == 0 || first <= sp->s_first_data_block ||
	    first >= sp->s_blocks_count ||
	    length > sp->s_blocks_count - first ||
	    sp->s_block_size == 0)
		return(FALSE);

	max_block = ((u64_t)~0 >> 1) / sp->s_block_size;
	if (first > max_block || length - 1 > max_block - first)
		return(FALSE);
	end = first + length;
	range_first = first;
	range_end = end;

	/*
	 * BIGALLOC reserves entire allocation clusters. A malformed extent
	 * must not point at a different block in a cluster that contains a
	 * superblock/GDT copy, group bitmap, inode table, or external xattr.
	 * Clip the final partial cluster to the filesystem's block count.
	 */
	if (HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_BIGALLOC)) {
		range_first -= (range_first - sp->s_first_data_block) %
		    sp->s_cluster_ratio;
		remainder = (range_end - sp->s_first_data_block) %
		    sp->s_cluster_ratio;
		if (remainder != 0) {
			extension = sp->s_cluster_ratio - remainder;
			if (extension > sp->s_blocks_count - range_end)
				range_end = sp->s_blocks_count;
			else
				range_end += extension;
		}
		/* The cluster containing the primary superblock is never data. */
		if (range_first <= sp->s_first_data_block)
			return(FALSE);
	}

	/* Primary and backup super/GDT clusters are reserved filesystem metadata. */
	if (ext4_reserved_metadata_overlaps(sp, range_first, range_end))
		return(FALSE);

	for (group = 0; group < sp->s_groups_count; group++) {
		gd = &sp->s_group_desc[group];
		if ((gd->block_bitmap >= range_first &&
		    gd->block_bitmap < range_end) ||
		    (gd->inode_bitmap >= range_first &&
		    gd->inode_bitmap < range_end))
			return(FALSE);
		if (gd->inode_table <= range_first) {
			if (range_first - gd->inode_table < sp->s_itb_per_group)
				return(FALSE);
		} else if (gd->inode_table < range_end) {
			return(FALSE);
		}
	}

	if (sp->s_creator_os == EXT4_OS_LINUX) {
		xattr_block = (block64_t)rip->i_file_acl;
		if (HAS_INCOMPAT_FEATURE(sp, INCOMPAT_64BIT))
			xattr_block |= (block64_t)(u16_t)conv2(le_CPU,
			    rip->osd2.linux2.i_pad1) << 32;
		if (xattr_block != 0 && xattr_block >= range_first &&
		    xattr_block < range_end)
			return(FALSE);
	}

	return(TRUE);
}

/*
 * Verify the Linux ext4 inode checksum. The checksum-low field is at
 * offset 124; checksum-high is at offset 130 when i_extra_isize includes it.
 */
int
ext4_verify_inode_csum(struct super_block *sp, ino_t ino,
	const unsigned char *raw, size_t inode_size)
{
	static const u16_t zero16 = 0;
	u32_t inum, generation, seed, crc, provided;
	u16_t extra_isize, checksum_lo, checksum_hi;
	size_t offset;
	int have_hi = FALSE;

	if (!HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_METADATA_CSUM))
		return(TRUE);
	if (sp->s_creator_os != EXT4_OS_LINUX || ino > UINT32_MAX ||
	    inode_size < 128)
		return(FALSE);

	inum = (u32_t)conv4(le_CPU, (u32_t)ino);
	generation = (u32_t)conv4(le_CPU, ext4_csum_read32(raw + 100));
	seed = ext4_crc32c(sp->s_csum_seed, &inum, sizeof(inum));
	seed = ext4_crc32c(seed, &generation, sizeof(generation));

	checksum_lo = ext4_csum_read16(raw + 124);
	crc = ext4_crc32c(seed, raw, 124);
	crc = ext4_crc32c(crc, &zero16, sizeof(zero16));
	crc = ext4_crc32c(crc, raw + 126, 2);

	provided = checksum_lo;
	offset = 128;
	if (inode_size > 128) {
		if (inode_size < 130)
			return(FALSE);
		extra_isize = ext4_csum_read16(raw + 128);
		if (extra_isize > inode_size - 128)
			return(FALSE);

		crc = ext4_crc32c(crc, raw + 128, 2);
		offset = 130;
		if (extra_isize >= 4 && inode_size >= 132) {
			checksum_hi = ext4_csum_read16(raw + 130);
			provided |= (u32_t)checksum_hi << 16;
			crc = ext4_crc32c(crc, &zero16, sizeof(zero16));
			offset = 132;
			have_hi = TRUE;
		}
		if (offset < inode_size)
			crc = ext4_crc32c(crc, raw + offset, inode_size - offset);
	}
	if (!have_hi)
		crc &= 0xffffU;
	return(provided == crc);
}

/*
 * Validate an external ext4 xattr block. The fixed header is present even
 * when metadata_csum is disabled, so always check its magic, reference count,
 * single-block size, and reserved words. When metadata_csum is enabled,
 * Linux additionally computes h_checksum over the little-endian 64-bit block
 * number followed by the entire block with h_checksum treated as zero.
 */
int
ext4_validate_xattr_block(struct super_block *sp, dev_t dev,
    block64_t block)
{
	struct buf *bp;
	const unsigned char *data;
	unsigned char block_nr_le[8];
	u32_t block_lo, block_hi, magic, refcount, blocks;
	u32_t stored, crc, zero32 = 0;

	if (sp->s_creator_os != EXT4_OS_LINUX || sp->s_block_size < 32 ||
	    block <= sp->s_first_data_block ||
	    block >= sp->s_blocks_count ||
	    block > (((u64_t)~0 >> 1) / sp->s_block_size))
		return(FALSE);

	bp = get_block(dev, block, NORMAL);
	if (bp == NULL)
		return(FALSE);
	data = (const unsigned char *)b_data(bp);

	magic = ext4_csum_read32(data);
	refcount = ext4_csum_read32(data + 4);
	blocks = ext4_csum_read32(data + 8);
	if (magic != EXT4_XATTR_MAGIC || refcount == 0 ||
	    refcount > EXT4_XATTR_REFCOUNT_MAX || blocks != 1 ||
	    ext4_csum_read32(data + 20) != 0 ||
	    ext4_csum_read32(data + 24) != 0 ||
	    ext4_csum_read32(data + 28) != 0) {
		put_block(bp);
		return(FALSE);
	}

	if (!HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_METADATA_CSUM)) {
		put_block(bp);
		return(TRUE);
	}

	block_lo = (u32_t)conv4(le_CPU, (u32_t)block);
	block_hi = (u32_t)conv4(le_CPU, (u32_t)(block >> 32));
	memcpy(block_nr_le, &block_lo, sizeof(block_lo));
	memcpy(block_nr_le + sizeof(block_lo), &block_hi, sizeof(block_hi));

	stored = ext4_csum_read32(data + 16);
	crc = ext4_crc32c(sp->s_csum_seed, block_nr_le, sizeof(block_nr_le));
	crc = ext4_crc32c(crc, data, 16);
	crc = ext4_crc32c(crc, &zero32, sizeof(zero32));
	crc = ext4_crc32c(crc, data + 20, sp->s_block_size - 20);

	put_block(bp);
	return(stored == crc);
}

/*
 * Verify one ext4 directory block. Leaf blocks end in a fake dirent checksum
 * tail; htree root and internal nodes instead have a dx_tail after their
 * count/limit and entry array.
 */
int
ext4_verify_dir_block(struct inode *rip, const unsigned char *data,
	unsigned int logical_block)
{
	const struct ext2_disk_dir_desc *first, *second;
	const unsigned char *tail;
	unsigned int block_size, count_offset, tail_offset, count, limit;
	u32_t seed, crc, stored, zero32 = 0;

	block_size = rip->i_sp->s_block_size;
	if (block_size < 32)
		return(FALSE);
	first = (const struct ext2_disk_dir_desc *)data;

	/*
	 * Validate the HTree root layout even without metadata_csum.  LARGEDIR
	 * raises the maximum indirect_levels value from two to three.  The
	 * indexed LOOK_UP follows validated index pointers in path.c;
	 * getdents and unsupported-hash compatibility lookups still scan blocks
	 * linearly and skip this root area via the ".." rec_len.
	 */
	if ((rip->i_flags & EXT2_INDEX_FL) && logical_block == 0) {
		unsigned int max_levels;

		if (block_size < 40 || ext4_dir_rec_len(first, block_size) != 12)
			return(FALSE);
		second = (const struct ext2_disk_dir_desc *)(data + 12);
		if (ext4_dir_rec_len(second, block_size) != block_size - 12 ||
		    ext4_csum_read32(data + 24) != 0 || data[29] != 8 ||
		    data[31] != 0)
			return(FALSE);
		max_levels = HAS_INCOMPAT_FEATURE(rip->i_sp, INCOMPAT_LARGEDIR) ?
		    3 : 2;
		if (data[30] > max_levels)
			return(FALSE);
	}

	if (!HAS_RO_COMPAT_FEATURE(rip->i_sp, RO_COMPAT_METADATA_CSUM))
		return(TRUE);

	seed = ext4_inode_csum_seed(rip);
	if ((rip->i_flags & EXT2_INDEX_FL) && logical_block == 0) {
		count_offset = 32;
	} else if ((rip->i_flags & EXT2_INDEX_FL) &&
	    ext4_csum_read32(data) == 0 && data[6] == 0 &&
	    ext4_dir_rec_len(first, block_size) == block_size &&
	    ext4_csum_read16(data + 8) != 0 &&
	    ext4_csum_read16(data + 10) != 0 &&
	    ext4_csum_read16(data + 10) <= ext4_csum_read16(data + 8) &&
	    ext4_csum_read16(data + 8) <= (block_size - 16) / 8) {
		/* Internal htree node: fake full-block dirent precedes dx entries. */
		count_offset = 8;
	} else {
		/* Directory leaf block checksum tail (12 bytes). */
		tail_offset = block_size - 12;
		tail = data + tail_offset;
		if (ext4_csum_read32(tail) != 0 ||
		    ext4_csum_read16(tail + 4) != 12 ||
		    tail[6] != 0 || tail[7] != 0xde)
			return(FALSE);
		stored = ext4_csum_read32(tail + 8);
		crc = ext4_crc32c(seed, data, tail_offset);
		return(stored == crc);
	}

	if (count_offset + 4 > block_size)
		return(FALSE);
	limit = ext4_csum_read16(data + count_offset);
	count = ext4_csum_read16(data + count_offset + 2);
	if (limit == 0 || count == 0 || count > limit ||
	    limit > (block_size - count_offset - 8) / 8)
		return(FALSE);
	tail_offset = count_offset + limit * 8;
	tail = data + tail_offset;
	stored = ext4_csum_read32(tail + 4);
	crc = ext4_crc32c(seed, data, count_offset + count * 8);
	crc = ext4_crc32c(crc, tail, 4);
	crc = ext4_crc32c(crc, &zero32, sizeof(zero32));
	return(stored == crc);
}


/*
 * ext4 reserves 0xffff (and, for larger block sizes, zero) to represent
 * an entire directory block. For block sizes above 64 KiB, the low two
 * bits of the disk value hold the high two bits of the decoded length.
 * Keep decoding and encoding shared by all directory paths.
 */
unsigned int
ext4_dir_rec_len(const struct ext2_disk_dir_desc *dp, unsigned int block_size)
{
	unsigned int rec_len;

	rec_len = conv2(le_CPU, dp->d_rec_len);
	if (rec_len == 0xffffU || rec_len == 0)
		return block_size;
	if (block_size < (1U << 16))
		return rec_len;

	return (rec_len & 0xfffcU) | ((rec_len & 0x0003U) << 16);
}

void
ext4_set_dir_rec_len(struct ext2_disk_dir_desc *dp, unsigned int rec_len,
    unsigned int block_size)
{
	unsigned int encoded;

	if (rec_len < MIN_DIR_ENTRY_SIZE || rec_len > block_size ||
	    (rec_len & (DIR_ENTRY_ALIGN - 1)) != 0)
		panic("ext4: invalid directory record length %u/%u",
		    rec_len, block_size);

	if (rec_len < (1U << 16))
		encoded = rec_len;
	else if (rec_len == block_size && block_size == (1U << 16))
		encoded = 0xffffU;
	else if (rec_len == block_size)
		encoded = 0;
	else
		encoded = (rec_len & 0xfffcU) | ((rec_len >> 16) & 0x0003U);

	dp->d_rec_len = (u16_t)conv2(le_CPU, (int)encoded);
}

/*
 * Validate an on-disk ext directory entry before following its record
 * length or accessing its name. Directory blocks are untrusted input.
 */
int ext4_valid_dirent(const struct ext2_disk_dir_desc *dp,
    unsigned int offset, unsigned int block_size)
{
    unsigned int rec_len;

    if (offset > block_size || block_size - offset < MIN_DIR_ENTRY_SIZE)
        return FALSE;

    rec_len = ext4_dir_rec_len(dp, block_size);
    if (rec_len < MIN_DIR_ENTRY_SIZE ||
        (rec_len & (DIR_ENTRY_ALIGN - 1)) != 0 ||
        rec_len > block_size - offset ||
        DIR_ENTRY_ACTUAL_SIZE(dp) > rec_len)
        return FALSE;

    return TRUE;
}


/*===========================================================================*
 *				conv2														 *
 *				norm: TRUE if no swap, FALSE for byte swap					 *
 *				w: promotion of 16-bit word to be swapped					 *
 *===========================================================================*/
unsigned conv2(int norm, int w)
{
/* Possibly swap a 16-bit word between 8086 and 68000 byte order. */
  if (norm) return( (unsigned) w & 0xFFFF);
  return( ((w&BYTE) << 8) | ( (w>>8) & BYTE));
}


/*===========================================================================*
 *				conv4														 *
 *				norm: TRUE if no swap, FALSE for byte swap					 *
 *				x: 32-bit long to be byte swapped							 *
 *===========================================================================*/
long conv4(int norm, long x)
{
/* Possibly swap a 32-bit long between 8086 and 68000 byte order. */
  unsigned lo, hi;
  long l;

  if (norm) return(x);			/* byte order was already ok */
  lo = conv2(FALSE, (int) x & 0xFFFF);	/* low-order half, byte swapped */
  hi = conv2(FALSE, (int) (x>>16) & 0xFFFF);	/* high-order half, swapped */
  l = ( (long) lo <<16) | hi;
  return(l);
}


/*===========================================================================*
 *				ansi_strcmp				     *
 *===========================================================================*/
int ansi_strcmp(register const char* ansi_s, register const char *s2,
			register size_t ansi_s_length)
{
/* Compare non null-terminated string ansi_s (length=ansi_s_length)
 * with C-string s2.
 * It returns 0 if strings are equal, otherwise -1 is returned.
 */
	if (ansi_s_length) {
		do {
			if (*s2 == '\0')
				return -1;
			if (*ansi_s++ != *s2++)
				return -1;
		} while (--ansi_s_length > 0);

		if (*s2 == '\0')
			return 0;
		else
			return -1;
	}
	return 0;
}


/*===========================================================================*
 *				setbit   				     *
 *===========================================================================*/
bit_t setbit(bitchunk_t *bitmap, bit_t max_bits, unsigned int word)
{
  /* Find free bit in bitmap and set. Return number of the bit,
   * if failed return -1.
   */
  bitchunk_t *wptr, *wlim;
  bit_t b = -1;

  /* TODO: do we need to add 1? I saw a situation, when it was
   * required, and since we check bit number with max_bits it
   * should be safe.
   */
  wlim = &bitmap[FS_BITMAP_CHUNKS(max_bits >> 3)];

  /* Iterate over the words in block. */
  for (wptr = &bitmap[word]; wptr < wlim; wptr++) {
	bit_t i;
	bitchunk_t k;

	/* Does this word contain a free bit? */
	if (*wptr == (bitchunk_t) ~0)
		continue;

	/* Find and allocate the free bit. */
	k = (int) *wptr;
	for (i = 0; (k & (1 << i)) != 0; ++i) {}

	/* Bit number from the start of the bit map. */
	b = (wptr - &bitmap[0]) * FS_BITCHUNK_BITS + i;

	/* Don't allocate bits beyond the end of the map. */
	if (b >= max_bits) {
		b = -1;
		continue;
	}

	/* Allocate bit number. */
	k |= 1 << i;
	*wptr = (int) k;
	break;
  }

  return b;
}


/*===========================================================================*
 *				setbyte   				     *
 *===========================================================================*/
bit_t setbyte(bitchunk_t *bitmap, bit_t max_bits)
{
  /* Find free byte in bitmap and set it. Return number of the starting bit,
   * if failed return -1.
   */
  unsigned char *wptr, *wlim;
  bit_t b = -1;

  wptr = (unsigned char*) &bitmap[0];
  /* TODO: do we need to add 1? I saw a situation, when it was
   * required, and since we check bit number with max_bits it
   * should be safe.
   */
  wlim = &wptr[(max_bits >> 3)];

  /* Iterate over the words in block. */
  for ( ; wptr < wlim; wptr++) {
	/* Is it a free byte? */
	if (*wptr | 0)
		continue;

	/* Bit number from the start of the bit map. */
	b = (wptr - (unsigned char*) &bitmap[0]) * CHAR_BIT;

	/* Don't allocate bits beyond the end of the map. */
	if (b + CHAR_BIT >= max_bits) {
		b = -1;
		continue;
	}

	/* Allocate byte number. */
	*wptr = (unsigned char) ~0;
	break;
  }
  return b;
}


/*===========================================================================*
 *				unsetbit   				     *
 *===========================================================================*/
int unsetbit(bitchunk_t *bitmap, bit_t bit)
{
  /* Unset specified bit. If requested bit is already free return -1,
   * otherwise return 0.
   */
  unsigned int word;		/* bit_returned word in bitmap */
  bitchunk_t k, mask;

  word = bit / FS_BITCHUNK_BITS;
  bit = bit % FS_BITCHUNK_BITS; /* index in word */
  mask = 1 << bit;

  k = (int) bitmap[word];
  if (!(k & mask))
	return -1;

  k &= ~mask;
  bitmap[word] = (int) k;
  return 0;
}
