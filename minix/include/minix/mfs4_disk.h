/*	$NetBSD$	*/

#ifndef _MINIX_MFS4_DISK_H
#define _MINIX_MFS4_DISK_H

/*
 * MFS4 on-disk format constants and byte-oriented codecs.
 *
 * These definitions are shared by the MFS server, mkfs and fsck.  On-disk
 * objects are byte arrays; native C structure layout must never be used as
 * the disk representation.
 */
#include <stddef.h>
#include <stdint.h>

#define MFS4_BLOCK_SIZE			4096U
#define MFS4_SUPER_SIZE			512U
#define MFS4_SUPER_PRIMARY_OFFSET	1024U
#define MFS4_INODE_SIZE			128U
#define MFS4_INODES_PER_BLOCK		32U
#define MFS4_DIRENT_SIZE		64U
#define MFS4_NAME_MAX			60U
#define MFS4_POINTERS_PER_BLOCK		1024U
#define MFS4_DIRECT_POINTERS		7U
#define MFS4_INODE_POINTERS		10U
#define MFS4_BITS_PER_BLOCK		(MFS4_BLOCK_SIZE * 8U)
#define MFS4_MAX_FILE_SIZE		(1ULL << 40)
#define MFS4_MAX_BLOCKS			(1ULL << 32)
#define MFS4_MAX_INODES			UINT32_MAX

#define MFS4_JOURNAL_START		2U
#define MFS4_JOURNAL_BLOCKS		66U
#define MFS4_JOURNAL_HEADER_BLOCK	2U
#define MFS4_JOURNAL_IMAGES_START	3U
#define MFS4_JOURNAL_MAX_IMAGES		64U
#define MFS4_JOURNAL_COMMIT_BLOCK	67U
#define MFS4_IMAP_START			68U

#define MFS4_SUPER_MAGIC		"MFS4FS\0\0"
#define MFS4_SUPER_MAGIC_SIZE		8U
#define MFS4_SUPER_MAJOR		4U
#define MFS4_SUPER_MINOR		0U

/* Superblock byte offsets. */
#define MFS4_SB_MAGIC			0U
#define MFS4_SB_MAJOR			8U
#define MFS4_SB_MINOR			10U
#define MFS4_SB_HEADER_SIZE		12U
#define MFS4_SB_BLOCK_SIZE		16U
#define MFS4_SB_INODE_SIZE		20U
#define MFS4_SB_DIRENT_SIZE		22U
#define MFS4_SB_COMPAT_FEATURES		24U
#define MFS4_SB_RO_COMPAT_FEATURES	28U
#define MFS4_SB_INCOMPAT_FEATURES	32U
#define MFS4_SB_VOLUME_FLAGS		36U
#define MFS4_SB_TOTAL_BLOCKS		40U
#define MFS4_SB_TOTAL_INODES		48U
#define MFS4_SB_IMAP_START		56U
#define MFS4_SB_IMAP_BLOCKS		64U
#define MFS4_SB_ZMAP_BLOCKS		68U
#define MFS4_SB_ZMAP_START		72U
#define MFS4_SB_INODE_TABLE_START	80U
#define MFS4_SB_INODE_TABLE_BLOCKS	88U
#define MFS4_SB_STATE_BLOCK		96U
#define MFS4_SB_DATA_START		104U
#define MFS4_SB_JOURNAL_START		112U
#define MFS4_SB_JOURNAL_BLOCKS		120U
#define MFS4_SB_MAX_TX_IMAGES		124U
#define MFS4_SB_BACKUP_BLOCK		128U
#define MFS4_SB_ROOT_INODE		136U
#define MFS4_SB_MAX_FILE_SIZE		144U
#define MFS4_SB_VOLUME_GENERATION	152U
#define MFS4_SB_UUID			160U
#define MFS4_SB_CREATED_AT		176U
#define MFS4_SB_CRC32C			508U

/* Inode byte offsets. */
#define MFS4_I_MODE			0U
#define MFS4_I_FLAGS			2U
#define MFS4_I_NLINK			4U
#define MFS4_I_UID			8U
#define MFS4_I_GID			12U
#define MFS4_I_SIZE			16U
#define MFS4_I_ATIME_SEC		24U
#define MFS4_I_MTIME_SEC		32U
#define MFS4_I_CTIME_SEC		40U
#define MFS4_I_ATIME_NSEC		48U
#define MFS4_I_MTIME_NSEC		52U
#define MFS4_I_CTIME_NSEC		56U
#define MFS4_I_GENERATION		60U
#define MFS4_I_DIRECT			64U
#define MFS4_I_SINGLE_INDIRECT		92U
#define MFS4_I_DOUBLE_INDIRECT		96U
#define MFS4_I_TRIPLE_INDIRECT		100U
#define MFS4_I_XATTR_HEAD		104U
#define MFS4_I_ORPHAN_NEXT		108U
#define MFS4_I_RDEV			112U
#define MFS4_I_PROJECT_ID		120U
#define MFS4_I_RESERVED			124U

/* Filesystem state block byte offsets and flags. */
#define MFS4_STATE_MAGIC		"M4ST"
#define MFS4_STATE_VERSION		1U
#define MFS4_STATE_ORPHAN_HEAD		8U
#define MFS4_STATE_FLAGS		12U
#define MFS4_STATE_GENERATION		16U
#define MFS4_STATE_CRC32C		4092U
#define MFS4_STATE_CLEAN		(1U << 0)

/* Fixed-size directory entry byte offsets. */
#define MFS4_DIRENT_INODE		0U
#define MFS4_DIRENT_NAME		4U

static inline uint16_t
mfs4_get_le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t
mfs4_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t
mfs4_get_le64(const uint8_t *p)
{
	return (uint64_t)mfs4_get_le32(p) |
	    ((uint64_t)mfs4_get_le32(p + 4) << 32);
}

static inline void
mfs4_put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void
mfs4_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline void
mfs4_put_le64(uint8_t *p, uint64_t v)
{
	mfs4_put_le32(p, (uint32_t)v);
	mfs4_put_le32(p + 4, (uint32_t)(v >> 32));
}

/* CRC-32C/Castagnoli, reflected polynomial, init/xorout 0xffffffff. */
static inline uint32_t
mfs4_crc32c(const void *buffer, size_t length)
{
	const uint8_t *p = (const uint8_t *)buffer;
	uint32_t crc = 0xffffffffU;
	size_t i;
	unsigned int bit;

	for (i = 0; i < length; i++) {
		crc ^= p[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^
			    ((crc & 1U) ? 0x82f63b78U : 0U);
	}
	return crc ^ 0xffffffffU;
}

#endif /* _MINIX_MFS4_DISK_H */
