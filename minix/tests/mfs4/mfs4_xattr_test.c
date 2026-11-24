#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <minix/mfs4_disk.h>
#include "mfs4_xattr_core.h"

static void
finish_block(uint8_t block[MFS4_BLOCK_SIZE], uint32_t owner,
    uint32_t generation, const uint8_t *entries, uint16_t used,
    uint16_t count, uint32_t next)
{
	memset(block, 0, MFS4_BLOCK_SIZE);
	memcpy(block + MFS4_XA_MAGIC, MFS4_XATTR_MAGIC, 4);
	mfs4_put_le32(block + MFS4_XA_OWNER, owner);
	mfs4_put_le32(block + MFS4_XA_GENERATION, generation);
	mfs4_put_le32(block + MFS4_XA_NEXT, next);
	mfs4_put_le16(block + MFS4_XA_USED_BYTES, used);
	mfs4_put_le16(block + MFS4_XA_ENTRY_COUNT, count);
	if (used != 0)
		memcpy(block + MFS4_XA_ENTRIES, entries, used);
	mfs4_put_le32(block + MFS4_XA_CRC32C,
	    mfs4_crc32c(block, MFS4_BLOCK_SIZE));
}

static void
test_name_policy(void)
{
	assert(mfs4_xattr_name_valid("user.comment", 0) == 0);
	assert(mfs4_xattr_name_valid("user.", 0) == EOPNOTSUPP);
	assert(mfs4_xattr_name_valid("system.posix_acl_access", 1) == 0);
	assert(mfs4_xattr_name_valid("system.posix_acl_default", 1) == 0);
	assert(mfs4_xattr_name_valid("security.selinux", 1) == EOPNOTSUPP);
	assert(mfs4_xattr_name_valid("trusted.secret", 1) == EOPNOTSUPP);
	assert(mfs4_xattr_name_valid("", 1) == EINVAL);
}

static void
test_remove_from_empty_stream(void)
{
	uint8_t data[32] = {0};
	size_t length = 0;
	uint32_t count = 0;

	assert(mfs4_xattr_remove(data, &length, &count,
	    "user.missing") == ENODATA);
}

static void
test_tlv_roundtrip_and_remove(void)
{
	uint8_t bytes[256];
	const char value1[] = "hello";
	const char value2[] = "world-value";
	struct mfs4_xattr_entry entry;
	size_t length = 0, offset;
	uint32_t count = 0;

	memset(bytes, 0, sizeof(bytes));
	assert(mfs4_xattr_append(bytes, sizeof(bytes), &length, &count,
	    "user.comment", value1, sizeof(value1)) == 0);
	assert(mfs4_xattr_append(bytes, sizeof(bytes), &length, &count,
	    "user.other", value2, sizeof(value2)) == 0);
	assert(count == 2);
	assert(mfs4_xattr_validate_entries(bytes, length, count, 1) == 0);
	assert(mfs4_xattr_find(bytes, length, "user.comment",
	    &offset, &entry) == 0);
	assert(entry.value_len == sizeof(value1));
	assert(memcmp(entry.value, value1, sizeof(value1)) == 0);
	assert(mfs4_xattr_append(bytes, sizeof(bytes), &length, &count,
	    "user.comment", value1, sizeof(value1)) == EEXIST);
	assert(mfs4_xattr_remove(bytes, &length, &count, "user.comment") == 0);
	assert(count == 1);
	assert(mfs4_xattr_find(bytes, length, "user.comment", NULL, NULL) ==
	    ENODATA);
	assert(mfs4_xattr_validate_entries(bytes, length, count, 1) == 0);
}

static void
test_block_validation(void)
{
	uint8_t block[MFS4_BLOCK_SIZE], entries[128];
	size_t length = 0;
	uint32_t count = 0, next;
	uint16_t used, entries_count;

	memset(entries, 0, sizeof(entries));
	assert(mfs4_xattr_append(entries, sizeof(entries), &length, &count,
	    "user.key", "value", 5) == 0);
	finish_block(block, 12, 99, entries, (uint16_t)length,
	    (uint16_t)count, 73);
	assert(mfs4_xattr_validate_block(block, 12, 99, &next, &used,
	    &entries_count) == 0);
	assert(next == 73 && used == length && entries_count == 1);

	finish_block(block, 12, 99, entries, (uint16_t)length,
	    (uint16_t)count, 0);
	assert(mfs4_xattr_validate_block(block, 12, 99, &next, &used,
	    &entries_count) == 0 && next == 0);

	block[100] ^= 1;
	assert(mfs4_xattr_validate_block(block, 12, 99, &next, &used,
	    &entries_count) == EIO);
	finish_block(block, 12, 99, entries, (uint16_t)length,
	    (uint16_t)count, 0);
	assert(mfs4_xattr_validate_block(block, 13, 99, &next, &used,
	    &entries_count) == EIO);
	assert(mfs4_xattr_validate_block(block, 12, 100, &next, &used,
	    &entries_count) == EIO);
	block[MFS4_XA_RESERVED] = 1;
	mfs4_put_le32(block + MFS4_XA_CRC32C, 0);
	mfs4_put_le32(block + MFS4_XA_CRC32C,
	    mfs4_crc32c(block, MFS4_BLOCK_SIZE));
	assert(mfs4_xattr_validate_block(block, 12, 99, &next, &used,
	    &entries_count) == EIO);
}

