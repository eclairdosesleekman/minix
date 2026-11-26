static inline int
mfs4_alloc_scan(const struct mfs4_alloc_scan_io *io, uint64_t origin, uint64_t *bit_out)
{
    uint64_t search_count, step, index, block_index, byte_index;
    const uint8_t *peek; uint8_t *image, mask; int r;
    if (io->bits <= io->first_allocatable || io->blocks == 0) return ENOSPC;
    search_count = io->bits - io->first_allocatable;
    if (origin < io->first_allocatable || origin >= io->bits) origin = io->first_allocatable;
    for (step = 0; step < search_count; step++) {
        index = io->first_allocatable + (origin - io->first_allocatable + step) % search_count;
        block_index = index / MFS4_BITS_PER_BLOCK;
        if (block_index >= io->blocks) break;
        byte_index = index % MFS4_BITS_PER_BLOCK;
        if ((r = io->peek(io->cookie, block_index, &peek)) != 0) return r;   /* read-only */
        mask = (uint8_t)(1U << (byte_index & 7));
        if ((peek[byte_index >> 3] & mask) == 0) {
            if ((r = io->stage(io->cookie, block_index, &image)) != 0) return r; /* 1画像のみ */
            image[byte_index >> 3] |= mask;
            *bit_out = index; return 0;
        }
    }
    return ENOSPC;
}
