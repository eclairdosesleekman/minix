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

  /* EA inodes are private xattr-value storage and must not be named. */
  if (rip->i_sp->s_is_ext4 && (rip->i_flags & EXT4_EA_INODE_FL)) {
	put_inode(rip);
	err_code = EIO;
	return(NULL);
  }

  return(rip);
}


/*===========================================================================*
 *				search_dir				     *
 *				ldir_ptr: ptr to inode for dir to search
 *				string: component to search for
 *				numb: pointer to inode number
 *				flag: LOOK_UP, ENTER, DELETE or IS_EMPTY
 *				ftype: used when ENTER and INCOMPAT_FILETYPE
 *===========================================================================*/

/*
 * ext4 HTree name hashing and indexed lookup.  Hash versions 0..5 are the
 * legacy, half-MD4, and TEA algorithms with signed/unsigned byte handling.
 * SipHash requires the encryption-key path and is deliberately left to the
 * compatibility linear scan.
 */
#define EXT4_DX_MAX_LEVELS 3
#define EXT4_DX_BLOCK_MASK 0x0fffffffU

struct ext4_dx_frame {
	struct buf *bp;
	u32_t logical;
	unsigned int offset;
	u16_t count;
	u16_t at;
};

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

#define EXT4_DX_F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define EXT4_DX_G(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define EXT4_DX_H(x, y, z) ((x) ^ (y) ^ (z))
#define EXT4_DX_ROUND(f, a, b, c, d, x, s) \
	do { (a) += f((b), (c), (d)) + (x); (a) = ext4_dx_rol32((a), (s)); } while (0)

static void
ext4_dx_half_md4(u32_t state[4], const u32_t input[8])
{
	u32_t a = state[0], b = state[1], c = state[2], d = state[3];

	/* Round 1. */
	EXT4_DX_ROUND(EXT4_DX_F, a, b, c, d, input[0], 3);
	EXT4_DX_ROUND(EXT4_DX_F, d, a, b, c, input[1], 7);
	EXT4_DX_ROUND(EXT4_DX_F, c, d, a, b, input[2], 11);
	EXT4_DX_ROUND(EXT4_DX_F, b, c, d, a, input[3], 19);
	EXT4_DX_ROUND(EXT4_DX_F, a, b, c, d, input[4], 3);
	EXT4_DX_ROUND(EXT4_DX_F, d, a, b, c, input[5], 7);
	EXT4_DX_ROUND(EXT4_DX_F, c, d, a, b, input[6], 11);
	EXT4_DX_ROUND(EXT4_DX_F, b, c, d, a, input[7], 19);

	/* Round 2. */
	EXT4_DX_ROUND(EXT4_DX_G, a, b, c, d, input[1] + 013240474631UL, 3);
	EXT4_DX_ROUND(EXT4_DX_G, d, a, b, c, input[3] + 013240474631UL, 5);
	EXT4_DX_ROUND(EXT4_DX_G, c, d, a, b, input[5] + 013240474631UL, 9);
	EXT4_DX_ROUND(EXT4_DX_G, b, c, d, a, input[7] + 013240474631UL, 13);
	EXT4_DX_ROUND(EXT4_DX_G, a, b, c, d, input[0] + 013240474631UL, 3);
	EXT4_DX_ROUND(EXT4_DX_G, d, a, b, c, input[2] + 013240474631UL, 5);
	EXT4_DX_ROUND(EXT4_DX_G, c, d, a, b, input[4] + 013240474631UL, 9);
	EXT4_DX_ROUND(EXT4_DX_G, b, c, d, a, input[6] + 013240474631UL, 13);

	/* Round 3. */
	EXT4_DX_ROUND(EXT4_DX_H, a, b, c, d, input[3] + 015666365641UL, 3);
	EXT4_DX_ROUND(EXT4_DX_H, d, a, b, c, input[7] + 015666365641UL, 9);
	EXT4_DX_ROUND(EXT4_DX_H, c, d, a, b, input[2] + 015666365641UL, 11);
	EXT4_DX_ROUND(EXT4_DX_H, b, c, d, a, input[6] + 015666365641UL, 15);
	EXT4_DX_ROUND(EXT4_DX_H, a, b, c, d, input[1] + 015666365641UL, 3);
	EXT4_DX_ROUND(EXT4_DX_H, d, a, b, c, input[5] + 015666365641UL, 9);
	EXT4_DX_ROUND(EXT4_DX_H, c, d, a, b, input[0] + 015666365641UL, 11);
	EXT4_DX_ROUND(EXT4_DX_H, b, c, d, a, input[4] + 015666365641UL, 15);

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
}

