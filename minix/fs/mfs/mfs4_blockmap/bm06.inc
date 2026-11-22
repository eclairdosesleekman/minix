		if (saved[i] == 0)
			continue;
		if (!mfs4_valid_block(map, saved[i]))
			return EIO;
		r = mfs4_free_tree(map, saved[i], level - 1);
		if (r != 0)
			return r;
	}
	return mfs4_blockmap_free(map, block);
}

static uint64_t
mfs4_level_span(unsigned int level)
{
	uint64_t span = 1;

	while (level-- > 0)
		span *= MFS4_BM_FANOUT;
	return span;
}

/*
 * Remove every allocated descendant in [first, end).  A wholly covered
 * subtree is freed without walking through intervening sparse logical holes.
 */
static int
mfs4_prune_node(struct mfs4_blockmap *map, uint32_t block,
    unsigned int level, uint64_t node_first, uint64_t first, uint64_t end,
    int *empty_out)
{
	uint32_t pointers[MFS4_BM_FANOUT];
	uint64_t child_span, child_first, child_end;
	unsigned int i;
	int changed = 0, r;

	if (!mfs4_valid_block(map, block))
		return EIO;
	r = mfs4_read_ptrs(map, block, pointers);
	if (r != 0)
		return r;
	child_span = mfs4_level_span(level - 1);
	for (i = 0; i < MFS4_BM_FANOUT; i++) {
		uint32_t child = pointers[i];
		int whole, child_empty;

		if (child == 0)
			continue;
		if (!mfs4_valid_block(map, child))
			return EIO;
		child_first = node_first + (uint64_t)i * child_span;
		child_end = child_first + child_span;
		if (child_end <= first || child_first >= end)
			continue;
		whole = first <= child_first && end >= child_end;
		if (level == 1) {
			pointers[i] = 0;
			changed = 1;
			r = mfs4_write_ptrs(map, block, pointers);
			if (r != 0)
				return r;
			r = mfs4_blockmap_free(map, child);
			if (r != 0)
				return r;
			continue;
		}
		if (whole) {
			pointers[i] = 0;
			changed = 1;
			r = mfs4_write_ptrs(map, block, pointers);
			if (r != 0)
				return r;
			r = mfs4_free_tree(map, child, level - 1);
			if (r != 0)
				return r;
		} else {
			r = mfs4_prune_node(map, child, level - 1,
			    child_first, first, end, &child_empty);
			if (r != 0)
				return r;
			if (child_empty) {
				pointers[i] = 0;
				changed = 1;
				r = mfs4_write_ptrs(map, block, pointers);
				if (r != 0)
					return r;
				r = mfs4_blockmap_free(map, child);
				if (r != 0)
					return r;
			}
		}
	}
	(void)changed;
	*empty_out = mfs4_empty_ptrs(pointers);
	return 0;
}

static int
mfs4_prune_root(struct mfs4_blockmap *map,
    uint32_t inode_pointers[MFS4_BM_INODE_POINTERS],
    unsigned int root_slot, unsigned int level, uint64_t root_first,