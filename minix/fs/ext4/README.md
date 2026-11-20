# MINIX ext4 file server

This is an experimental ext4 file server kept separate from the existing
ext2 server. It supports read-only mounts and a deliberately restricted
read-write mode.

## Implemented

- Extent trees stored in the inode and in external extent nodes (read path,
  depth up to 5).
- Written extents and unwritten extents (unwritten ranges read as zeroes).
- 64-bit physical block numbers, 64-byte group descriptors, and high block
  counters from the ext4 superblock. Group descriptor bitmap and inode-table
  pointers are range-checked before any per-group metadata blocks are read.
  Cross-group bitmap/inode-table overlap is rejected, while `flex_bg`
  placement outside the nominal group is preserved.
- 64-bit regular-file sizes using the inode size-high field.
- Read-write support for the `huge_file` feature. The in-core `i_blocks`
  counter is 64-bit; writes serialize the 48-bit on-disk counter in 512-byte
  units when possible, then switch to filesystem-block units and set
  `HUGE_FILE` when the 48-bit sector representation is exceeded.
- Read-only verification of `metadata_csum` CRC32C checksums on the superblock,
  group descriptors, initialized block/inode bitmaps, Linux inodes, directory
  leaf/htree blocks, external extent-tree nodes, and external xattr blocks.
  The 48-bit `i_file_acl` pointer is range-checked and shared xattr blocks use
  the block-number-based checksum defined by Linux ext4. Bitmap checksums are
  verified for every initialized group during mount, before the filesystem is
  exposed. Uninitialized bitmaps are skipped according to their group flags.
  Both UUID-derived and `metadata_csum_seed` checksum seeds are supported.
  When both `metadata_csum` and the legacy `gdt_csum` flag are present, the
  metadata_csum algorithm is used. Unknown checksum types and non-Linux
  metadata checksum formats are rejected. An invalid inode checksum is
  reported as `EIO` and the inode is not cached.
- Directory record lengths, names, and inode numbers are bounds-checked before
  use.
- 64 KiB blocks are supported; ext4's extended `rec_len` encoding is
  decoded and encoded consistently by directory traversal and mutation paths.
- Linear directory traversal of htree leaf blocks; the dx root metadata after
  `..` is skipped in the first directory block.
- Read-write support for filesystems without a journal. On filesystems
  advertising `dir_index`, unindexed directories remain writable; lookup in
  indexed directories is supported by linear traversal, but entry insertion
  and deletion in directories carrying `EXT2_INDEX_FL` are rejected with
  `EOPNOTSUPP` until HTree maintenance is implemented. The block and inode
  bitmaps, group free counters, inode table, superblock counters, and data
  blocks are updated.
- 64-bit physical block allocation and release.
- Inline (depth-zero) extent roots can be inserted into, coalesced, and split
  while allocating, writing, and truncating files. Writes into unwritten
  inline extents convert the affected block to an initialized extent. Before
  any inline-root mutation, every physical extent is checked against the
  filesystem block range and all group block-bitmap, inode-bitmap, and
  inode-table ranges; invalid mappings fail with `EIO` before a block is
  released or the extent root is changed.
  Physical ranges in the inline root are also checked for overlap, and a
  newly allocated block is rejected if it is already mapped by that root.
  Before mutation, the depth-one writer checks the selected leaf against
  every sibling and checks every pair of sibling leaves for physical aliases;
  speculative allocations already mapped elsewhere in the tree are rejected.
- Inline extent roots automatically promote to depth-one trees when a write,
  unwritten-extent conversion, or middle-block removal needs more than the
  four inode-root extent slots. The old records are moved into a new external
  leaf and the added metadata block is included in `i_blocks`.