static void
ext4_dx_tea(u32_t state[4], const u32_t input[4])
{
	u32_t sum = 0, b0 = state[0], b1 = state[1];
	unsigned int n = 16;

	do {
		sum += 0x9e3779b9U;
		b0 += ((b1 << 4) + input[0]) ^ (b1 + sum) ^
		    ((b1 >> 5) + input[1]);
		b1 += ((b0 << 4) + input[2]) ^ (b0 + sum) ^
		    ((b0 >> 5) + input[3]);
	} while (--n != 0);

	state[0] += b0;
	state[1] += b1;
}

static void
ext4_dx_str2hashbuf(const char *name, int len, u32_t *buf, int words,
	int unsigned_bytes)
{
	u32_t pad, value;
	int i;

	pad = (u32_t)len | ((u32_t)len << 8);
	pad |= pad << 16;
	value = pad;
	if (len > words * 4)
		len = words * 4;
	for (i = 0; i < len; i++) {
		int ch;

		if ((i & 3) == 0)
			value = pad;
		ch = unsigned_bytes ?
		    (int)((const unsigned char *)name)[i] :
		    (int)((const signed char *)name)[i];
		value = (u32_t)ch + (value << 8);
		if ((i & 3) == 3) {
			*buf++ = value;
			value = pad;
			words--;
		}
	}
	if (--words >= 0)
		*buf++ = value;
	while (--words >= 0)
		*buf++ = pad;
}

static int
ext4_dx_hash_name(struct super_block *sp, const char *name, int len,
	unsigned int version, u32_t *hashp)
{
	u32_t state[4], input[8], hash, hash0, hash1;
	const char *p;
	unsigned int i;
	int unsigned_bytes;

	if (version > 5 || len < 0)
		return(EOPNOTSUPP);
#if defined(__CHAR_UNSIGNED__)
	if (version <= 2)
		version += 3;
#endif
	state[0] = 0x67452301U;
	state[1] = 0xefcdab89U;
	state[2] = 0x98badcfeU;
	state[3] = 0x10325476U;
	for (i = 0; i < 4; i++) {
		if (sp->s_hash_seed[i] != 0) {
			memcpy(state, sp->s_hash_seed, sizeof(state));
			break;
		}
	}
	unsigned_bytes = version >= 3;
	switch (version) {
	case 0:
	case 3:
		hash0 = 0x12a3fe2dU;
		hash1 = 0x37abe8f9U;
		for (i = 0; i < (unsigned int)len; i++) {
			int ch = unsigned_bytes ?
			    (int)((const unsigned char *)name)[i] :
			    (int)((const signed char *)name)[i];
			hash = hash1 + (hash0 ^ (u32_t)(ch * 7152373));
			if (hash & 0x80000000U)
				hash -= 0x7fffffffU;
			hash1 = hash0;
			hash0 = hash;
		}
		hash = hash0 << 1;
		break;
	case 1:
	case 4:
		p = name;
		while (len > 0) {
			ext4_dx_str2hashbuf(p, len, input, 8, unsigned_bytes);
			ext4_dx_half_md4(state, input);
			len -= 32;
			p += 32;
		}
		hash = state[1];
		break;
	case 2:
	case 5:
		p = name;
		while (len > 0) {
			ext4_dx_str2hashbuf(p, len, input, 4, unsigned_bytes);
			ext4_dx_tea(state, input);
			len -= 16;
			p += 16;
		}
		hash = state[0];
		break;
	default:
		return(EOPNOTSUPP);
	}
	hash &= ~1U;
	if (hash == 0xfffffffeU)
		hash = 0xfffffffcU;
	*hashp = hash;
	return(OK);
}

