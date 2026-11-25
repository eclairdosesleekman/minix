/*
 * MFS4 POSIX ACL persistence, chmod synchronization and default inheritance.
 * Final access decisions are made only by VFS; this module validates and
 * stores ACL values and applies the directory inheritance rules.
 */
#include "fs.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <minix/posix_acl.h>

#include "inode.h"
#include "super.h"
#include "mfs4_acl.h"
#include "mfs4_xattr.h"

#define ACL_ACCESS_NAME "system.posix_acl_access"
#define ACL_DEFAULT_NAME "system.posix_acl_default"

static int
mfs4_acl_chmod_value(uint8_t *value, size_t length, mode_t mode)
{
	size_t count, i, group_index;
	uint16_t group_tag = MINIX_ACL_GROUP_OBJ;
	int r;

	if ((r = minix_acl_validate(value, length)) != 0)
		return r;
	count = minix_acl_get_le32(value + 4);
	if (minix_acl_find_tag(value, count, MINIX_ACL_MASK,
	    MINIX_ACL_UNDEFINED_ID, &group_index) == 0)
		group_tag = MINIX_ACL_MASK;

	for (i = 0; i < count; i++) {
		size_t off = minix_acl_entry_offset(i);
		uint16_t tag = minix_acl_get_le16(value + off);
		uint16_t perm;

		if (tag == MINIX_ACL_USER_OBJ)
			perm = (uint16_t)((mode >> 6) & 7U);
		else if (tag == group_tag)
			perm = (uint16_t)((mode >> 3) & 7U);
		else if (tag == MINIX_ACL_OTHER)
			perm = (uint16_t)(mode & 7U);
		else
			continue;

		minix_acl_put_le16(value + off + 2, perm);
	}
	return 0;
}

int
mfs4_acl_chmod(struct inode *rip, mode_t new_mode)
{
	uint8_t *value = NULL;
	size_t length = 0;
	int r;

	if (rip == NULL || rip->i_sp == NULL ||
	    rip->i_sp->s_version != V4)
		return EOPNOTSUPP;

	r = mfs4_xattr_getvalue(rip, ACL_ACCESS_NAME, &value, &length);
	if (r == ENODATA)
		return OK;
	if (r != OK)
		return r;

	r = mfs4_acl_chmod_value(value, length, new_mode);
	if (r == OK)
		r = mfs4_xattr_setvalue_tx(rip, ACL_ACCESS_NAME, value,
		    length, 0);
	free(value);
	return r;
}

int
mfs4_acl_inherit(struct inode *parent, struct inode *child,
    int child_is_directory)
{
	uint8_t *default_acl = NULL, *access_acl = NULL;
	size_t default_len = 0;
	int r;

	if (parent == NULL || child == NULL || parent->i_sp == NULL ||
	    child->i_sp == NULL || parent->i_sp->s_version != V4 ||
	    child->i_sp->s_version != V4)
		return EOPNOTSUPP;

	r = mfs4_xattr_getvalue(parent, ACL_DEFAULT_NAME, &default_acl,
	    &default_len);
	if (r == ENODATA)
		return OK;
	if (r != OK)
		return r;
	if (minix_acl_validate(default_acl, default_len) != 0) {
		free(default_acl);
		return EIO;
	}

	access_acl = malloc(default_len);
	if (access_acl == NULL) {
		free(default_acl);
		return ENOMEM;
	}
	memcpy(access_acl, default_acl, default_len);

	/* Intersect USER_OBJ, MASK/GROUP_OBJ and OTHER with the create mode. */
	r = minix_acl_restrict_mode(access_acl, default_len,
	    (uint16_t)(child->i_mode & ALL_MODES));
	if (r == 0)
		r = mfs4_xattr_setvalue_tx(child, ACL_ACCESS_NAME, access_acl,
		    default_len, 0);
	if (r == 0 && child_is_directory)
		r = mfs4_xattr_setvalue_tx(child, ACL_DEFAULT_NAME, default_acl,
		    default_len, 0);

	free(access_acl);
	free(default_acl);
	return r;
}