- Depth-one extent leaves are split when an edit needs more records than the
  selected leaf can hold, provided the inode root has a free index slot. The
  upper half is moved to a new leaf and a sorted root index entry is inserted.
  Updates keep the index key synchronized when a leaf's first extent changes.
  After writes and truncation, adjacent leaves are merged when their combined
  records fit in one block; physically and logically contiguous boundary
  extents are coalesced as part of the merge. When one remaining leaf fits in
  the inode root, the tree is demoted back to an inline root and its external
  metadata block is freed. Empty leaves are detached and freed, and the
  surviving neighbors are reconsidered for merging/demotion. A depth-two tree
  with exactly one inode-root index entry is also writable: its external
  depth-one index node can direct the same leaf insertion, splitting,
  unwritten conversion, removal, and compaction operations. The root key is
  propagated when the first child changes; the external index node is copied
  back into the inode when its entries fit, or freed when emptied. Depth-two
  trees with multiple inode-root entries and trees deeper than depth two
  remain read-only for mutation.
- Final unlink of ext4 extent-backed files is supported for inline roots
  and validated depth-one trees. Before removing the last directory entry,
  mappings are checked for valid physical ranges, non-aliasing, and
  containment within the file's logical size. The preflight verifies that
  `i_blocks` exactly matches data and external extent-node blocks and that an
  external xattr block has a valid header and reference count without aliasing
  file data or extent nodes. Truncation frees data blocks and external leaves;
  final inode reclamation decrements a shared xattr block reference or frees
  the block when its last reference is released. A depth-two tree with a
  single inode-root index entry is also preflighted and reclaimed, provided
  every mapping lies within EOF and the full data/node `i_blocks` accounting
  matches. Legacy indirect-mapped files with external xattr blocks remain
  conservatively rejected because their block-ownership walk is not
  implemented; special inodes without data mappings may still release a
  validated xattr block. Multi-entry depth-two roots, deeper trees, and
  mappings beyond EOF remain unsupported for final unlink.
- If truncation fails unexpectedly during inode reclamation, the inode bitmap
  is not released. The zero-link inode is kept allocated for later fsck/recovery
  rather than allowing its still-referenced blocks to be reallocated.
- New regular files, directories, and long symlinks are initialized with an
  inline extent root. Fast symlinks continue to store their target in the inode.

## Read-write requirements and limitations

Read-write mode is intentionally limited. Use a clean, unmounted test volume
and keep a backup. It is not a crash-consistent or production-ready ext4
implementation.

- **No journal support:** writable mounts reject filesystems with
  `has_journal`. Journal replay and commit are not implemented.
- **Read-only metadata checksums:** `metadata_csum` superblock, group
  descriptor, initialized bitmap, inode, directory, external extent-node, and
  external xattr-block checksums are verified. All initialized block and inode bitmaps are scanned
  at mount time. Writable mounts reject `metadata_csum` because allocation,
  inode and directory mutation paths do not update checksums. Legacy
  `gdt_csum` without `metadata_csum` remains unsupported.
- **No HTree mutation:** a filesystem-wide `dir_index` feature bit does
  not by itself prevent a writable mount. Lookup still linearly scans indexed
  directories, but `search_dir()` rejects `ENTER` and `DELETE` for an
  individual directory carrying `EXT2_INDEX_FL`. Rename preflights both
  parent directories before changing either one, preventing a half-completed
  cross-directory rename. Unindexed directories on the same volume can be
  changed normally. HTree index maintenance and conversion of indexed
  directories to linear form are not implemented.
- **Limited external extent-tree mutation:** inline roots can grow into
  depth-one trees, and depth-one leaves can be allocated, split, merged, and
  compacted while the inode root has capacity. A depth-two tree with a single
  inode-root index entry can also mutate its depth-zero leaves through the
  existing external depth-one index node. The tree collapses back to depth
  one when the child index entries fit in the inode root. Multiple inode-root
  entries at depth two, depth-three and deeper trees, and splitting a full
  external index node into a higher level remain unsupported. Operations that
  require those changes fail without changing the tree.
- Final unlink is supported for inline and validated depth-one extent trees,
  including inodes with a valid external xattr block. External trees deeper
  than one level and depth-one trees with mappings beyond EOF are rejected
  before the final directory entry is removed.
- Existing restrictions still apply: recovery, meta_bg, MMP, inline data,
  encryption, casefold, bigalloc, orphan_file, and unsupported feature flags
  are rejected. The filesystem must be clean, with an empty legacy orphan list.
- Without `huge_file`, inode writes fail with `EFBIG` rather than silently
  truncating an `i_blocks` value that exceeds 32 bits. With `huge_file`, ext4
  can store a 48-bit sector count, or a 48-bit filesystem-block count when
  the inode's `HUGE_FILE` flag is set.