static u32_t
ext4_dx_entry_hash(const struct ext4_dx_frame *frame, unsigned int index)
{
	const unsigned char *data = (const unsigned char *)b_data(frame->bp);

	if (index == 0)
		return(0);
	return(ext4_dx_read32(data + frame->offset + index * 8));
}

static u32_t
ext4_dx_entry_block(const struct ext4_dx_frame *frame, unsigned int index)
{
	const unsigned char *data = (const unsigned char *)b_data(frame->bp);

	return(ext4_dx_read32(data + frame->offset + index * 8 + 4) &
	    EXT4_DX_BLOCK_MASK);
}

static unsigned int
ext4_dx_choose(const struct ext4_dx_frame *frame, u32_t hash)
{
	unsigned int low = 1, high = frame->count;

	while (low < high) {
		unsigned int middle = low + (high - low) / 2;

		if (ext4_dx_entry_hash(frame, middle) <= hash)
			low = middle + 1;
		else
			high = middle;
	}
	return(low - 1);
}

static int
ext4_dx_load_frame(struct inode *rip, u32_t logical, int root,
	struct ext4_dx_frame *frame)
{
	struct super_block *sp = rip->i_sp;
	const struct ext2_disk_dir_desc *dot, *dotdot, *fake;
	const unsigned char *data;
	u64_t directory_blocks, offset;
	unsigned int limit_offset, max_limit, i;
	u32_t previous_hash = 0;
	if (rip->i_size <= 0 || sp->s_block_size == 0)
		return(EIO);
	directory_blocks = ((u64_t)rip->i_size + sp->s_block_size - 1) /
	    sp->s_block_size;
	if (logical >= directory_blocks)
		return(EIO);
	offset = (u64_t)logical * sp->s_block_size;
	err_code = OK;
	frame->bp = get_block_map(rip, offset);
	if (frame->bp == NULL)
		return(err_code != OK ? err_code : EIO);
	data = (const unsigned char *)b_data(frame->bp);
	if (!ext4_verify_dir_block(rip, data, logical)) {
		put_block(frame->bp);
		frame->bp = NULL;
		return(EIO);
	}
	frame->logical = logical;
	frame->offset = root ? 32 : 8;
	if (root) {
		dot = (const struct ext2_disk_dir_desc *)data;
		dotdot = (const struct ext2_disk_dir_desc *)(data + 12);
		if (ext4_dx_read32(data + 24) != 0 || data[29] != 8 ||
		    data[31] != 0 || dot->d_name_len != 1 ||
		    dot->d_name[0] != '.' || dotdot->d_name_len != 2 ||
		    memcmp(dotdot->d_name, "..", 2) != 0 ||
		    ext4_dx_read32(data) != (u32_t)rip->i_num ||
		    ext4_dx_read32(data + 12) == 0 ||
		    ext4_dx_read32(data + 12) > sp->s_inodes_count) {
			put_block(frame->bp);
			frame->bp = NULL;
			return(EIO);
		}
	} else {
		fake = (const struct ext2_disk_dir_desc *)data;
		if (fake->d_ino != 0 ||
		    ext4_dir_rec_len(fake, sp->s_block_size) != sp->s_block_size ||
		    fake->d_name_len != 0 || fake->d_file_type != 0) {
			put_block(frame->bp);
			frame->bp = NULL;
			return(EIO);
		}
	}
	limit_offset = frame->offset;
	if (sp->s_block_size < limit_offset + 8) {
		put_block(frame->bp);
		frame->bp = NULL;
		return(EIO);
	}
	max_limit = (sp->s_block_size - limit_offset -
	    (HAS_RO_COMPAT_FEATURE(sp, RO_COMPAT_METADATA_CSUM) ? 8 : 0)) / 8;
	frame->count = ext4_dx_read16(data + limit_offset + 2);
	if (ext4_dx_read16(data + limit_offset) != max_limit ||
	    frame->count == 0 || frame->count > max_limit) {
		put_block(frame->bp);
		frame->bp = NULL;
		return(EIO);
	}
	previous_hash = 0;
	for (i = 0; i < frame->count; i++) {
		u32_t child = ext4_dx_read32(data + limit_offset + i * 8 + 4) &
		    EXT4_DX_BLOCK_MASK;
		u32_t hash = i == 0 ? 0 :
		    ext4_dx_read32(data + limit_offset + i * 8);

		if (child == 0 || (u64_t)child >= directory_blocks ||
		    (i != 0 && hash < previous_hash)) {
			put_block(frame->bp);
			frame->bp = NULL;
			return(EIO);
		}
		previous_hash = hash;
	}
	frame->at = 0;
	return(OK);
}

