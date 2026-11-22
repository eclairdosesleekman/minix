/* This file contains the procedures that look up path names in the directory
 * system and determine the inode number that goes with a given path name.
 *
 * Created (MFS based):
 *   February 2010 (Evgeniy Ivanov)
 */

#include "fs.h"
#include <assert.h>
#include <limits.h>
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
 * Populate a slot wholly contained in one inline-directory dirent area.
 * Preserve any useful tail as a separate unused record.
 */
static void
ext4_fill_inline_dirent(struct inode *dir,
	struct ext2_disk_dir_desc *dp, unsigned int slot_len,
	unsigned int area_len, const char *string, unsigned int name_len,
	ino_t child, int ftype)
{
	unsigned int entry_len, unused_len;
	struct ext2_disk_dir_desc *unused;

	entry_len = MIN_DIR_ENTRY_SIZE + name_len;
	entry_len += (DIR_ENTRY_ALIGN -
	    (entry_len & (DIR_ENTRY_ALIGN - 1))) &
	    (DIR_ENTRY_ALIGN - 1);
	unused_len = slot_len - entry_len;
	if (unused_len < MIN_DIR_ENTRY_SIZE) {
		entry_len = slot_len;
		unused_len = 0;
	}

	memset(dp, 0, slot_len);
	ext4_set_dir_rec_len(dp, entry_len, area_len);
	dp->d_ino = (u32_t)conv4(le_CPU, child);
	dp->d_name_len = (u8_t)name_len;
	if (HAS_INCOMPAT_FEATURE(dir->i_sp, INCOMPAT_FILETYPE)) {
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
	memcpy(dp->d_name, string, name_len);

	if (unused_len != 0) {
		unused = (struct ext2_disk_dir_desc *)
		    ((unsigned char *)dp + entry_len);
		memset(unused, 0, unused_len);
		unused->d_ino = NO_ENTRY;
		ext4_set_dir_rec_len(unused, unused_len, area_len);
	}
}


/*
 * Search the two independent dirent areas of an ext4 inline directory.
 * "." and ".." are implicit: only the parent inode number is stored in
 * i_block[0..4]. Insertions and deletions are bounded to each existing area;
 * neither operation changes the inline payload or xattr value length.
 */
static int
ext4_search_inline_dir(struct inode *dir, const char *string, ino_t *numb,
	int flag, int ftype)
{
	unsigned char *areas[2];
	unsigned int lengths[2], area, offset, rec_len, name_len;
	unsigned int previous_len, insert_name_len, required_space;
	unsigned int actual_size, slot_len;
	struct ext2_disk_dir_desc *dp, *previous, *slot;
	ino_t child;

	insert_name_len = 0;
	required_space = 0;
	if (flag == ENTER) {
		if (string == NULL || numb == NULL || *numb == NO_ENTRY ||
		    *numb > dir->i_sp->s_inodes_count)
			return(EINVAL);
		insert_name_len = strlen(string);
		if (insert_name_len == 0)
			return(ENOENT);
		if (insert_name_len > EXT2_NAME_MAX ||
		    insert_name_len > NAME_MAX)
			return(ENAMETOOLONG);
		if (strcmp(string, ".") == 0 || strcmp(string, "..") == 0)
			return(EEXIST);
		required_space = MIN_DIR_ENTRY_SIZE + insert_name_len;
		required_space += (DIR_ENTRY_ALIGN -
		    (required_space & (DIR_ENTRY_ALIGN - 1))) &
		    (DIR_ENTRY_ALIGN - 1);
	}
	if (flag == DELETE && (string == NULL ||
	    strcmp(string, ".") == 0 || strcmp(string, "..") == 0))
		return(EOPNOTSUPP);
	if (dir->i_inline_size < EXT4_INLINE_DATA_MIN ||
	    dir->i_inline_size > EXT4_INLINE_DATA_TOTAL_MAX ||
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

	for (area = 0; area < 2; area++) {
		if (lengths[area] == 0)
			continue;
		if (lengths[area] < MIN_DIR_ENTRY_SIZE ||
		    (lengths[area] & (DIR_ENTRY_ALIGN - 1)) != 0)
			return(EIO);
		previous = NULL;
		for (offset = 0; offset < lengths[area]; offset += rec_len) {
			dp = (struct ext2_disk_dir_desc *)(areas[area] + offset);
			if (!ext4_valid_dirent(dp, offset, lengths[area]))
				return(EIO);
			rec_len = ext4_dir_rec_len(dp, lengths[area]);
			if (rec_len > lengths[area] - offset)
				return(EIO);
			if (dp->d_ino == NO_ENTRY) {
				if (flag == ENTER && rec_len >= required_space) {
					ext4_fill_inline_dirent(dir, dp, rec_len,
					    lengths[area], string, insert_name_len,
					    *numb, ftype);
					dir->i_update |= CTIME | MTIME;
					dir->i_dirt = IN_DIRTY;
					return(OK);
				}
				previous = dp;
				continue;
			}

			name_len = dp->d_name_len;
			if (name_len == 0 || name_len > EXT2_NAME_MAX ||
			    name_len > NAME_MAX)
				return(EIO);
			if ((name_len == 1 && dp->d_name[0] == '.') ||
			    (name_len == 2 && dp->d_name[0] == '.' &&
			    dp->d_name[1] == '.'))
				return(EIO);

			child = (ino_t)conv4(le_CPU, dp->d_ino);
			if (child == NO_ENTRY ||
			    child > dir->i_sp->s_inodes_count)
				return(EIO);

			if (flag == ENTER &&
			    ansi_strcmp(dp->d_name, string, name_len) == 0)
				return(EEXIST);

			if ((flag == LOOK_UP || flag == DELETE) &&
			    ansi_strcmp(dp->d_name, string, name_len) == 0) {
				if (flag == LOOK_UP) {
					*numb = child;
					return(OK);
				}

				/*
				 * Inline directory areas are separate ext4 dirent
				 * blocks. Never merge a record across their boundary.
				 * A first record becomes an unused slot; otherwise
				 * extend the immediately preceding record.
				 */
				dp->d_ino = NO_ENTRY;
				if (previous != NULL) {
					previous_len = ext4_dir_rec_len(previous,
					    lengths[area]);
					if (previous_len > lengths[area] - rec_len) {
						/* Restore the live entry on malformed layout. */
						dp->d_ino = conv4(le_CPU, child);
						return(EIO);
					}
					ext4_set_dir_rec_len(previous,
					    previous_len + rec_len, lengths[area]);
				}
				dir->i_update |= CTIME | MTIME;
				dir->i_dirt = IN_DIRTY;
				return(OK);
			}
			if (flag == IS_EMPTY)
				return(ENOTEMPTY);

			if (flag == ENTER) {
				actual_size = DIR_ENTRY_ACTUAL_SIZE(dp);
				if (actual_size > rec_len)
					return(EIO);
				if (required_space <= rec_len - actual_size) {
					slot = (struct ext2_disk_dir_desc *)
					    ((unsigned char *)dp + actual_size);
					slot_len = rec_len - actual_size;
					ext4_set_dir_rec_len(dp, actual_size,
					    lengths[area]);
					ext4_fill_inline_dirent(dir, slot, slot_len,
					    lengths[area], string, insert_name_len,
					    *numb, ftype);
					dir->i_update |= CTIME | MTIME;
					dir->i_dirt = IN_DIRTY;
					return(OK);
				}
			}
			previous = dp;
		}
	}

	return(flag == IS_EMPTY ? OK :
	    (flag == ENTER ? ENOSPC : ENOENT));
}


/* ext4 HTree hash versions stored in dx_root_info.hash_version. */
#define EXT4_DX_HASH_LEGACY          0
#define EXT4_DX_HASH_HALF_MD4        1
#define EXT4_DX_HASH_TEA             2
#define EXT4_DX_HASH_LEGACY_UNSIGNED 3
#define EXT4_DX_HASH_HALF_MD4_UNSIGNED 4
#define EXT4_DX_HASH_TEA_UNSIGNED    5

static u16_t
ext4_dx_read16(const unsigned char *p)
{
	u16_t value;

	memcpy(&value, p, sizeof(value));
	return((u16_t)conv2(le_CPU, value));
}

static u32_t
ext4_dx_read32(const unsigned char *p)
{
	u32_t value;

	memcpy(&value, p, sizeof(value));
	return((u32_t)conv4(le_CPU, value));
}

static u32_t
ext4_dx_rol32(u32_t value, unsigned int shift)
{
	return((value << shift) | (value >> (32 - shift)));
}

static u32_t
ext4_dx_legacy_hash(const char *name, unsigned int len, int use_unsigned)
{
	u32_t hash0 = 0x12a3fe2dU, hash1 = 0x37abe8f9U;
	u32_t hash;
	int ch;

	while (len-- != 0) {
		ch = use_unsigned ? (unsigned char)*name :
		    (signed char)*name;
		name++;
		hash = hash1 + (hash0 ^ (u32_t)(ch * 7152373));
		if (hash & 0x80000000U)
			hash -= 0x7fffffffU;
		hash1 = hash0;
		hash0 = hash;
	}
	return(hash0 << 1);
}

/*
 * Convert up to num * 4 name bytes into the big-endian words used by ext4's
 * half-MD4 and TEA directory hashes. Signed and unsigned hash variants
 * differ only in how bytes with their high bit set are extended.
 */
static void
ext4_dx_str2hashbuf(const char *name, int len, u32_t *buf, int num,
	int use_unsigned)
{
	u32_t pad, value, ch;
	int i;

	pad = (u32_t)len | ((u32_t)len << 8);
	pad |= pad << 16;
	if (len > num * 4)
		len = num * 4;

	while (len >= 4) {
		if (use_unsigned) {
			value = ((u32_t)(unsigned char)name[0] << 24) +
			    ((u32_t)(unsigned char)name[1] << 16) +
			    ((u32_t)(unsigned char)name[2] << 8) +
			    (u32_t)(unsigned char)name[3];
		} else {
			value = ((u32_t)(long)(signed char)name[0] << 24) +
			    ((u32_t)(long)(signed char)name[1] << 16) +
			    ((u32_t)(long)(signed char)name[2] << 8) +
			    (u32_t)(long)(signed char)name[3];
		}
		*buf++ = value;
		name += 4;
		len -= 4;
		num--;
	}

	value = pad;
	for (i = 0; i < len; i++) {
		ch = use_unsigned ? (u32_t)(unsigned char)name[i] :
		    (u32_t)(long)(signed char)name[i];
		value = ch + (value << 8);
	}
	if (--num >= 0)
		*buf++ = value;
	while (--num >= 0)
		*buf++ = pad;
}

#define EXT4_DX_F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define EXT4_DX_G(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define EXT4_DX_H(x, y, z) ((x) ^ (y) ^ (z))
#define EXT4_DX_ROUND(f, a, b, c, d, x, shift) do { \
	(a) += f((b), (c), (d)) + (x); \
	(a) = ext4_dx_rol32((a), (shift)); \
} while (0)

static void
ext4_dx_half_md4_transform(u32_t buf[4], const u32_t in[8])
{
	u32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];

	EXT4_DX_ROUND(EXT4_DX_F, a, b, c, d, in[0], 3);
	EXT4_DX_ROUND(EXT4_DX_F, d, a, b, c, in[1], 7);
	EXT4_DX_ROUND(EXT4_DX_F, c, d, a, b, in[2], 11);
	EXT4_DX_ROUND(EXT4_DX_F, b, c, d, a, in[3], 19);
	EXT4_DX_ROUND(EXT4_DX_F, a, b, c, d, in[4], 3);
	EXT4_DX_ROUND(EXT4_DX_F, d, a, b, c, in[5], 7);
	EXT4_DX_ROUND(EXT4_DX_F, c, d, a, b, in[6], 11);
	EXT4_DX_ROUND(EXT4_DX_F, b, c, d, a, in[7], 19);

	EXT4_DX_ROUND(EXT4_DX_G, a, b, c, d, in[1] + 0x5a827999U, 3);
	EXT4_DX_ROUND(EXT4_DX_G, d, a, b, c, in[3] + 0x5a827999U, 5);
	EXT4_DX_ROUND(EXT4_DX_G, c, d, a, b, in[5] + 0x5a827999U, 9);
	EXT4_DX_ROUND(EXT4_DX_G, b, c, d, a, in[7] + 0x5a827999U, 13);
	EXT4_DX_ROUND(EXT4_DX_G, a, b, c, d, in[0] + 0x5a827999U, 3);
	EXT4_DX_ROUND(EXT4_DX_G, d, a, b, c, in[2] + 0x5a827999U, 5);
	EXT4_DX_ROUND(EXT4_DX_G, c, d, a, b, in[4] + 0x5a827999U, 9);
	EXT4_DX_ROUND(EXT4_DX_G, b, c, d, a, in[6] + 0x5a827999U, 13);

	EXT4_DX_ROUND(EXT4_DX_H, a, b, c, d, in[3] + 0x6ed9eba1U, 3);
	EXT4_DX_ROUND(EXT4_DX_H, d, a, b, c, in[7] + 0x6ed9eba1U, 9);
	EXT4_DX_ROUND(EXT4_DX_H, c, d, a, b, in[2] + 0x6ed9eba1U, 11);
	EXT4_DX_ROUND(EXT4_DX_H, b, c, d, a, in[6] + 0x6ed9eba1U, 15);
	EXT4_DX_ROUND(EXT4_DX_H, a, b, c, d, in[1] + 0x6ed9eba1U, 3);
	EXT4_DX_ROUND(EXT4_DX_H, d, a, b, c, in[5] + 0x6ed9eba1U, 9);
	EXT4_DX_ROUND(EXT4_DX_H, c, d, a, b, in[0] + 0x6ed9eba1U, 11);
	EXT4_DX_ROUND(EXT4_DX_H, b, c, d, a, in[4] + 0x6ed9eba1U, 15);

	buf[0] += a;
	buf[1] += b;
	buf[2] += c;
	buf[3] += d;
}

