struct mfs4_alloc_scan_io {
	void *cookie; uint64_t bits, blocks, first_allocatable;
	int (*peek)(void *, uint64_t, const uint8_t **);   /* read-only */
	int (*stage)(void *, uint64_t, uint8_t **);        /* tx image */
};
static inline int mfs4_alloc_scan(const struct mfs4_alloc_scan_io *io,
    uint64_t origin, uint64_t *bit_out)
{
	/* ... Scanning is only peek(). Reserve one image using stage() only when a free bit is found ... */
}
