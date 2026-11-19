/*
 * Read-only ext4 extent-tree lookup.
 *
 * Extent trees have a 12-byte header followed by 12-byte index or extent
 * records.  The inode root is stored in i_block; child nodes are filesystem
 * blocks.  Holes and unwritten extents are reported as NO_BLOCK so the
 * caller returns zero-filled data.
 */
#include "fs.h"
#include <string.h>
#include "buf.h"
#include "inode.h"
#include "super.h"

static u16_t
ext4_le16(const u8_t *p)
{
        u16_t v;

        memcpy(&v, p, sizeof(v));
        return (u16_t)conv2(le_CPU, v);
}

static u32_t
ext4_le32(const u8_t *p)
{
        u32_t v;

        memcpy(&v, p, sizeof(v));
        return (u32_t)conv4(le_CPU, (long)v);
}

block64_t
ext4_read_map(struct inode *rip, u64_t logical, int how)
{
        const u8_t *node;
        struct buf *bp, *next;
        size_t node_size, capacity, i;
        u16_t entries, maximum, depth, expected_depth;
        u32_t previous_end, first;
        u64_t physical;
        block64_t result;
        int have_expected;

        if (rip == NULL || rip->i_sp == NULL ||
            !(rip->i_flags & EXT4_EXTENTS_FL) || logical > UINT32_MAX)
                return NO_BLOCK;

        bp = NULL;
        node = (const u8_t *)rip->i_block;
        node_size = sizeof(rip->i_block);
        have_expected = 0;
        expected_depth = 0;
        result = NO_BLOCK;

        for (;;) {
                if (node_size < 12 ||
                    ext4_le16(node) != EXT4_EXTENT_MAGIC)
                        break;

                entries = ext4_le16(node + 2);
                maximum = ext4_le16(node + 4);
                depth = ext4_le16(node + 6);
                capacity = (node_size - 12) / 12;

                if (depth > EXT4_EXTENT_MAX_DEPTH ||
                    (have_expected && depth != expected_depth) ||
                    maximum == 0 || maximum > capacity ||
                    entries > maximum)
                        break;

                if (entries == 0)
                        break;

                if (depth == 0) {
                        previous_end = 0;
                        for (i = 0; i < entries; i++) {
                                const u8_t *ep = node + 12 + i * 12;
                                u32_t start = ext4_le32(ep);
                                u16_t encoded_len = ext4_le16(ep + 4);
                                u32_t extent_len;
                                u64_t delta;

                                /* Reject overlapping or unsorted extents. */
                                if (i != 0 && start < previous_end)
                                        goto out;
                                extent_len = encoded_len > 0x8000 ?
                                    (u32_t)encoded_len - 0x8000 :
                                    (u32_t)encoded_len;
                                if (extent_len == 0 ||
                                    (u64_t)start + extent_len > (u64_t)UINT32_MAX + 1)
                                        goto out;
                                previous_end = start + extent_len;

                                if (start > (u32_t)logical)
                                        break;
                                if (logical - start >= extent_len)
                                        continue;

                                /* ee_len > 32768 denotes an unwritten extent. */
                                if (encoded_len > 0x8000)
                                        goto out;

                                physical = ((u64_t)ext4_le16(ep + 6) << 32) |
                                    ext4_le32(ep + 8);
                                delta = logical - start;
                                if (physical >= rip->i_sp->s_blocks_count64 ||
                                    delta >= rip->i_sp->s_blocks_count64 - physical)
                                        goto out;

                                result = physical + delta;
                                goto out;
                        }
                        break;
                }

                /* Select the last index whose logical key is <= the block. */
                first = ext4_le32(node + 12);
                if (first > (u32_t)logical)
                        break;
                previous_end = first;
                {
                        const u8_t *selected = node + 12;
                        for (i = 1; i < entries; i++) {
                                const u8_t *ep = node + 12 + i * 12;
                                u32_t key = ext4_le32(ep);

                                if (key < previous_end)
                                        goto out;
                                previous_end = key;
                                if (key > (u32_t)logical)
                                        break;
                                selected = ep;
                        }

                        physical = ((u64_t)ext4_le16(selected + 8) << 32) |
                            ext4_le32(selected + 4);
                }

                if (physical == 0 ||
                    physical >= rip->i_sp->s_blocks_count64)
                        break;

                next = get_block(rip->i_dev, physical, how);
                if (next == NULL)
                        break;

                if (bp != NULL)
                        put_block(bp);
                bp = next;
                node = (const u8_t *)b_data(bp);
                node_size = rip->i_sp->s_block_size;
                expected_depth = depth - 1;
                have_expected = 1;
        }

out:
        if (bp != NULL)
                put_block(bp);
        return result;
}
