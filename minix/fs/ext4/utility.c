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