static void
ext4_dx_release_frames(struct ext4_dx_frame *frames, unsigned int depth)
{
	unsigned int i;

	for (i = 0; i <= depth; i++) {
		if (frames[i].bp != NULL) {
			put_block(frames[i].bp);
			frames[i].bp = NULL;
		}
	}
}

static int
ext4_dx_scan_leaf(struct inode *rip, u32_t logical, const char *name,
	int name_len, ino_t *numb)
{
	struct buf *bp;
	struct ext2_disk_dir_desc *dp;
	unsigned char *data;
	unsigned int block_off, rec_len;
	u64_t position;
	ino_t ino;
	int r = ENOENT;

	position = (u64_t)logical * rip->i_sp->s_block_size;
	err_code = OK;
	bp = get_block_map(rip, position);
	if (bp == NULL)
		return(err_code != OK ? err_code : EIO);
	data = (unsigned char *)b_data(bp);
	if (!ext4_verify_dir_block(rip, data, logical)) {
		put_block(bp);
		return(EIO);
	}
	for (block_off = 0; block_off < rip->i_sp->s_block_size;
	    block_off += rec_len) {
		dp = (struct ext2_disk_dir_desc *)(data + block_off);
		if (!ext4_valid_dirent(dp, block_off, rip->i_sp->s_block_size)) {
			r = EIO;
			break;
		}
		rec_len = ext4_dir_rec_len(dp, rip->i_sp->s_block_size);
		if (dp->d_ino != NO_ENTRY && dp->d_name_len == name_len &&
		    memcmp(dp->d_name, name, name_len) == 0) {
			ino = (ino_t)conv4(le_CPU, dp->d_ino);
			if (ino == NO_ENTRY || ino > rip->i_sp->s_inodes_count) {
				r = EIO;
				break;
			}
			if (numb != NULL)
				*numb = ino;
			r = OK;
			break;
		}
	}
	put_block(bp);
	return(r);
}

