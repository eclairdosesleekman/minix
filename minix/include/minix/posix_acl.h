/*
 * Portable POSIX ACL v2 disk codec shared by MFS and VFS.
 *
 * The byte stream is deliberately independent of native structure packing:
 * little-endian version/count followed by tag/perm/id 8-byte records.
 */
#ifndef _MINIX_POSIX_ACL_H
#define _MINIX_POSIX_ACL_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#define MINIX_ACL_VERSION          2U
#define MINIX_ACL_HEADER_SIZE      8U
#define MINIX_ACL_ENTRY_SIZE       8U
#define MINIX_ACL_MAX_VALUE         3801U
#define MINIX_ACL_PERM_MASK        0x0007U
#define MINIX_ACL_UNDEFINED_ID     UINT32_C(0xffffffff)

#define MINIX_ACL_USER_OBJ         0x0001U
#define MINIX_ACL_USER             0x0002U
#define MINIX_ACL_GROUP_OBJ        0x0004U
#define MINIX_ACL_GROUP            0x0008U
#define MINIX_ACL_MASK             0x0010U
#define MINIX_ACL_OTHER            0x0020U

#define MINIX_ACL_READ             0x0004U
#define MINIX_ACL_WRITE            0x0002U
#define MINIX_ACL_EXECUTE          0x0001U

static inline uint16_t
minix_acl_get_le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t
minix_acl_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void
minix_acl_put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void
minix_acl_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline size_t
minix_acl_entry_offset(size_t index)
{
	return MINIX_ACL_HEADER_SIZE + index * MINIX_ACL_ENTRY_SIZE;
}

/* Validate required base entries, IDs, permission masks and duplicate tags. */
static inline int
minix_acl_validate(const void *value, size_t length)
{
	const uint8_t *p = (const uint8_t *)value;
	uint32_t count, i, j, id_i, id_j;
	uint16_t tag_i, perm_i, tag_j;
	unsigned int seen = 0;
	int named = 0;

	if (p == NULL || length < MINIX_ACL_HEADER_SIZE ||
	    length > MINIX_ACL_MAX_VALUE ||
	    minix_acl_get_le32(p) != MINIX_ACL_VERSION)
		return EIO;
	count = minix_acl_get_le32(p + 4);
	if (count < 3 || count > (length - MINIX_ACL_HEADER_SIZE) /
	    MINIX_ACL_ENTRY_SIZE ||
	    length != MINIX_ACL_HEADER_SIZE +
	    (size_t)count * MINIX_ACL_ENTRY_SIZE)
		return EIO;

	for (i = 0; i < count; i++) {
		size_t off = minix_acl_entry_offset(i);
		tag_i = minix_acl_get_le16(p + off);
		perm_i = minix_acl_get_le16(p + off + 2);
		id_i = minix_acl_get_le32(p + off + 4);
		if ((perm_i & ~MINIX_ACL_PERM_MASK) != 0)
			return EIO;
		switch (tag_i) {
		case MINIX_ACL_USER_OBJ:
		case MINIX_ACL_GROUP_OBJ:
		case MINIX_ACL_MASK:
		case MINIX_ACL_OTHER:
			if (id_i != MINIX_ACL_UNDEFINED_ID)
				return EIO;
			break;
		case MINIX_ACL_USER:
		case MINIX_ACL_GROUP:
			if (id_i == MINIX_ACL_UNDEFINED_ID)
				return EIO;
			named = 1;
			break;
		default:
			return EIO;
		}
		for (j = 0; j < i; j++) {
			size_t prev = minix_acl_entry_offset(j);
			tag_j = minix_acl_get_le16(p + prev);
			id_j = minix_acl_get_le32(p + prev + 4);
			if (tag_i == tag_j &&
			    ((tag_i != MINIX_ACL_USER &&
			    tag_i != MINIX_ACL_GROUP) || id_i == id_j))
				return EIO;
		}
		switch (tag_i) {
		case MINIX_ACL_USER_OBJ: seen |= 1U; break;
		case MINIX_ACL_GROUP_OBJ: seen |= 2U; break;
		case MINIX_ACL_MASK: seen |= 4U; break;
		case MINIX_ACL_OTHER: seen |= 8U; break;
		default: break;
		}
	}
	/* USER_OBJ|GROUP_OBJ|OTHER (1|2|8) always required; MASK only with named. */
	if ((seen & 11U) != 11U || (named && !(seen & 4U)))
		return EIO;
	return 0;
}

