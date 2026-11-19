/*
 * Read-only verification of ext4 metadata checksums.
 *
 * ext4 uses the reflected CRC32C polynomial.  The routines here keep the
 * running (uncomplemented) CRC value so that multiple metadata fields can be
 * chained exactly as the Linux ext4 implementation does.
 */

#include "fs.h"
#include <string.h>
#include "inode.h"
#include "super.h"

#define EXT4_CRC32C_CHKSUM       1
#define EXT4_SB_CHECKSUM_OFFSET  0x3fc
#define EXT4_SB_CSUM_TYPE_OFFSET 0x175
#define EXT4_GD_CHECKSUM_OFFSET  0x1e
#define EXT4_INODE_CSUM_LO       0x7c
#define EXT4_INODE_EXTRA_ISIZE   0x80
#define EXT4_INODE_CSUM_HI       0x82
#define EXT4_INODE_GENERATION    0x64
#define EXT4_DIR_ENTRY_TAIL_SIZE 12

static u16_t
ext4_get_le16(const unsigned char *p)
{
	u16_t value;

	memcpy(&value, p, sizeof(value));
	return((u16_t)conv2(le_CPU, value));
}

static u32_t
ext4_get_le32(const unsigned char *p)
{
	u32_t value;

	memcpy(&value, p, sizeof(value));
	return((u32_t)conv4(le_CPU, value));
}

u32_t
ext4_crc32c(u32_t crc, const void *data, size_t length)
{
	const unsigned char *p = data;
	size_t i;
	unsigned int bit;

	for (i = 0; i < length; i++) {
		crc ^= p[i];
		for (bit = 0; bit < 8; bit++) {
			if (crc & 1)
				crc = (crc >> 1) ^ 0x82f63b78U;
			else
				crc >>= 1;
		}
	}

	return(crc);
}

static u32_t
ext4_inode_checksum_seed(struct super_block *sp, ino_t ino,
	const unsigned char *raw_inode)
{
	u32_t inode_le;
	u32_t crc;

	inode_le = (u32_t)conv4(le_CPU, (u32_t)ino);
	crc = ext4_crc32c(sp->s_checksum_seed, &inode_le,
	    sizeof(inode_le));
	/* i_generation is already in little-endian on-disk byte order. */
	return(ext4_crc32c(crc, raw_inode + EXT4_INODE_GENERATION, 4));
}

/*
 * The superblock checksum is CRC32C(~0, bytes[0..0x3fb]).  It is deliberately
 * independent of the UUID-derived seed used by the other metadata objects.
 */
int
ext4_verify_super_checksum(const unsigned char *raw_super)
{
	u32_t stored, calculated;

	if (raw_super[EXT4_SB_CSUM_TYPE_OFFSET] != EXT4_CRC32C_CHKSUM)
		return(FALSE);

	stored = ext4_get_le32(raw_super + EXT4_SB_CHECKSUM_OFFSET);
	calculated = ext4_crc32c((u32_t)~0U, raw_super,
	    EXT4_SB_CHECKSUM_OFFSET);
	return(stored == calculated);
}

int
ext4_verify_group_desc_checksum(struct super_block *sp, u32_t group,
	const unsigned char *raw_desc)
{
	unsigned char group_le[4], zero[2] = { 0, 0 };
	u32_t group_value, crc;
	u16_t stored;

	if (!sp->s_metadata_csum)
		return(TRUE);
	if (sp->s_desc_size < EXT4_GD_CHECKSUM_OFFSET + 2)
		return(FALSE);

	stored = ext4_get_le16(raw_desc + EXT4_GD_CHECKSUM_OFFSET);
	group_value = (u32_t)conv4(le_CPU, group);
	memcpy(group_le, &group_value, sizeof(group_value));

	crc = ext4_crc32c(sp->s_checksum_seed, group_le,
	    sizeof(group_le));
	crc = ext4_crc32c(crc, raw_desc, EXT4_GD_CHECKSUM_OFFSET);
	crc = ext4_crc32c(crc, zero, sizeof(zero));
	crc = ext4_crc32c(crc,
	    raw_desc + EXT4_GD_CHECKSUM_OFFSET + sizeof(zero),
	    sp->s_desc_size - EXT4_GD_CHECKSUM_OFFSET - sizeof(zero));

	return(stored == (u16_t)crc);
}

