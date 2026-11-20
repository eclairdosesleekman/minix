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
- Read-only support for the `huge_file` feature, including the 48-bit `i_blocks` counter and the per-inode `HUGE_FILE` block-unit flag.
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
- Read-write support for filesystems without a journal and without indexed
  directories. The block and inode bitmaps, group free counters, inode table,
  superblock counters, and data blocks are updated.
- 64-bit physical block allocation and release.
- Inline (depth-zero) extent roots can be inserted into, coalesced, and split
  while allocating, writing, and truncating files. Writes into unwritten
  inline extents convert the affected block to an initialized extent.
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
- **No indexed directories:** writable mounts reject `dir_index`; htree
  directory updates are not implemented.
- **Inline extent root only for extent mutations:** the inode root has room
  for four extent records. Allocation, unwritten-extent conversion, and
  truncation return `EOPNOTSUPP` or `EFBIG` when an external extent node or
  additional root capacity would be required. Initialized data blocks already
  mapped by an external extent tree can still be read and overwritten.
- Removing the final name of an inode with an external extent tree is rejected
  rather than freeing the inode while leaking its data blocks.
- Existing restrictions still apply: recovery, meta_bg, MMP, inline data,
  encryption, casefold, bigalloc, orphan_file, and unsupported feature flags
  are rejected. The filesystem must be clean, with an empty legacy orphan list.
- The `huge_file` feature is supported only for read-only mounts. Read-write
  mounts reject it because the current inode writer does not persist the high
  16 bits of `i_blocks`.
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

Attach the images to MINIX, then test creating, writing, overwriting, appending,
sparse extension, truncating, renaming, linking, unlinking, directory creation,
and long symlinks. Include files with unwritten extents and files whose inline
extent root reaches its four-entry capacity. For the 64 KiB image, exercise
directory enumeration and enough create/unlink operations to split and merge
records. Unmount cleanly and run `e2fsck -fn` on the host after each test.

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