static void
ext4_dx_tea_transform(u32_t buf[4], const u32_t in[4])
{
	u32_t sum = 0, b0 = buf[0], b1 = buf[1];
	int rounds = 16;

	do {
		sum += 0x9e3779b9U;
		b0 += ((b1 << 4) + in[0]) ^ (b1 + sum) ^
		    ((b1 >> 5) + in[1]);
		b1 += ((b0 << 4) + in[2]) ^ (b0 + sum) ^
		    ((b0 >> 5) + in[3]);
	} while (--rounds != 0);
	buf[0] += b0;
	buf[1] += b1;
}

#undef EXT4_DX_ROUND
#undef EXT4_DX_F
#undef EXT4_DX_G
#undef EXT4_DX_H

/*
 * Calculate the 32-bit major HTree hash. Encrypted/casefolded directories
 * and the newer SipHash encoding are not supported by this server; those
 * directories continue to use the established linear lookup path.
 */
static int
ext4_dx_name_hash(struct inode *dir, const char *name, unsigned int len,
	u8_t version, u32_t *hashp)
{
	u32_t seed[4], input[8], hash;
	int i, use_unsigned;

	if (len == 0 || len > EXT2_NAME_MAX)
		return(EINVAL);
	use_unsigned = FALSE;
	/* Match Linux ext4's architecture-dependent signed-char hash mode. */
	if (CHAR_MIN == 0 && version <= EXT4_DX_HASH_TEA)
		version += 3;
	switch (version) {
	case EXT4_DX_HASH_LEGACY:
		break;
	case EXT4_DX_HASH_HALF_MD4:
	case EXT4_DX_HASH_TEA:
		break;
	case EXT4_DX_HASH_LEGACY_UNSIGNED:
	case EXT4_DX_HASH_HALF_MD4_UNSIGNED:
	case EXT4_DX_HASH_TEA_UNSIGNED:
		use_unsigned = TRUE;
		break;
	case 6: /* SipHash needs hash-in-dirent support not implemented here. */
		return(EOPNOTSUPP);
	default:
		return(EIO);
	}

	seed[0] = 0x67452301U;
	seed[1] = 0xefcdab89U;
	seed[2] = 0x98badcfeU;
	seed[3] = 0x10325476U;
	for (i = 0; i < 4; i++) {
		u32_t v = (u32_t)conv4(le_CPU, dir->i_sp->s_hash_seed[i]);
		if (v != 0) {
			seed[0] = (u32_t)conv4(le_CPU,
			    dir->i_sp->s_hash_seed[0]);
			seed[1] = (u32_t)conv4(le_CPU,
			    dir->i_sp->s_hash_seed[1]);
			seed[2] = (u32_t)conv4(le_CPU,
			    dir->i_sp->s_hash_seed[2]);
			seed[3] = (u32_t)conv4(le_CPU,
			    dir->i_sp->s_hash_seed[3]);
			break;
		}
	}

	switch (version) {
	case EXT4_DX_HASH_LEGACY:
	case EXT4_DX_HASH_LEGACY_UNSIGNED:
		hash = ext4_dx_legacy_hash(name, len, use_unsigned);
		break;
	case EXT4_DX_HASH_HALF_MD4:
	case EXT4_DX_HASH_HALF_MD4_UNSIGNED:
		while (len > 0) {
			ext4_dx_str2hashbuf(name, (int)len, input, 8,
			    use_unsigned);
			ext4_dx_half_md4_transform(seed, input);
			if (len > 32)
				len -= 32;
			else
				len = 0;
			name += 32;
		}
		hash = seed[1];
		break;
	case EXT4_DX_HASH_TEA:
	case EXT4_DX_HASH_TEA_UNSIGNED:
		while (len > 0) {
			ext4_dx_str2hashbuf(name, (int)len, input, 4,
			    use_unsigned);
			ext4_dx_tea_transform(seed, input);
			if (len > 16)
				len -= 16;
			else
				len = 0;
			name += 16;
		}
		hash = seed[0];
		break;
	default:
		return(EIO);
	}

	hash &= ~1U;
	if (hash == 0xfffffffeU)
		hash = 0xfffffffcU;
	*hashp = hash;
	return(OK);
}

