#ifndef _MFS4_ALLOC_SCAN_H
#define _MFS4_ALLOC_SCAN_H
#include <errno.h>
#include <stdint.h>
#include <minix/mfs4_disk.h>
struct mfs4_alloc_scan_io {
	void *cookie; uint64_t bits, blocks, first_allocatable;
	int (*peek)(void *, uint64_t, const uint8_t **);   /* read-only */
	int (*stage)(void *, uint64_t, uint8_t **);        /* tx image */
};
static inline int
mfs4_alloc_scan(const struct mfs4_alloc_scan_io *io, uint64_t origin, uint64_t *bit_out)
{
	uint64_t sc, step, idx, bi, by; const uint8_t *peek; uint8_t *image, mask; int r;
	if (io->bits <= io->first_allocatable || io->blocks == 0) return ENOSPC;
	sc = io->bits - io->first_allocatable;
	if (origin < io->first_allocatable || origin >= io->bits) origin = io->first_allocatable;
	for (step = 0; step < sc; step++) {
		idx = io->first_allocatable + (origin - io->first_allocatable + step) % sc;
		bi = idx / MFS4_BITS_PER_BLOCK; if (bi >= io->blocks) break;
		by = idx % MFS4_BITS_PER_BLOCK;
		if ((r = io->peek(io->cookie, bi, &peek)) != 0) return r;
		mask = (uint8_t)(1U << (by & 7));
		if ((peek[by >> 3] & mask) == 0) {
			if ((r = io->stage(io->cookie, bi, &image)) != 0) return r;
			image[by >> 3] |= mask; *bit_out = idx; return 0;
		}
	}
	return ENOSPC;
}
#endif