static int
ext4_dx_lookup(struct inode *rip, const char *name, int name_len, ino_t *numb,
	u32_t *leaf_out)
{
	struct ext4_dx_frame frames[EXT4_DX_MAX_LEVELS + 1];
	const unsigned char *root_data;
	unsigned int depth, i, p;
	u32_t hash, leaf;
	int r;

	memset(frames, 0, sizeof(frames));
	r = ext4_dx_load_frame(rip, 0, TRUE, &frames[0]);
	if (r != OK)
		return(r);
	root_data = (const unsigned char *)b_data(frames[0].bp);
	if (root_data[28] > 5) {
		ext4_dx_release_frames(frames, 0);
		return(EOPNOTSUPP);
	}
	if (root_data[31] != 0) {
		ext4_dx_release_frames(frames, 0);
		return(EIO);
	}
	depth = root_data[30];
	if (depth > EXT4_DX_MAX_LEVELS ||
	    (depth == 3 && !HAS_INCOMPAT_FEATURE(rip->i_sp,
	    INCOMPAT_LARGEDIR))) {
		ext4_dx_release_frames(frames, 0);
		return(EIO);
	}
	r = ext4_dx_hash_name(rip->i_sp, name, name_len, root_data[28], &hash);
	if (r != OK) {
		ext4_dx_release_frames(frames, 0);
		return(r);
	}

	for (i = 0; i <= depth; i++) {
		frames[i].at = (u16_t)ext4_dx_choose(&frames[i], hash);
		leaf = ext4_dx_entry_block(&frames[i], frames[i].at);
		if (i < depth) {
			unsigned int j;

			for (j = 0; j <= i; j++) {
				if (frames[j].logical == leaf) {
					ext4_dx_release_frames(frames, i);
					return(EIO);
				}
			}
			r = ext4_dx_load_frame(rip, leaf, FALSE, &frames[i + 1]);
			if (r != OK) {
				ext4_dx_release_frames(frames, i);
				return(r);
			}
		} else {
			unsigned int j;

			for (j = 0; j <= depth; j++) {
				if (frames[j].logical == leaf) {
					ext4_dx_release_frames(frames, depth);
					return(EIO);
				}
			}
		}
	}

	/* Keep the hash-selected leaf for insertion even when the name is absent. */
	if (leaf_out != NULL)
		*leaf_out = leaf;
	for (;;) {
		r = ext4_dx_scan_leaf(rip, leaf, name, name_len, numb);
		if (r != ENOENT) {
			if (r == OK && leaf_out != NULL)
				*leaf_out = leaf;
			break;
		}

		/* Advance through the index stack to the next collision leaf. */
		for (p = depth + 1; p > 0; p--) {
			unsigned int level = p - 1;

			if ((unsigned int)frames[level].at + 1 < frames[level].count) {
				frames[level].at++;
				break;
			}
		}
		if (p == 0)
			break;
		{
			unsigned int level = p - 1;
			u32_t next_hash = ext4_dx_entry_hash(&frames[level],
			    frames[level].at);

			if ((hash & 1U) == 0 && (next_hash & ~1U) != hash)
				break;
			for (i = level + 1; i <= depth; i++) {
				if (frames[i].bp != NULL) {
					put_block(frames[i].bp);
					frames[i].bp = NULL;
				}
			}
			for (i = level + 1; i <= depth; i++) {
				unsigned int j;
				u32_t child = ext4_dx_entry_block(&frames[i - 1],
				    frames[i - 1].at);

				for (j = 0; j < i; j++) {
					if (frames[j].logical == child) {
						ext4_dx_release_frames(frames, depth);
						return(EIO);
					}
				}
				r = ext4_dx_load_frame(rip, child, FALSE, &frames[i]);
				if (r != OK) {
					ext4_dx_release_frames(frames, depth);
					return(r);
				}
				frames[i].at = 0;
			}
			leaf = ext4_dx_entry_block(&frames[depth],
			    frames[depth].at);
			for (i = 0; i <= depth; i++) {
				if (frames[i].logical == leaf) {
					ext4_dx_release_frames(frames, depth);
					return(EIO);
				}
			}
		}
	}
	ext4_dx_release_frames(frames, depth);
	return(r);
}

