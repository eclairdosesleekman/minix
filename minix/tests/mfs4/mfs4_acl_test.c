#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <minix/posix_acl.h>

static void
put_entry(uint8_t *p, unsigned int i, uint16_t tag, uint16_t perm,
    uint32_t id)
{
	size_t off = minix_acl_entry_offset(i);
	minix_acl_put_le16(p + off, tag);
	minix_acl_put_le16(p + off + 2, perm);
	minix_acl_put_le32(p + off + 4, id);
}

static size_t
make_acl(uint8_t *p)
{
	memset(p, 0, 64);
	minix_acl_put_le32(p, MINIX_ACL_VERSION);
	minix_acl_put_le32(p + 4, 6);
	put_entry(p, 0, MINIX_ACL_USER_OBJ, 7, MINIX_ACL_UNDEFINED_ID);
	put_entry(p, 1, MINIX_ACL_USER, 4, 1000);
	put_entry(p, 2, MINIX_ACL_GROUP_OBJ, 6, MINIX_ACL_UNDEFINED_ID);
	put_entry(p, 3, MINIX_ACL_GROUP, 2, 2000);
	put_entry(p, 4, MINIX_ACL_MASK, 5, MINIX_ACL_UNDEFINED_ID);
	put_entry(p, 5, MINIX_ACL_OTHER, 1, MINIX_ACL_UNDEFINED_ID);
	return MINIX_ACL_HEADER_SIZE + 6 * MINIX_ACL_ENTRY_SIZE;
}

static int
group_match(void *context, uint32_t gid)
{
	const uint32_t *groups = context;
	size_t i;
	for (i = 0; groups[i] != UINT32_MAX; i++)
		if (groups[i] == gid)
			return 1;
	return 0;
}

int
main(void)
{
	uint8_t raw[64], changed[64];
	size_t len = make_acl(raw);
	uint16_t perms, mode;
	uint32_t groups[] = { 3000, 2000, UINT32_MAX };

	assert(minix_acl_validate(raw, len) == 0);
	/* Named user match is terminal and is masked (4 & 5 == 4). */
	assert(minix_acl_access(raw, len, 1000, 10, 3000,
	    group_match, groups, &perms) == 0);
	assert(perms == 4);
	/* Group class unions GROUP_OBJ (6) and named GROUP (2), then MASK (5). */
	assert(minix_acl_access(raw, len, 2001, 10, 3000,
	    group_match, groups, &perms) == 0);
	assert(perms == 4);
	/* No group match selects OTHER. */
	groups[0] = 7000;
	groups[1] = UINT32_MAX;
	assert(minix_acl_access(raw, len, 2001, 10, 3000,
	    group_match, groups, &perms) == 0);
	assert(perms == 1);
	/* Owner always selects USER_OBJ without the mask. */
	assert(minix_acl_access(raw, len, 10, 10, 3000,
	    group_match, groups, &perms) == 0);
	assert(perms == 7);

	assert(minix_acl_mode(raw, len, &mode) == 0);
	assert(mode == 0751);
	memcpy(changed, raw, len);
	assert(minix_acl_set_mode(changed, len, 0640) == 0);
	assert(minix_acl_mode(changed, len, &mode) == 0);
	assert(mode == 0640);
	/* chmod changes MASK, not the underlying GROUP_OBJ permission. */
	assert(minix_acl_get_le16(changed +
	    minix_acl_entry_offset(2) + 2) == 6);
	assert(minix_acl_get_le16(changed +
	    minix_acl_entry_offset(4) + 2) == 4);
	memcpy(changed, raw, len);
	assert(minix_acl_restrict_mode(changed, len, 0640) == 0);
	assert(minix_acl_mode(changed, len, &mode) == 0);
	assert(mode == 0640);
	/* A named entry without MASK is invalid. */
	minix_acl_put_le32(changed + 4, 5);
	memcpy(changed + MINIX_ACL_HEADER_SIZE + 4 * MINIX_ACL_ENTRY_SIZE,
	    changed + MINIX_ACL_HEADER_SIZE + 5 * MINIX_ACL_ENTRY_SIZE, 8);
	assert(minix_acl_validate(changed, len - 8) == EIO);
	/* Unknown tag is rejected. */
	memcpy(changed, raw, len);
	minix_acl_put_le16(changed + MINIX_ACL_HEADER_SIZE, 0x7777);
	assert(minix_acl_validate(changed, len) == EIO);
	puts("mfs4_acl_test: all tests passed");
	return 0;
}
