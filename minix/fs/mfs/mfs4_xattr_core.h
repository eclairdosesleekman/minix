#ifndef _MFS4_XATTR_CORE_H
#define _MFS4_XATTR_CORE_H

#include <stddef.h>
#include <stdint.h>

#include <minix/mfs4_disk.h>

struct mfs4_xattr_entry {
	const uint8_t *name;
	uint16_t name_len;
	const uint8_t *value;
	uint32_t value_len;
	size_t encoded_len;
};

int mfs4_xattr_name_valid(const char *name, int allow_acl);
int mfs4_xattr_entry_next(const uint8_t *data, size_t length, size_t *offset,
    struct mfs4_xattr_entry *entry);
int mfs4_xattr_validate_entries(const uint8_t *data, size_t length,
    uint32_t expected_count, int reject_duplicates);
int mfs4_xattr_validate_block(const uint8_t block[MFS4_BLOCK_SIZE],
    uint32_t owner, uint32_t generation, uint32_t *next,
    uint16_t *used_bytes, uint16_t *entry_count);
int mfs4_xattr_find(const uint8_t *data, size_t length, const char *name,
    size_t *entry_offset, struct mfs4_xattr_entry *entry);
int mfs4_xattr_append(uint8_t *data, size_t capacity, size_t *length,
    uint32_t *count, const char *name, const void *value, size_t value_len);
int mfs4_xattr_remove(uint8_t *data, size_t *length, uint32_t *count,
    const char *name);

#endif /* _MFS4_XATTR_CORE_H */