static inline int
minix_acl_find_tag(const uint8_t *p, size_t count, uint16_t tag,
    uint32_t id, size_t *index)
{
	size_t i;
	for (i = 0; i < count; i++) {
		size_t off = minix_acl_entry_offset(i);
		if (minix_acl_get_le16(p + off) == tag &&
		    ((tag != MINIX_ACL_USER && tag != MINIX_ACL_GROUP) ||
		    minix_acl_get_le32(p + off + 4) == id)) {
			if (index != NULL)
				*index = i;
			return 0;
		}
	}
	return ENOENT;
}

/* Return the mode bits represented by USER_OBJ, MASK/GROUP_OBJ and OTHER. */
static inline int
minix_acl_mode(const void *value, size_t length, uint16_t *mode_out)
{
	const uint8_t *p = (const uint8_t *)value;
	size_t count, i;
	uint16_t mode = 0, group_obj = 0, mask = 0;
	int have_mask = 0, r;

	if (mode_out == NULL)
		return EINVAL;
	if ((r = minix_acl_validate(value, length)) != 0)
		return r;
	count = minix_acl_get_le32(p + 4);
	for (i = 0; i < count; i++) {
		size_t off = minix_acl_entry_offset(i);
		uint16_t tag = minix_acl_get_le16(p + off);
		uint16_t perm = minix_acl_get_le16(p + off + 2);
		switch (tag) {
		case MINIX_ACL_USER_OBJ: mode |= (uint16_t)(perm << 6); break;
		case MINIX_ACL_GROUP_OBJ: group_obj = perm; break;
		case MINIX_ACL_MASK: mask = perm; have_mask = 1; break;
		case MINIX_ACL_OTHER: mode |= perm; break;
		default: break;
		}
	}
	mode |= (uint16_t)((have_mask ? mask : group_obj) << 3);
	*mode_out = mode;
	return 0;
}

/* chmod: replace USER_OBJ, MASK (or GROUP_OBJ), and OTHER with mode bits. */
static inline int
minix_acl_set_mode(void *value, size_t length, uint16_t mode)
{
	uint8_t *p = (uint8_t *)value;
	size_t count, i, group_index;
	uint16_t group_tag = MINIX_ACL_GROUP_OBJ;
	int r;

	if ((r = minix_acl_validate(value, length)) != 0)
		return r;
	count = minix_acl_get_le32(p + 4);
	if (minix_acl_find_tag(p, count, MINIX_ACL_MASK,
	    MINIX_ACL_UNDEFINED_ID, &group_index) == 0)
		group_tag = MINIX_ACL_MASK;
	for (i = 0; i < count; i++) {
		size_t off = minix_acl_entry_offset(i);
		uint16_t tag = minix_acl_get_le16(p + off);
		uint16_t perm;
		if (tag == MINIX_ACL_USER_OBJ)
			perm = (uint16_t)((mode >> 6) & 7U);
		else if (tag == group_tag)
			perm = (uint16_t)((mode >> 3) & 7U);
		else if (tag == MINIX_ACL_OTHER)
			perm = (uint16_t)(mode & 7U);
		else
			continue;
		minix_acl_put_le16(p + off + 2, perm);
	}
	return 0;
}

