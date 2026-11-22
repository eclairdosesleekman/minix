				if (parent_ptrs[parent_idx] != 0) {
					r = EAGAIN;
					goto rollback;
				}
				parent_ptrs[parent_idx] = fresh;
				r = mfs4_write_ptrs(map, parent, parent_ptrs);
				if (r != 0)
					goto rollback;
			}
			current = fresh;
		}

		if (!mfs4_valid_block(map, current)) {
			r = EIO;
			goto rollback;
		}
		r = mfs4_read_ptrs(map, current, pointers);
		if (r != 0)
			goto rollback;
		if (level + 1 == path.levels) {
			uint32_t old = pointers[path.index[level]];
			if (old != 0 && old != physical_block) {
				r = EEXIST;
				goto rollback;
			}
			if (old == physical_block)
				return 0;
			pointers[path.index[level]] = physical_block;
			r = mfs4_write_ptrs(map, current, pointers);
			if (r != 0)
				goto rollback;
			return 0;
		}
		{
			uint32_t child = pointers[path.index[level]];
			if (child == 0) {
				/* Save the current parent for the next allocated level. */
				current_parent = current;
				current = 0;
				continue;
			}
			if (!mfs4_valid_block(map, child)) {
				r = EIO;
				goto rollback;
			}
			current = child;
		}
	}
	r = EIO;

rollback:
	/* Best-effort unlink/free of only the nodes created by this insertion. */
	cleanup_error = 0;
	for (j = created_count; j > 0; j--) {
		unsigned int n = j - 1;
		uint32_t fresh = created[n];
		if (parents[n] == 0) {
			if (inode_pointers[path.root_slot] == fresh)
				inode_pointers[path.root_slot] = 0;
		} else {
			uint32_t pp[MFS4_BM_FANOUT];
			if (mfs4_read_ptrs(map, parents[n], pp) == 0 &&
			    pp[parent_indexes[n]] == fresh) {
				pp[parent_indexes[n]] = 0;
				if (mfs4_write_ptrs(map, parents[n], pp) != 0)
					cleanup_error = EIO;
			}
		}
		if (mfs4_blockmap_free(map, fresh) != 0)
			cleanup_error = EIO;
	}
	return cleanup_error != 0 ? cleanup_error : r;
}

static int
mfs4_free_tree(struct mfs4_blockmap *map, uint32_t block,
    unsigned int level)
{
	uint32_t pointers[MFS4_BM_FANOUT], saved[MFS4_BM_FANOUT];
	uint32_t zero[MFS4_BM_FANOUT];
	unsigned int i;
	int r;

	if (!mfs4_valid_block(map, block))
		return EIO;
	if (level == 0)
		return mfs4_blockmap_free(map, block);
	r = mfs4_read_ptrs(map, block, pointers);
	if (r != 0)
		return r;
	memcpy(saved, pointers, sizeof(saved));
	for (i = 0; i < MFS4_BM_FANOUT; i++)
		if (saved[i] != 0 && !mfs4_valid_block(map, saved[i]))
			return EIO;
	memset(zero, 0, sizeof(zero));
	/* Clear the child list before freeing any referenced blocks. */
	r = mfs4_write_ptrs(map, block, zero);
	if (r != 0)
		return r;
	for (i = 0; i < MFS4_BM_FANOUT; i++) {