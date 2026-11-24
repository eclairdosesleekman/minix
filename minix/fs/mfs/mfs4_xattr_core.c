#include <errno.h>
#include <string.h>

#include <sys/xattr.h>
#include <minix/mfs4_disk.h>

#include "mfs4_xattr_core.h"

static size_t
mfs4_xattr_align8(size_t value)
{
	return (value + 7U) & ~(size_t)7U;
}

int
mfs4_xattr_name_valid(const char *name, int allow_acl)
{
	size_t len;

	if (name == NULL)
		return EINVAL;
	len = strlen(name);
	if (len == 0)
		return EINVAL;
	if (len > MFS4_XATTR_MAX_NAME)
		return ERANGE;
	if (strncmp(name, "user.", 5) == 0 && len > 5)
		return 0;
	if (allow_acl &&
	    (strcmp(name, "system.posix_acl_access") == 0 ||
	    strcmp(name, "system.posix_acl_default") == 0))
		return 0;
	if (strncmp(name, "trusted.", 8) == 0 ||
	    strncmp(name, "security.", 9) == 0)
		return EOPNOTSUPP;
	return EOPNOTSUPP;
}

int
mfs4_xattr_entry_next(const uint8_t *data, size_t length, size_t *offset,
    struct mfs4_xattr_entry *entry)
{
	size_t start, encoded, payload;
	uint16_t name_len, flags;
	uint32_t value_len;
	size_t i;

	if (data == NULL || offset == NULL || entry == NULL)
		return EINVAL;
	start = *offset;
	if (start > length || length - start < MFS4_XATTR_TLV_HEADER_SIZE)
		return EIO;

	name_len = mfs4_get_le16(data + start + MFS4_XT_NAME_LEN);
	flags = mfs4_get_le16(data + start + MFS4_XT_FLAGS);
	value_len = mfs4_get_le32(data + start + MFS4_XT_VALUE_LEN);
	if (name_len == 0 || name_len > MFS4_XATTR_MAX_NAME || flags != 0 ||
	    value_len > MFS4_XATTR_MAX_VALUE)
		return EIO;

	payload = (size_t)name_len + (size_t)value_len;
	if (payload > SIZE_MAX - MFS4_XATTR_TLV_HEADER_SIZE)
		return EIO;
	encoded = mfs4_xattr_align8(MFS4_XATTR_TLV_HEADER_SIZE + payload);
	if (encoded > length - start)
		return EIO;

	{
		char name[MFS4_XATTR_MAX_NAME + 1];

		for (i = 0; i < name_len; i++) {
			name[i] = (char)data[start +
			    MFS4_XATTR_TLV_HEADER_SIZE + i];
			if (name[i] == '\0')
				return EIO;
		}
		name[name_len] = '\0';
		if (mfs4_xattr_name_valid(name, 1) != 0)
			return EIO;
	}
	for (i = MFS4_XATTR_TLV_HEADER_SIZE + payload; i < encoded; i++)
		if (data[start + i] != 0)
			return EIO;

	entry->name = data + start + MFS4_XATTR_TLV_HEADER_SIZE;
	entry->name_len = name_len;
	entry->value = entry->name + name_len;
	entry->value_len = value_len;
	entry->encoded_len = encoded;
	*offset = start + encoded;
	return 0;
}

int
mfs4_xattr_validate_entries(const uint8_t *data, size_t length,
    uint32_t expected_count, int reject_duplicates)
{
	struct mfs4_xattr_entry current, previous;
	size_t offset = 0, prior;
	uint32_t count = 0;
	int r;

	if (length != 0 && data == NULL)
		return EINVAL;
	while (offset < length) {
		size_t start = offset;

		if ((r = mfs4_xattr_entry_next(data, length, &offset,
		    &current)) != 0)
			return r;
		if (reject_duplicates) {
			prior = 0;
			while (prior < start) {
				if ((r = mfs4_xattr_entry_next(data, start, &prior,
				    &previous)) != 0)
					return r;
				if (previous.name_len == current.name_len &&
				    memcmp(previous.name, current.name,
				    current.name_len) == 0)
					return EIO;
			}
		}
		if (++count > expected_count && expected_count != UINT32_MAX)
			return EIO;
	}
	if (offset != length ||
	    (expected_count != UINT32_MAX && count != expected_count))
		return EIO;
	return 0;
}

