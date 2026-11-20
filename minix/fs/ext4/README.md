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
- Depth-one extent trees can be edited in place when the selected external
  leaf already has capacity. The inode index is kept in sync when a leaf's
  first extent changes, and an empty leaf is detached and freed. Operations
  that require leaf splitting or tree rebalancing are rejected; deeper trees
  remain read-only for mutation.
- Final unlink of ext4 extent-backed files is supported for inline roots
  and validated depth-one trees. Before removing the last directory entry,
  all mappings are checked for valid physical ranges, non-aliasing, and
  containment within the file's logical size. The preflight also verifies
  that `i_blocks` exactly matches data and external extent-node blocks.
  Truncation frees the data blocks and detaches/frees each external leaf.
  Depth-two and deeper trees, mappings beyond EOF, and inodes with external
  xattr blocks are rejected before the name is removed.
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
- **Limited external extent-tree mutation:** depth-zero inode roots and
  depth-one trees with pre-existing leaf nodes are writable. Depth-one updates
  require enough free slots in the selected leaf; this server does not
  allocate/split external nodes or rebalance trees. Such operations return
  `EOPNOTSUPP` or `EFBIG` without modifying the extent tree. Trees deeper
  than one external level remain read-only for mutation.
- Final unlink is supported for inline and validated depth-one extent trees.
  External trees deeper than one level, depth-one trees with mappings beyond
  EOF, and inodes referencing external xattr blocks are rejected before the
  final directory entry is removed.
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
Also create fragmented files
whose extent roots grow to depth one; overwrite unwritten extents, allocate
holes within existing leaves, trim extents at both ends, split an extent by
punching a middle block, and remove all mappings from one leaf. Unlink files
with one and multiple external leaves and confirm all data and leaf blocks are
freed; then check free-block/free-inode counts with `e2fsck -fn`. Construct a
depth-one file with a mapping beyond EOF and confirm final unlink fails before
removing its name. Inject an extent-node read/write failure during final
reclamation and confirm the inode bitmap is not released. Confirm updates
that would require a full leaf or tree split fail without changing the inode
or leaf metadata. Depth-two and deeper trees must remain readable but reject
mutation and final unlink. For the 64 KiB image, exercise
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