- The `metadata_csum` and `metadata_csum_seed` features are supported only
  for read-only mounts.
- The server does not implement ext4 journaling, orphan recovery, extent-tree
  rebalancing, metadata checksum updates, quota updates, or the full set of
  ext4 features. A power loss or server crash during metadata updates can leave
  the volume inconsistent.

Do not use writable mode on valuable data. Run `e2fsck` on the unmounted
volume after every test session.

## Build and test

The normal `minix/fs` build includes this server. Start the installed server
and mount a disposable ext4 volume read-write (device name and service path
may vary):

```sh
service up /service/ext4
mount -t ext4 /dev/c0d0p1 /mnt
```

A host-side test image can be prepared with e2fsprogs. The feature list below
disables journaling and checksum features that this server does not update.
The `dir_index` feature can be enabled: keep the test directory unindexed for
write tests, and verify mutations in an indexed directory are rejected:

```sh
truncate -s 512M ext4-rw-test.img
mke2fs -F -t ext4 -b 4096 \
  -O extent,64bit,dir_index,^has_journal,^metadata_csum,^gdt_csum,\
^huge_file,^orphan_file ext4-rw-test.img
e2fsck -fn ext4-rw-test.img
```

A separate image exercises the 64 KiB directory-record encoding:

```sh
truncate -s 512M ext4-64k-test.img
mke2fs -F -t ext4 -b 65536 \
  -O extent,^has_journal,^dir_index,^metadata_csum,^gdt_csum,\
^huge_file,^orphan_file ext4-64k-test.img
e2fsck -fn ext4-64k-test.img
```

Also create a `huge_file` image without journaling, indexed directories, or
metadata checksums, but with the `huge_file` feature enabled:

```sh
truncate -s 512M ext4-huge-file-test.img
mke2fs -F -t ext4 -b 4096 \
  -O extent,64bit,huge_file,^has_journal,^dir_index,\
^metadata_csum,^gdt_csum,^orphan_file ext4-huge-file-test.img
```

After creating the image, use one unindexed directory for ordinary
create/rename/unlink tests and a separate sufficiently populated directory
indexed by `e2fsck -D`. Confirm lookup and getdents work in the indexed
directory, while create, unlink, and rename attempts that modify it fail with
`EOPNOTSUPP` and leave its index metadata unchanged. Confirm the unindexed
directory on the same filesystem remains writable. Run `e2fsck -fn` after
clean unmount.

