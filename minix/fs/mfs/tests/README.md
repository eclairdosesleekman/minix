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

Orphan-list recovery and orphan-based large truncate/unlink cleanup are Phase
5 work in the design. Until then, the MFS4 mount path refuses a volume with a
non-empty orphan list. Large truncate operations are still constrained by the
64-image transaction limit and need the planned orphan/chunk implementation.
