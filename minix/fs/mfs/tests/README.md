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

The MFS4 orphan worker validates and frees a zero-link inode's XAT4
chain in its final journal transaction before clearing the inode and orphan
record. The standalone host orphan test does not execute this MFS inode-cache,
xattr, LMFS, or journal integration; end-to-end crash recovery remains a
QEMU/device release test.


## Phase 6 xattr format and API

MFS4 XAT4 xattr blocks are stored in the regular data area, allocated through
zmap, and written only through redo-journal shadow images. The 32-byte XAT4
header and 8-byte aligned TLV stream are defined in
`minix/include/minix/mfs4_disk.h`. The portable parser validates block CRC,
owner inode and generation, flags/reserved fields, TLV boundaries, namespace
names, duplicate names, and the 16-block chain limit.

MFS4 get/set/list/remove callbacks are connected through the FS request
numbers and `struct fsdriver` callbacks to VFS. The libc layer implements the
path, no-follow path, and descriptor variants declared by `<sys/xattr.h>`.
The public API in this phase exposes only `user.*`; the two
`system.posix_acl_*` names are accepted by the MFS storage codec for the
Phase 7 ACL layer but are not exposed by the public Phase 6 VFS API.
`trusted.*` and `security.*` remain unsupported.

A metadata update creates the replacement XAT4 chain in shadow images, switches
the inode `xattr_head`, and frees the previous chain in one transaction.
With at most 16 chain blocks, at most 16 new and 16 old zmap images, plus the
inode image, the operation is bounded at 49 unique images (below the journal's
64-image limit). Final orphan reclamation also frees a zero-link inode's
validated xattr chain inside the final orphan transaction.

The `mfs4_xattr_test` host suite covers TLV append/find/remove, empty values,
the maximum name/value that exactly fills a block, namespace restrictions,
duplicate rejection, malformed TLVs, CRC corruption, and owner/generation
mismatch. It tests the portable codec only. It does not exercise the VFS
syscalls, grant-based FS protocol, LMFS, MFS inode cache, journaled block
allocation on a mounted volume, or power-loss recovery. These require the
MFS server build and end-to-end/QEMU testing before Phase 6 acceptance.