int
mfs4_xattr_validate_block(const uint8_t block[MFS4_BLOCK_SIZE],
    uint32_t owner, uint32_t generation, uint32_t *next,
    uint16_t *used_bytes, uint16_t *entry_count)
{
	uint8_t copy[MFS4_BLOCK_SIZE];
	uint16_t used, count;
	uint32_t link;
	unsigned int i;
	int r;

	if (block == NULL || next == NULL || used_bytes == NULL ||
	    entry_count == NULL || owner == 0 || generation == 0)
		return EINVAL;
	if (memcmp(block + MFS4_XA_MAGIC, MFS4_XATTR_MAGIC, 4) != 0 ||
	    mfs4_get_le32(block + MFS4_XA_OWNER) != owner ||
	    mfs4_get_le32(block + MFS4_XA_GENERATION) != generation)
		return EIO;

	link = mfs4_get_le32(block + MFS4_XA_NEXT);
	used = mfs4_get_le16(block + MFS4_XA_USED_BYTES);
	count = mfs4_get_le16(block + MFS4_XA_ENTRY_COUNT);
	if (mfs4_get_le32(block + MFS4_XA_FLAGS) != 0 ||
	    mfs4_get_le32(block + MFS4_XA_RESERVED) != 0 ||
	    used > MFS4_XATTR_DATA_SIZE ||
	    (count == 0) != (used == 0))
		return EIO;

	for (i = MFS4_XA_ENTRIES + used; i < MFS4_BLOCK_SIZE; i++)
		if (block[i] != 0)
			return EIO;
	if ((r = mfs4_xattr_validate_entries(block + MFS4_XA_ENTRIES,
	    used, count, 1)) != 0)
		return EIO;

	memcpy(copy, block, sizeof(copy));
	mfs4_put_le32(copy + MFS4_XA_CRC32C, 0);
	if (mfs4_get_le32(block + MFS4_XA_CRC32C) !=
	    mfs4_crc32c(copy, sizeof(copy)))
		return EIO;

	*next = link;
	*used_bytes = used;
	*entry_count = count;
	return 0;
}

int
mfs4_xattr_find(const uint8_t *data, size_t length, const char *name,
    size_t *entry_offset, struct mfs4_xattr_entry *entry)
{
	struct mfs4_xattr_entry current;
	size_t offset = 0, start;
	size_t name_len;
	int r;

	if ((length != 0 && data == NULL) || name == NULL)
		return EINVAL;
	if ((r = mfs4_xattr_name_valid(name, 1)) != 0)
		return r;
	name_len = strlen(name);
	while (offset < length) {
		start = offset;
		if ((r = mfs4_xattr_entry_next(data, length, &offset,
		    &current)) != 0)
			return r;
		if (current.name_len == name_len &&
		    memcmp(current.name, name, name_len) == 0) {
			if (entry_offset != NULL)
				*entry_offset = start;
			if (entry != NULL)
				*entry = current;
			return 0;
		}
	}
	return ENODATA;
}

int
mfs4_xattr_append(uint8_t *data, size_t capacity, size_t *length,
    uint32_t *count, const char *name, const void *value, size_t value_len)
{
	size_t name_len, encoded, payload, i;
	size_t offset;
	struct mfs4_xattr_entry found;
	int r;

	if (data == NULL || length == NULL || count == NULL ||
	    (value_len != 0 && value == NULL))
		return EINVAL;
	if ((r = mfs4_xattr_name_valid(name, 1)) != 0)
		return r;
	name_len = strlen(name);
	if (value_len > MFS4_XATTR_MAX_VALUE ||
	    name_len + value_len > SIZE_MAX - MFS4_XATTR_TLV_HEADER_SIZE)
		return ERANGE;
	payload = name_len + value_len;
	encoded = mfs4_xattr_align8(MFS4_XATTR_TLV_HEADER_SIZE + payload);
	if (*length > capacity || encoded > capacity - *length)
		return ENOSPC;
	r = mfs4_xattr_find(data, *length, name, NULL, &found);
	if (r == 0)
		return EEXIST;
	if (r != ENODATA)
		return r;
	if (*count == UINT32_MAX)
		return E2BIG;

	offset = *length;
	memset(data + offset, 0, encoded);
	mfs4_put_le16(data + offset + MFS4_XT_NAME_LEN, (uint16_t)name_len);
	mfs4_put_le16(data + offset + MFS4_XT_FLAGS, 0);
	mfs4_put_le32(data + offset + MFS4_XT_VALUE_LEN, (uint32_t)value_len);
	memcpy(data + offset + MFS4_XATTR_TLV_HEADER_SIZE, name, name_len);
	if (value_len != 0)
		memcpy(data + offset + MFS4_XATTR_TLV_HEADER_SIZE + name_len,
		    value, value_len);
	for (i = MFS4_XATTR_TLV_HEADER_SIZE + payload; i < encoded; i++)
		data[offset + i] = 0;
	*length += encoded;
	(*count)++;
	return 0;
}

int
mfs4_xattr_remove(uint8_t *data, size_t *length, uint32_t *count,
    const char *name)
{
	size_t offset = 0, start, name_len;
	struct mfs4_xattr_entry current;
	int r;

	if (data == NULL || length == NULL || count == NULL || *count == 0)
		return EINVAL;
	if ((r = mfs4_xattr_name_valid(name, 1)) != 0)
		return r;
	name_len = strlen(name);
	while (offset < *length) {
		start = offset;
		if ((r = mfs4_xattr_entry_next(data, *length, &offset,
		    &current)) != 0)
			return r;
		if (current.name_len == name_len &&
		    memcmp(current.name, name, name_len) == 0) {
			memmove(data + start, data + offset, *length - offset);
			*length -= current.encoded_len;
			memset(data + *length, 0, current.encoded_len);
			(*count)--;
			return 0;
		}
	}
	return ENODATA;
}
