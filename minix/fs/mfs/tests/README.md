# MFS4 Phase 3 block-map component tests

Run the portable host suite from this directory:

    ./run-mfs4-blockmap-test.sh

The suite compiles mfs4_blockmap.c with an in-memory block-I/O backend. It
covers bitmap allocation (including a multi-block bitmap), reserved root-block
protection, bad-pointer rejection, direct/single/double/triple-indirect
boundaries, the 1 TiB logical-size boundary, sparse I/O above 4 GiB, subtree
pruning, and retained-tail zeroing during truncate/extension.

## Important scope distinction

This is a component test for the callback-based mfs4_blockmap.c API. The
active MFS VFS read/write path currently dispatches through
mfs4_read_map_checked() and mfs4_write_map() in mfs4_map.c (called by
read.c and write.c). The host suite does not execute that
inode/LMFS/journal-integrated runtime path, so its passing result must not be
used as end-to-end evidence for MFS4 read/write/truncate.

Phase 4 transaction wrappers and redo-journal integration are in
table.c, mfs4_journal.c, and mfs4_journal_core.c; those paths are
covered separately by minix/tests/mfs4/mfs4_journal_test.c. That journal
suite also does not replace VFS/QEMU/device crash testing.

## Phase 5 orphan handling

The MFS4 VFS path now uses `mfs4_orphan.c` for journaled orphan insertion,
chunked data/indirect reclamation, last-close cleanup, writable-mount recovery,
and unmount's empty-list check. `I4_ORPHAN` and `I4_TRUNCATING` are validated
by the runtime inode codec. A cleanup chunk covers at most 32 logical blocks,
leaving room under the journal's 64-unique-image ceiling for fragmented zmap
images, indirect pointer blocks, and the inode image.

The host orphan test exercises the portable state-block CRC/generation/head
helpers and the open-reference reclaim policy. It does not instantiate the
MFS inode cache, LMFS buffer cache, VFS callbacks, or device flush/recovery
path. Those still require a MINIX runtime/QEMU test and crash injection.

MFS4 v1 xattr-chain encoding/cleanup is not implemented yet. Linked-inode
truncate preserves its xattr chain and clears the truncate orphan state.
For a zero-link orphan with nonzero `xattr_head`, the worker returns
`EOPNOTSUPP` and preserves the inode/orphan record rather than freeing an
inode with unhandled xattrs.
