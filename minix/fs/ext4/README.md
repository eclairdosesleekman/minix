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
- Verification and write-time regeneration of legacy `gdt_csum` group-descriptor checksums using ext4's CRC16 algorithm. Both 32-byte descriptors and the 64-byte descriptors used with `64bit` are covered. Read-write mounts can use `gdt_csum` when `metadata_csum` is absent; descriptor checksums are recomputed after updated group counters are serialized. `metadata_csum` takes precedence when both features are present.
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
  reported as `EIO` and the inode is not cached. Inode checksums are also
  regenerated on inode writeback; initialized block/inode bitmap checksums
  and group-descriptor checksums are regenerated when allocation counters are
  committed by `write_super()`. Directory, extent-node, and external-xattr
  checksum mutation paths remain incomplete, so writable mounts still reject
  `metadata_csum`.
- Directory record lengths, names, and inode numbers are bounds-checked before
  use.
- 64 KiB blocks are supported; ext4's extended `rec_len` encoding is
  decoded and encoded consistently by directory traversal and mutation paths.
- Linear directory traversal of htree leaf blocks; the dx root metadata after
  `..` is skipped in the first directory block. The `large_dir` incompat
  feature is accepted for read-only mounts: internal HTree nodes are skipped
  as inode-zero dummy directory entries, so lookup and readdir do not depend
  on the HTree depth. The root's depth field is checked against the ext4
  limit (two levels normally, three with `large_dir`), even without
  `metadata_csum`.
- Inline-data support for regular files and symlinks. The first 60 payload
  bytes are stored in `i_block`; an optional remainder is stored in the
  inode-body `system.data` xattr. The xattr header, entry table, terminator,
  value bounds, creator OS, inode size, and inode checksum (when enabled) are
  validated before the payload is exposed. Regular-file writes and truncation
  update the existing inline payload in place, but cannot grow the on-disk
  `system.data` value or exceed the capacity already present in the inode.
  Inline directories, EA-inode-backed values, and VM page mapping remain
  unsupported.
- Read-write support for filesystems without a journal and without indexed
  directories. The block and inode bitmaps, group free counters, inode table,
  superblock counters, and data blocks are updated.
- 64-bit physical block allocation and release.
- Inline extent roots and existing external extent-tree leaves can be updated
  in place while allocating, writing, and truncating files. External-leaf
  updates are bounded: they must preserve the leaf's first logical-block key
  and fit in the existing leaf. Writes into unwritten extents convert the
  affected block to an initialized extent.
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
  `gdt_csum` without `metadata_csum` is verified at mount and its CRC16
  checksum is regenerated whenever group descriptors are written, so it is
  supported on read-write mounts.
- **No indexed-directory updates:** writable mounts reject `dir_index`
  and `large_dir`; htree directory updates are not implemented. Both
  features can be read on a read-only mount, including multi-level
  `large_dir` indexes.
- **Bounded extent-tree mutation:** an external leaf can be edited only when
  the operation fits in its existing slots and does not change its first
  logical-block key. Operations that need a new node, split/rebalance a node,
  insert before a leaf's first extent, or remove the first block of a leaf
  return `EOPNOTSUPP` or `EFBIG`. These limits avoid changing parent index
  entries without a complete tree-rebalancing implementation.
- Removing the final name of an inode with an external extent tree is rejected
  rather than freeing the inode while leaking its data blocks.
- Existing restrictions still apply: recovery, meta_bg, MMP, inline
  directories, inline-data xattr growth, encryption, casefold, bigalloc,
  orphan_file, and unsupported feature flags are rejected. The filesystem
  must be clean, with an empty legacy orphan list.
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
disables journaling, indexed directories, and checksum features that this
server does not update:

```sh
truncate -s 512M ext4-rw-test.img
mke2fs -F -t ext4 -b 4096 \
  -O extent,64bit,^has_journal,^dir_index,^metadata_csum,^gdt_csum,\
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

Attach the images to MINIX, then test creating, writing, overwriting, appending,
sparse extension, truncating, renaming, linking, unlinking, directory creation,
and long symlinks. Include files with unwritten extents and files whose
inline extent root reaches its four-entry capacity. To exercise external-leaf
updates, first populate a disposable image on Linux with a fragmented file
containing more than four extents, then attach that image to MINIX; this server
does not yet create or split external extent nodes. On that file, exercise
writes into holes within a leaf, unwritten-extent conversion, and tail
truncation while keeping the first extent of each leaf intact. Operations that
require changing a leaf's first logical-block key or splitting a full leaf
must fail without damaging the filesystem. Unmount cleanly and run
`e2fsck -fn`. For the 64 KiB image, exercise
directory enumeration and enough create/unlink operations to split and merge
records. Unmount cleanly and run `e2fsck -fn` on the host after each test.
For the `huge_file` image, verify ordinary writes keep the on-disk high
`i_blocks` field and inode flag consistent. Confirm the extent write path
accepts an allocation that moves the in-core sector count beyond 2^32 when
`huge_file` is enabled, while the same increment is rejected without that
feature. Exercise encoder boundary values at 2^32 and 2^48 sectors, and the
switch to filesystem-block units above 2^48 sectors; reject values exceeding
the 48-bit filesystem-block limit before changing the extent tree.

For inline-data coverage, create a disposable Linux ext4 image with
the `inline_data` feature enabled, no journal, and metadata checksums disabled
for read-write testing:

```sh
truncate -s 128M ext4-inline-test.img
mke2fs -F -t ext4 -b 4096 -I 256 \
  -O extent,inline_data,^has_journal,^metadata_csum,^dir_index,\