#undef EXT4_DX_F
#undef EXT4_DX_G
#undef EXT4_DX_H
#undef EXT4_DX_ROUND


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
  int indexed_leaf = FALSE;
  u32_t dx_leaf = 0;
  off_t pos_limit;
  unsigned int block_size, block_off, rec_len, entry_rec_len;

  /* If 'ldir_ptr' is not a pointer to a dir inode, error. */
  if ( (ldir_ptr->i_mode & I_TYPE) != I_DIRECTORY)  {
	return(ENOTDIR);
  }

  /*
   * HTree indexes are immutable in this implementation: updates are limited
   * to existing leaves and never split, merge, or append directory blocks.
   * The selected leaf is found through the same validated hash path as lookup.
   */
  block_size = ldir_ptr->i_sp->s_block_size;
  pos_limit = ldir_ptr->i_size;
  new_slots = 0;
  e_hit = FALSE;
  match = 0;    	/* set when a string match occurs */
  pos = 0;

  if ((string_len = strlen(string)) > EXT2_NAME_MAX)
	return(ENAMETOOLONG);

  if (flag == LOOK_UP && (ldir_ptr->i_flags & EXT2_INDEX_FL) &&
      strcmp(string, ".") != 0 && strcmp(string, "..") != 0) {
	r = ext4_dx_lookup(ldir_ptr, string, string_len, numb, NULL);
	if (r != EOPNOTSUPP)
		return(r);
  }

  if ((ldir_ptr->i_flags & EXT2_INDEX_FL) && flag == ENTER) {
	/* Dot entries share the HTree root block and are not ordinary leaves. */
	if (strcmp(string, ".") == 0 || strcmp(string, "..") == 0)
		return(EOPNOTSUPP);
	r = ext4_dx_lookup(ldir_ptr, string, string_len, NULL, &dx_leaf);
	if (r == OK)
		return(EEXIST);
	if (r != ENOENT)
		return(r);
	pos = (off_t)dx_leaf * block_size;
	pos_limit = pos + block_size;
	indexed_leaf = TRUE;
  } else if ((ldir_ptr->i_flags & EXT2_INDEX_FL) && flag == DELETE &&
      strcmp(string, ".") != 0 && strcmp(string, "..") != 0) {
	/* Deletion must locate the exact collision leaf containing the name. */
	r = ext4_dx_lookup(ldir_ptr, string, string_len, NULL, &dx_leaf);
	if (r == EOPNOTSUPP) {
		/* SipHash has no key path here; a validated linear scan is safe. */
	} else if (r != OK) {
		return(r);
	} else {
		pos = (off_t)dx_leaf * block_size;
		pos_limit = pos + block_size;
		indexed_leaf = TRUE;
	}
  }

  if (flag == ENTER) {
	required_space = MIN_DIR_ENTRY_SIZE + string_len;
	required_space += (required_space & 0x03) == 0 ? 0 :
			     (DIR_ENTRY_ALIGN - (required_space & 0x03) );

	if (!indexed_leaf && ldir_ptr->i_last_dpos < ldir_ptr->i_size &&
	    ldir_ptr->i_last_dentry_size <= required_space)
		pos = ldir_ptr->i_last_dpos;
  }

  for (; pos < pos_limit; pos += block_size) {
	/* Since directories don't have holes, 'b' cannot be NO_BLOCK. */
	if(!(bp = get_block_map(ldir_ptr, rounddown(pos, block_size))))
		return(err_code != OK ? err_code : EIO);

	if (!ext4_verify_dir_block(ldir_ptr,
	    (const unsigned char *)b_data(bp),
	    (unsigned int)(pos / block_size))) {
		put_block(bp);
		return(EIO);
	}

	prev_dp = NULL; /* New block - new first dentry, so no prev. */

	/*
	 * Search the block using the decoded record length. Never use the
	 * on-disk 16-bit value for pointer arithmetic: 0xffff represents a
	 * complete 64 KiB directory block.
	 */
	for (block_off = 0, dp = (struct ext2_disk_dir_desc*) &b_data(bp);
	     block_off < block_size;) {
		/* Validate the record before touching its name or advancing. */
		if (!ext4_valid_dirent(dp, block_off, block_size)) {
			put_block(bp);
			return(EIO);
		}

		rec_len = ext4_dir_rec_len(dp, block_size);

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
				lmfs_markdirty(bp);

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
			lmfs_markdirty(bp);
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

  /* A full indexed leaf cannot be extended without updating its HTree. */
  if (indexed_leaf)
	return(EOPNOTSUPP);

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
	ext4_set_dir_rec_len(dp, block_size, block_size);
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
  lmfs_markdirty(bp);
  put_block(bp);
  ldir_ptr->i_update |= CTIME | MTIME;	/* mark mtime for update later */
  ldir_ptr->i_dirt = IN_DIRTY;

  if (new_slots == 1) {
	ldir_ptr->i_size += (off_t) entry_rec_len;
	/* Send the change to disk if the directory is extended. */
	if (extended) rw_inode(ldir_ptr, WRITING);
  }
  return(OK);

}