Attach the images to MINIX, then test creating, writing, overwriting, appending,
sparse extension, truncating, renaming, linking, unlinking, directory creation,
and long symlinks. Include files with unwritten extents and files whose inline
extent root reaches its four-entry capacity. For the write-side
metadata guard, use a disposable image with a deliberately corrupted
depth-zero extent that points at a group block bitmap, inode bitmap, or
inode-table block. Confirm overwrite, truncate, and hole-punch operations
fail with `EIO` before changing the referenced metadata bitmap or table.
Also test extents ending exactly at the last filesystem block and extents
whose lengths would run past it. Construct inline and depth-one leaf records
whose logical ranges are distinct but whose physical block ranges overlap;
confirm write, truncate, and hole-punch attempts fail with `EIO` before
changing either extent node or releasing a block. For depth-one trees, test physical-range overlaps between the selected leaf
and each sibling, and between two non-selected siblings; all such writes must
fail with EIO before mutation. Also simulate an allocator returning a block
already mapped in the inline root,
selected leaf, or sibling leaf and confirm it is rejected.
Also create a fragmented file with more than four non-coalescing extents;
confirm the inline root is promoted to depth one and that the new leaf is
counted in `i_blocks`. Grow the file with enough isolated extents to
fill a leaf, then trigger additional insertions, unwritten-extent conversions,
and middle-block removals that require leaf splitting. Construct a depth-two
tree with a single inode-root index entry, and exercise hole writes,
unwritten-extent conversion, extent removal, leaf splitting/merging, and
depth-two-to-depth-one collapse. Confirm the external index block is counted
in and removed from `i_blocks` correctly. Final unlink must pass the
depth-two preflight and reclaim all remaining leaves and the external index
block. A depth-two tree with multiple inode-root entries must reject writes
and final unlink without mutation. Confirm both resulting
leaves remain readable and writable, their root keys are ordered, and
`i_blocks` includes every external leaf. Then remove extents so adjacent
leaves fit together; verify the right-hand leaf is freed, the remaining
root keys stay sorted, and contiguous boundary extents coalesce only when
their written/unwritten state and physical ranges agree. Reduce a one-leaf
tree to four or fewer extents and confirm it is demoted to an inline root and
its metadata block is freed. Also remove an entire leaf and verify the
surviving neighboring leaves are reconsidered for merging/demotion. Fill all
inode-root index slots and verify a
further split request fails without changing either leaf or the index. Unlink files
with one and multiple external leaves and confirm all data and leaf blocks are
freed; then check free-block/free-inode counts with `e2fsck -fn`. Construct a
depth-one file with a mapping beyond EOF and confirm final unlink fails before
removing its name. Create an external xattr block with refcount one and with
multiple references; final unlink must respectively free the block or decrement
the reference count, clear the inode pointer, and preserve unrelated references.
Corrupt the xattr magic, h_blocks, refcount, and pointer range and confirm final
unlink fails before removing its name. Repeat the shared and unshared xattr
cases when rename replaces an existing destination; the rename must preflight
before changing either name. Also test an xattr pointer that aliases a data
extent, an extent leaf, or group metadata. Inject an extent-node read/write
failure during final reclamation and confirm the inode bitmap is not released. Confirm depth-one leaf splits succeed when a root index slot is available;
requests that would require depth-two index-node creation fail without
changing the extent tree. Depth-two and deeper trees must remain readable but
reject mutation and final unlink. For the 64 KiB image, exercise
directory enumeration and enough create/unlink operations to split and merge
records. Unmount cleanly and run `e2fsck -fn` on the host after each test.
For the `huge_file` image, verify ordinary writes keep the on-disk high
`i_blocks` field and inode flag consistent. Confirm the extent write path
accepts an allocation that moves the in-core sector count beyond 2^32 when
`huge_file` is enabled, while the same increment is rejected without that
feature. Exercise encoder boundary values at 2^32 and 2^48 sectors, and the
switch to filesystem-block units above 2^48 sectors; reject values exceeding
the 48-bit filesystem-block limit before changing the extent tree.

For metadata checksum coverage, create a disposable Linux ext4 filesystem
with `metadata_csum` enabled, mount it read-only, and exercise directories,
indexed directories, nested external extent trees, and large inode tables.
Include groups with both initialized and uninitialized block/inode bitmaps.
Create external xattr blocks by adding enough attributes to files to exceed
inode-body xattr space; repeat identical attribute sets on multiple files to
exercise shared xattr blocks. Read/stat those inodes, then corrupt the xattr
block checksum and confirm the server reports `EIO`. Also corrupt one copy of
each other checksum-protected structure, including each bitmap type, and
confirm the server rejects the affected metadata rather than following it.
For group-descriptor pointer validation, also test an image with `flex_bg`
enabled (valid metadata may be outside its nominal group). In images both
with and without `metadata_csum`, corrupt block-bitmap, inode-bitmap, and
inode-table pointers to lie outside the filesystem. When checksums are enabled,
recompute the group-descriptor checksum so the range validator is reached.
Confirm invalid ranges are rejected before any pointed-to block is read.
Also construct descriptors whose pointers are individually in range but alias
another group's bitmap or inode-table extent; confirm the mount rejects these
cross-group overlaps without rejecting a valid `flex_bg` layout.
Use `e2fsck -fn` on the unmounted image after each test.

The large-file read path should be tested with files larger than 2 TiB on
4 KiB-block volumes and with sizes near the 32-bit logical-block boundary on
larger-block volumes. Verify reported `st_blocks` in 512-byte units. As with
the rest of this experimental server, run `e2fsck -fn` after clean unmount.

The 64 KiB directory-record, `huge_file`, and `metadata_csum` extensions have not yet
been compiled or runtime-tested in a MINIX build environment. Treat this as a
review candidate and test it on disposable images only.
