#ifndef _MFS4_ORPHAN_CORE_H
#define _MFS4_ORPHAN_CORE_H

#include <stdint.h>

/*
 * Validate and update the byte-oriented MFS4 state block. The volume
 * generation at offset 16 is immutable for the lifetime of the filesystem;
 * orphan head updates therefore change only the head and CRC.
 */
int mfs4_orphan_state_validate(const uint8_t state[4096],
    uint64_t volume_generation, uint32_t total_inodes, uint32_t *head_out);
int mfs4_orphan_state_set_head(uint8_t state[4096],
    uint64_t volume_generation, uint32_t total_inodes, uint32_t head);

/* open_refs includes the caller's temporary reference when nonzero. */
int mfs4_orphan_can_reclaim(uint16_t inode_flags, uint32_t nlink,
    unsigned int open_refs, int mount_recovery);

#endif /* _MFS4_ORPHAN_CORE_H */