int
ext4_verify_inode_checksum(struct super_block *sp, ino_t ino,
	const unsigned char *raw_inode, size_t inode_size)
{
	static const unsigned char zero[2] = { 0, 0 };
	u32_t crc, stored, extra_isize;
	u16_t checksum_lo, checksum_hi;
	int has_checksum_hi;

	if (!sp->s_metadata_csum)
		return(OK);
	if (sp->s_creator_os != 0 || inode_size < EXT4_INODE_CSUM_LO + 2)
		return(EIO);

	checksum_lo = ext4_get_le16(raw_inode + EXT4_INODE_CSUM_LO);
	has_checksum_hi = FALSE;
	if (inode_size > EXT2_GOOD_OLD_INODE_SIZE) {
		if (inode_size < EXT4_INODE_EXTRA_ISIZE + 2)
			return(EIO);
		extra_isize = ext4_get_le16(raw_inode +
		    EXT4_INODE_EXTRA_ISIZE);
		if ((extra_isize & 3) != 0 ||
		    extra_isize > inode_size - EXT2_GOOD_OLD_INODE_SIZE)
			return(EIO);
		has_checksum_hi = extra_isize >= 4;
	}
	if (has_checksum_hi && inode_size < EXT4_INODE_CSUM_HI + 2)
		return(EIO);

	crc = ext4_inode_checksum_seed(sp, ino, raw_inode);
	crc = ext4_crc32c(crc, raw_inode, EXT4_INODE_CSUM_LO);
	crc = ext4_crc32c(crc, zero, sizeof(zero));
	crc = ext4_crc32c(crc, raw_inode + EXT4_INODE_CSUM_LO + 2,
	    (has_checksum_hi ? EXT4_INODE_CSUM_HI :
	    inode_size) - (EXT4_INODE_CSUM_LO + 2));
	if (has_checksum_hi) {
		crc = ext4_crc32c(crc, zero, sizeof(zero));
		crc = ext4_crc32c(crc,
		    raw_inode + EXT4_INODE_CSUM_HI + 2,
		    inode_size - (EXT4_INODE_CSUM_HI + 2));
	}

	stored = checksum_lo;
	if (has_checksum_hi) {
		checksum_hi = ext4_get_le16(raw_inode + EXT4_INODE_CSUM_HI);
		stored |= (u32_t)checksum_hi << 16;
	} else {
		crc &= 0xffffU;
	}

	return(stored == crc ? OK : EIO);
}

int
ext4_verify_extent_checksum(struct inode *rip, const unsigned char *data,
	size_t block_size)
{
	u32_t stored, calculated;
	u32_t seed;

	if (!rip->i_sp->s_metadata_csum)
		return(OK);
	if (block_size < sizeof(u32_t))
		return(EIO);

	stored = ext4_get_le32(data + block_size - sizeof(u32_t));
	seed = ext4_inode_checksum_seed(rip->i_sp, rip->i_num,
	    (const unsigned char *)rip->i_block);
	calculated = ext4_crc32c(seed, data,
	    block_size - sizeof(u32_t));

	return(stored == calculated ? OK : EIO);
}

int
ext4_verify_dir_checksum(struct inode *rip, const unsigned char *data,
	size_t block_size, u64_t logical_block)
{
	const unsigned char *tail;
	u32_t stored, calculated;
	u32_t seed;

	if (!rip->i_sp->s_metadata_csum)
		return(OK);

	/*
	 * Indexed directory roots use a dx_tail checksum layout rather than
	 * the ordinary 12-byte directory-entry tail.  Do not accept them until
	 * that separate format is implemented and verified.
	 */
	if (logical_block == 0 && (rip->i_flags & EXT2_INDEX_FL))
		return(EOPNOTSUPP);
	if (block_size < EXT4_DIR_ENTRY_TAIL_SIZE)
		return(EIO);

	tail = data + block_size - EXT4_DIR_ENTRY_TAIL_SIZE;
	if (ext4_get_le32(tail) != 0 ||
	    ext4_get_le16(tail + 4) != EXT4_DIR_ENTRY_TAIL_SIZE ||
	    tail[6] != 0 || tail[7] != 0xde)
		return(EIO);

	stored = ext4_get_le32(tail + 8);
	seed = ext4_inode_checksum_seed(rip->i_sp, rip->i_num,
	    (const unsigned char *)rip->i_block);
	calculated = ext4_crc32c(seed, data,
	    block_size - EXT4_DIR_ENTRY_TAIL_SIZE);

	return(stored == calculated ? OK : EIO);
}