/* Creation inheritance: intersect the inherited ACL with requested mode. */
static inline int
minix_acl_restrict_mode(void *value, size_t length, uint16_t mode)
{
	uint8_t *p = (uint8_t *)value;
	size_t count, i;
	int r;
	if ((r = minix_acl_validate(value, length)) != 0)
		return r;
	count = minix_acl_get_le32(p + 4);
	for (i = 0; i < count; i++) {
		size_t off = minix_acl_entry_offset(i);
		uint16_t tag = minix_acl_get_le16(p + off);
		uint16_t mask;
		if (tag == MINIX_ACL_USER_OBJ)
			mask = (uint16_t)((mode >> 6) & 7U);
		else if (tag == MINIX_ACL_MASK)
			mask = (uint16_t)((mode >> 3) & 7U);
		else if (tag == MINIX_ACL_GROUP_OBJ) {
			size_t dummy;
			if (minix_acl_find_tag(p, count, MINIX_ACL_MASK,
			    MINIX_ACL_UNDEFINED_ID, &dummy) == 0)
				continue;
			mask = (uint16_t)((mode >> 3) & 7U);
		} else if (tag == MINIX_ACL_OTHER)
			mask = (uint16_t)(mode & 7U);
		else
			continue;
		minix_acl_put_le16(p + off + 2,
		    (uint16_t)(minix_acl_get_le16(p + off + 2) & mask));
	}
	return 0;
}

typedef int (*minix_acl_group_match_fn)(void *, uint32_t);

/*
 * Evaluate access ACL in POSIX order. A matching named USER is terminal;
 * group class is the union of all matching owning/named groups, then MASK.
 */
static inline int
minix_acl_access(const void *value, size_t length, uint32_t uid,
    uint32_t owner_uid, uint32_t owner_gid,
    minix_acl_group_match_fn group_match, void *context,
    uint16_t *permissions)
{
	const uint8_t *p = (const uint8_t *)value;
	uint32_t count, id;
	uint16_t tag, perm, mask = MINIX_ACL_PERM_MASK;
	uint16_t group_union = 0;
	size_t i, index;
	int group_found = 0, have_mask = 0, r;

	if (permissions == NULL || group_match == NULL)
		return EINVAL;
	if ((r = minix_acl_validate(value, length)) != 0)
		return r;
	count = minix_acl_get_le32(p + 4);
	if (uid == owner_uid) {
		(void)minix_acl_find_tag(p, count, MINIX_ACL_USER_OBJ,
		    MINIX_ACL_UNDEFINED_ID, &index);
		*permissions = minix_acl_get_le16(p +
		    minix_acl_entry_offset(index) + 2);
		return 0;
	}
	for (i = 0; i < count; i++) {
		size_t off = minix_acl_entry_offset(i);
		tag = minix_acl_get_le16(p + off);
		if (tag == MINIX_ACL_USER &&
		    minix_acl_get_le32(p + off + 4) == uid) {
			perm = minix_acl_get_le16(p + off + 2);
			(void)minix_acl_find_tag(p, count, MINIX_ACL_MASK,
			    MINIX_ACL_UNDEFINED_ID, &index);
			mask = minix_acl_get_le16(p +
			    minix_acl_entry_offset(index) + 2);
			*permissions = (uint16_t)(perm & mask);
			return 0;
		}
	}
	if (group_match(context, owner_gid)) {
		(void)minix_acl_find_tag(p, count, MINIX_ACL_GROUP_OBJ,
		    MINIX_ACL_UNDEFINED_ID, &index);
		group_union |= minix_acl_get_le16(p +
		    minix_acl_entry_offset(index) + 2);
		group_found = 1;
	}
	for (i = 0; i < count; i++) {
		size_t off = minix_acl_entry_offset(i);
		tag = minix_acl_get_le16(p + off);
		if (tag != MINIX_ACL_GROUP)
			continue;
		id = minix_acl_get_le32(p + off + 4);
		if (group_match(context, id)) {
			group_union |= minix_acl_get_le16(p + off + 2);
			group_found = 1;
		}
	}
	if (group_found) {
		if (minix_acl_find_tag(p, count, MINIX_ACL_MASK,
		    MINIX_ACL_UNDEFINED_ID, &index) == 0) {
			mask = minix_acl_get_le16(p +
			    minix_acl_entry_offset(index) + 2);
			have_mask = 1;
		}
		(void)have_mask;
		*permissions = (uint16_t)(group_union & mask);
		return 0;
	}
	(void)minix_acl_find_tag(p, count, MINIX_ACL_OTHER,
	    MINIX_ACL_UNDEFINED_ID, &index);
	*permissions = minix_acl_get_le16(p +
	    minix_acl_entry_offset(index) + 2);
	return 0;
}

#endif /* _MINIX_POSIX_ACL_H */
