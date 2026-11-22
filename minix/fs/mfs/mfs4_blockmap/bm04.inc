	r = mfs4_make_path(logical_block, &path);
	if (r != 0)
		return r;

	if (path.levels == 0) {
		uint32_t old = inode_pointers[path.root_slot];
		if (old != 0 && physical_block != 0 && old != physical_block)
			return EEXIST;
		if (old == physical_block)
			return 0;
		inode_pointers[path.root_slot] = physical_block;
		if (old != 0)
			return mfs4_blockmap_free(map, old);
		return 0;
	}

	if (physical_block == 0) {
		uint32_t blocks[MFS4_INDIRECT_LEVELS];
		uint32_t child;

		current = inode_pointers[path.root_slot];
		if (current == 0)
			return 0;
		for (level = 0; level < path.levels; level++) {
			if (!mfs4_valid_block(map, current))
				return EIO;
			blocks[level] = current;
			r = mfs4_read_ptrs(map, current, pointers);
			if (r != 0)
				return r;
			child = pointers[path.index[level]];
			if (child == 0)
				return 0;
			if (!mfs4_valid_block(map, child))
				return EIO;
			current = child;
		}

		/* Clear the data pointer before releasing its bitmap bit. */
		r = mfs4_read_ptrs(map, blocks[path.levels - 1], pointers);
		if (r != 0)
			return r;
		child = pointers[path.index[path.levels - 1]];
		pointers[path.index[path.levels - 1]] = 0;
		r = mfs4_write_ptrs(map, blocks[path.levels - 1], pointers);
		if (r != 0)
			return r;
		r = mfs4_blockmap_free(map, child);
		if (r != 0)
			return r;

		/* Prune empty indirect blocks from the leaf toward the inode. */
		for (level = path.levels; level > 0; level--) {
			uint32_t node = blocks[level - 1];
			uint32_t parent = level == 1 ? 0 : blocks[level - 2];
			unsigned int parent_idx = level == 1 ? 0 :
			    path.index[level - 2];

			r = mfs4_read_ptrs(map, node, pointers);
			if (r != 0)
				return r;
			if (!mfs4_empty_ptrs(pointers))
				break;
			r = mfs4_detach_indirect(map, inode_pointers,
			    path.root_slot, parent, parent_idx, node);
			if (r != 0)
				return r;
		}
		return 0;
	}

	/* Insert a new pointer, allocating and zeroing missing indirect levels. */
	created_count = 0;
	current = inode_pointers[path.root_slot];
	for (level = 0; level < path.levels; level++) {
		if (current == 0) {
			uint32_t fresh;
			uint32_t parent = level == 0 ? 0 : current_parent;
			unsigned int parent_idx = level == 0 ? 0 :
			    path.index[level - 1];

			r = mfs4_blockmap_alloc(map, &fresh);
			if (r != 0)
				goto rollback;
			r = mfs4_zero_block(map, fresh);
			if (r != 0) {
				(void)mfs4_blockmap_free(map, fresh);
				goto rollback;
			}
			created[created_count] = fresh;
			parents[created_count] = parent;
			parent_indexes[created_count] = parent_idx;
			created_count++;
			if (level == 0) {
				inode_pointers[path.root_slot] = fresh;
			} else {
				uint32_t parent_ptrs[MFS4_BM_FANOUT];
				r = mfs4_read_ptrs(map, parent, parent_ptrs);
				if (r != 0)
					goto rollback;