/*
 * Validate a dx count/limit array and choose the last entry whose hash does
 * not exceed the target. Entry zero has an implicit hash of zero; its first
 * word is the count/limit overlay and its second word is the logical block.
 */
static int
ext4_dx_choose_block(struct inode *dir, const unsigned char *data,
	unsigned int entries_offset, u32_t hash, u64_t dir_blocks,
	u32_t *blockp)
{
	unsigned int block_size, limit, count, expected_limit, i;
	u32_t previous_hash = 0, key, raw_block, block;
	const unsigned char *entry;
	int selected = FALSE;

	block_size = dir->i_sp->s_block_size;
	if (entries_offset > block_size ||
	    block_size - entries_offset < 8)
		return(EIO);
	expected_limit = (block_size - entries_offset -
	    (HAS_RO_COMPAT_FEATURE(dir->i_sp, RO_COMPAT_METADATA_CSUM) ?
	    8 : 0)) / 8;
	limit = ext4_dx_read16(data + entries_offset);
	count = ext4_dx_read16(data + entries_offset + 2);
	if (limit != expected_limit || count == 0 || count > limit)
		return(EIO);

	for (i = 0; i < count; i++) {
		entry = data + entries_offset + i * 8;
		if (i == 0) {
			key = 0;
			raw_block = ext4_dx_read32(entry + 4);
		} else {
			key = ext4_dx_read32(entry);
			raw_block = ext4_dx_read32(entry + 4);
			if (key < previous_hash)
				return(EIO);
		}
		previous_hash = key;
		if ((raw_block & 0xf0000000U) != 0)
			return(EIO);
		block = raw_block & 0x0fffffffU;
		if (block == 0 || (u64_t)block >= dir_blocks)
			return(EIO);
		if (key <= hash) {
			*blockp = block;
			selected = TRUE;
		}
	}
	return(selected ? OK : EIO);
}