static void
test_malformed_tlvs(void)
{
	uint8_t bytes[64];
	size_t offset = 0;

	memset(bytes, 0, sizeof(bytes));
	mfs4_put_le16(bytes + MFS4_XT_NAME_LEN, 1);
	mfs4_put_le16(bytes + MFS4_XT_FLAGS, 1);
	mfs4_put_le32(bytes + MFS4_XT_VALUE_LEN, 0);
	assert(mfs4_xattr_entry_next(bytes, 16, &offset,
	    &(struct mfs4_xattr_entry){0}) == EIO);

	memset(bytes, 0, sizeof(bytes));
	mfs4_put_le16(bytes + MFS4_XT_NAME_LEN, 1);
	mfs4_put_le16(bytes + MFS4_XT_FLAGS, 0);
	mfs4_put_le32(bytes + MFS4_XT_VALUE_LEN, 0);
	bytes[8] = 0;
	assert(mfs4_xattr_entry_next(bytes, 16, &offset,
	    &(struct mfs4_xattr_entry){0}) == EIO);
}


static void
test_maximum_name_and_value_fit_one_block(void)
{
	uint8_t name[MFS4_XATTR_MAX_NAME + 1];
	uint8_t value[MFS4_XATTR_MAX_VALUE];
	uint8_t entries[MFS4_XATTR_DATA_SIZE];
	struct mfs4_xattr_entry entry;
	size_t length = 0;
	uint32_t count = 0;
	size_t i;

	memcpy(name, "user.", 5);
	for (i = 5; i < MFS4_XATTR_MAX_NAME; i++)
		name[i] = 'n';
	name[MFS4_XATTR_MAX_NAME] = '\\0';
	memset(value, 0x5a, sizeof(value));
	memset(entries, 0, sizeof(entries));

	assert(mfs4_xattr_append(entries, sizeof(entries), &length, &count,
	    (const char *)name, value, sizeof(value)) == 0);
	assert(length == MFS4_XATTR_DATA_SIZE);
	assert(count == 1);
	assert(mfs4_xattr_validate_entries(entries, length, count, 1) == 0);
	assert(mfs4_xattr_find(entries, length, (const char *)name,
	    NULL, &entry) == 0);
	assert(entry.name_len == MFS4_XATTR_MAX_NAME);
	assert(entry.value_len == MFS4_XATTR_MAX_VALUE);
	assert(memcmp(entry.value, value, sizeof(value)) == 0);
}

static void
test_empty_value_and_invalid_capacity(void)
{
	uint8_t entries[32];
	size_t length = 0;
	uint32_t count = 0;

	memset(entries, 0, sizeof(entries));
	assert(mfs4_xattr_append(entries, sizeof(entries), &length, &count,
	    "user.empty", NULL, 0) == 0);
	assert(length == 24);
	assert(count == 1);
	assert(mfs4_xattr_validate_entries(entries, length, count, 1) == 0);
	length = sizeof(entries) + 1;
	assert(mfs4_xattr_append(entries, sizeof(entries), &length, &count,
	    "user.more", "v", 1) == ENOSPC);
}


static void
test_duplicate_entries_rejected_on_disk(void)
{
	uint8_t data[128];
	size_t length = 0;
	uint32_t count = 0;
	struct mfs4_xattr_entry entry;
	size_t first_len;

	memset(data, 0, sizeof(data));
	assert(mfs4_xattr_append(data, sizeof(data), &length, &count,
	    "user.dup", "x", 1) == 0);
	first_len = length;
	assert(mfs4_xattr_entry_next(data, length, &(size_t){0},
	    &entry) == 0);
	memcpy(data + length, data, first_len);
	length += first_len;
	count++;
	assert(mfs4_xattr_validate_entries(data, length, count, 1) == EIO);
}

static void
test_nonzero_tlv_padding_rejected(void)
{
	uint8_t data[64];
	size_t length = 0;
	uint32_t count = 0;
	size_t pad;

	memset(data, 0, sizeof(data));
	assert(mfs4_xattr_append(data, sizeof(data), &length, &count,
	    "user.pad", "v", 1) == 0);
	pad = MFS4_XATTR_TLV_HEADER_SIZE + strlen("user.pad") + 1;
	assert(pad < length);
	data[pad] = 0x7f;
	assert(mfs4_xattr_validate_entries(data, length, count, 1) == EIO);
}

int
main(void)
{
	test_name_policy();
	test_remove_from_empty_stream();
	test_tlv_roundtrip_and_remove();
	test_block_validation();
	test_malformed_tlvs();
	test_maximum_name_and_value_fit_one_block();
	test_empty_value_and_invalid_capacity();
	test_duplicate_entries_rejected_on_disk();
	test_nonzero_tlv_padding_rejected();
	puts("mfs4_xattr_test: all tests passed");
	return 0;
}