^orphan_file ext4-inline-test.img
e2fsck -fn ext4-inline-test.img
```

Populate it on Linux with short and 60-byte-boundary regular files, files
whose payload extends into the `system.data` xattr, empty inline files, and
inline symlinks. Confirm reads return exact contents at offsets spanning the
60-byte boundary and at EOF. On a read-write mount, overwrite bytes in short
files and in existing `system.data` payloads, extend a file only up to its
already allocated inline payload capacity, and shrink/extend it with
truncate. Writes or truncates beyond the current inline capacity must fail
without changing the inode. Corrupt the inode-body xattr header, entry
terminator, duplicate `system.data` entry, value offset/size, or inode size
independently and confirm the inode is rejected with `EIO`. Inline directories, payloads larger than the in-core 256-byte bound, external
xattr-inode values, growth requiring a larger `system.data` xattr value, and
page mapping must remain unsupported. Run
`e2fsck -fn` on the unmounted image after testing.

For legacy `gdt_csum` coverage, create disposable ext4 images with
`metadata_csum` disabled. Test both 32-byte and 64-byte group descriptors:

```sh
truncate -s 512M ext4-gdt-csum-test.img
mke2fs -F -t ext4 -b 4096 \
  -O extent,gdt_csum,^metadata_csum,^has_journal,^dir_index,\
^64bit,^huge_file,^orphan_file ext4-gdt-csum-test.img

truncate -s 512M ext4-gdt-csum-64bit-test.img
mke2fs -F -t ext4 -b 4096 \
  -O extent,64bit,gdt_csum,^metadata_csum,^has_journal,^dir_index,\
^huge_file,^orphan_file ext4-gdt-csum-64bit-test.img
```

Mount both images read-only and verify normal directory and inode reads.
Corrupt a group's descriptor payload and its stored `bg_checksum`
independently; each image must be rejected before the affected group metadata
is used. A read-write mount must be rejected without changing the image.
Run `e2fsck -fn` on each unmounted image after testing.

For writable legacy `gdt_csum` coverage, create two disposable images with
checksums enabled and metadata_csum/journaling/indexed directories disabled:

```sh
truncate -s 512M ext4-gdt-csum-rw-test.img
mke2fs -F -t ext4 -b 4096 \
  -O extent,gdt_csum,^metadata_csum,^has_journal,^dir_index,^64bit,\
^huge_file,^orphan_file ext4-gdt-csum-rw-test.img

truncate -s 512M ext4-gdt-csum-rw-64bit-test.img
mke2fs -F -t ext4 -b 4096 \
  -O extent,64bit,gdt_csum,^metadata_csum,^has_journal,^dir_index,\
^huge_file,^orphan_file ext4-gdt-csum-rw-64bit-test.img

e2fsck -fn ext4-gdt-csum-rw-test.img
e2fsck -fn ext4-gdt-csum-rw-64bit-test.img
```

Attach each image to MINIX and mount it read-write. Create and remove files
and directories so that block, inode, and used-directory counters change.
Unmount cleanly, then run `e2fsck -fn` on both images. Repeat after enough
operations to cross group boundaries. Verify that the 32-byte and 64-byte
group descriptors both pass checksum validation and that a deliberately
corrupted `bg_checksum` still causes a read-only mount to fail.

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

For read-only `large_dir` coverage, create an ext4 image with
`large_dir` and `dir_index` enabled (and `orphan_file` disabled), populate
a directory on Linux until `debugfs -R 'htree_dump /path' image` reports a
three-level HTree, then run `e2fsck -fn`. Attach the image to MINIX and verify
both pathname lookup and `getdents`/directory listing across the full
directory. Include enough entries to force enumeration across multiple leaf
and internal-index blocks. Mount read-only; modifying indexed directories is
still intentionally rejected. Run `e2fsck -fn` again after testing.

The large-file read path should be tested with files larger than 2 TiB on
4 KiB-block volumes and with sizes near the 32-bit logical-block boundary on
larger-block volumes. Verify reported `st_blocks` in 512-byte units. As with
the rest of this experimental server, run `e2fsck -fn` after clean unmount.

The 64 KiB directory-record, `huge_file`, `metadata_csum`, bounded
external-extent-leaf update, and writable legacy `gdt_csum` changes have not
yet been compiled or runtime-tested in a MINIX build environment. Treat this
as a review candidate and test it on disposable images only.