/*
 * Use the read-only ext4 HTree to select the first leaf for a name. The
 * returned leaf is only a fast-path candidate: a miss falls back to the
 * complete linear scan below, which also handles equal-hash continuation
 * leaves without risking false ENOENT results.
 */
static int
ext4_htree_search_block(struct inode *dir, const char *name, unsigned int len,
	ino_t *numb)
{
	struct buf *bp = NULL;
	const unsigned char *data;
	const struct ext2_disk_dir_desc *dot, *dotdot, *fake;
	u64_t dir_blocks;
	u32_t hash, child, path_blocks[4];
	u32_t parent_ino;
	unsigned int depth, level, entries_offset, block_size, block_off;
	unsigned int rec_len, block_limit;
	struct ext2_disk_dir_desc *dp;
	ino_t child_ino;
	int r;

	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return(ENOENT);
	if (dir->i_size <= 0)
		return(EIO);
	block_size = dir->i_sp->s_block_size;
	dir_blocks = ((u64_t)dir->i_size + block_size - 1) / block_size;
	if (dir_blocks < 2)
		return(EIO);

	bp = get_block_map(dir, 0);
	if (bp == NULL)
		return(err_code != OK ? err_code : EIO);
	data = (const unsigned char *)b_data(bp);
	if (!ext4_verify_dir_block(dir, data, 0)) {
		put_block(bp);
		return(EIO);
	}
	dot = (const struct ext2_disk_dir_desc *)data;
	dotdot = (const struct ext2_disk_dir_desc *)(data + 12);
	if (!ext4_valid_dirent(dot, 0, block_size) ||
	    !ext4_valid_dirent(dotdot, 12, block_size) ||
	    conv4(le_CPU, dot->d_ino) != dir->i_num ||
	    dot->d_name_len != 1 || dot->d_name[0] != '.' ||
	    dotdot->d_name_len != 2 || dotdot->d_name[0] != '.' ||
	    dotdot->d_name[1] != '.' ||
	    (parent_ino = (u32_t)conv4(le_CPU, dotdot->d_ino)) == 0 ||
	    parent_ino > dir->i_sp->s_inodes_count) {
		put_block(bp);
		return(EIO);
	}

	if (ext4_dx_read32(data + 24) != 0 || data[29] != 8 ||
	    data[31] != 0) {
		put_block(bp);
		return(EOPNOTSUPP);
	}
	depth = data[30];
	if (depth > (HAS_INCOMPAT_FEATURE(dir->i_sp, INCOMPAT_LARGEDIR) ?
	    3U : 2U)) {
		put_block(bp);
		return(EIO);
	}
	r = ext4_dx_name_hash(dir, name, len, data[28], &hash);
	if (r != OK) {
		put_block(bp);
		return(r);
	}

	path_blocks[0] = 0;
	for (level = 0; level <= depth; level++) {
		entries_offset = level == 0 ? 32 : 8;
		if (level != 0) {
			fake = (const struct ext2_disk_dir_desc *)data;
			if (conv4(le_CPU, fake->d_ino) != 0 ||
			    ext4_dir_rec_len(fake, block_size) != block_size ||
			    fake->d_name_len != 0 || fake->d_file_type != 0 ||
			    !ext4_verify_dir_block(dir, data, path_blocks[level])) {
				put_block(bp);
				return(EIO);
			}
		}

		r = ext4_dx_choose_block(dir, data, entries_offset, hash,
		    dir_blocks, &child);
		if (r != OK) {
			put_block(bp);
			return(r);
		}
		{
			unsigned int j;
			for (j = 0; j <= level; j++) {
				if (path_blocks[j] == child) {
					put_block(bp);
					return(EIO);
				}
			}
		}

		if (level == depth)
			break;
		path_blocks[level + 1] = child;
		put_block(bp);
		bp = get_block_map(dir, (u64_t)child * block_size);
		if (bp == NULL)
			return(err_code != OK ? err_code : EIO);
		data = (const unsigned char *)b_data(bp);
	}
	if (child == 0 || (u64_t)child >= dir_blocks) {
		put_block(bp);
		return(EIO);
	}
	put_block(bp);
	bp = get_block_map(dir, (u64_t)child * block_size);
	if (bp == NULL)
		return(err_code != OK ? err_code : EIO);
	data = (const unsigned char *)b_data(bp);
	if (!ext4_verify_dir_block(dir, data, child)) {
		put_block(bp);
		return(EIO);
	}

	block_limit = block_size;
	for (block_off = 0; block_off < block_limit;) {
		dp = (struct ext2_disk_dir_desc *)(b_data(bp) + block_off);
		if (!ext4_valid_dirent(dp, block_off, block_size)) {
			put_block(bp);
			return(EIO);
		}
		rec_len = ext4_dir_rec_len(dp, block_size);
		if (rec_len > block_limit - block_off) {
			put_block(bp);
			return(EIO);
		}
		if (dp->d_ino != NO_ENTRY) {
			child_ino = (ino_t)conv4(le_CPU, dp->d_ino);
			if (child_ino == NO_ENTRY ||
			    child_ino > dir->i_sp->s_inodes_count) {
				put_block(bp);
				return(EIO);
			}
			if (ansi_strcmp(dp->d_name, name,
			    dp->d_name_len) == 0) {
				*numb = child_ino;
				put_block(bp);
				return(OK);
			}
		}
		block_off += rec_len;
	}
	put_block(bp);
	return(ENOENT);
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

  /*
   * Inserting into an indexed directory requires an HTree update, which is
   * not implemented. Deleting a live leaf entry does not change the index,
   * but dot entries and the dx root metadata must remain untouched.
   */
  if (ldir_ptr->i_sp->s_is_ext4 &&
      (ldir_ptr->i_flags & EXT2_INDEX_FL) &&
      (flag == ENTER || (flag == DELETE &&
      (strcmp(string, ".") == 0 || strcmp(string, "..") == 0))))
	return(EOPNOTSUPP);

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
	return(ext4_search_inline_dir(ldir_ptr, string, numb, flag, ftype));

  /*
   * Indexed ext4 directories use their HTree to find the likely leaf first.
   * On a valid index miss, keep the linear fallback for hash-collision
   * continuation leaves and for names not covered by the index candidate.
   */
  if (flag == LOOK_UP && ldir_ptr->i_sp->s_is_ext4 &&
      (ldir_ptr->i_flags & EXT2_INDEX_FL)) {
	r = ext4_htree_search_block(ldir_ptr, string,
	    (unsigned int)string_len, numb);
	if (r == OK)
		return(OK);
	if (r != ENOENT && r != EOPNOTSUPP)
		return(r);
  }

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
				    (unsigned char *)b_data(bp),
				    (u64_t)(pos / block_size));
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

		/* The indexed root's dx info follows ".." in block zero. */
		if (flag != ENTER && pos == 0 &&
		    (ldir_ptr->i_flags & EXT2_INDEX_FL) &&
		    dp->d_name_len == 2 && dp->d_name[0] == '.' &&
		    dp->d_name[1] == '.')
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
      (unsigned char *)b_data(bp), (u64_t)(pos / block_size));
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
