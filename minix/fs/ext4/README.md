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
- Read-only support for ext4 `meta_bg` group-descriptor placement. The
  descriptor table is assembled in logical order from the classic contiguous
  prefix and the primary descriptor blocks in later metablock groups. A
  `resize_inode` + `meta_bg` combination is rejected, matching the
  e2fsprogs restriction.
  Descriptor checksums are verified after all descriptor blocks are gathered.
  Primary and backup descriptor blocks in the first, second, and last groups
  of each metablock group are reserved against group bitmap/inode-table
  pointers, file-data extents, external extent-tree nodes, and external xattr
  pointers. Classic-prefix descriptor copies in the primary and sparse-super
  backup groups are reserved as well.
  Writable mounts remain disabled because the current writer emits a classic
  contiguous descriptor table. Alternate-superblock selection and malformed
  `s_first_meta_bg` values are rejected.
- Read-only support for ext4 `bigalloc` clustered allocation. The superblock's
  cluster size and clusters-per-group fields are validated against the block
  geometry; block-bitmap checksum coverage uses one bit per cluster, while
  inode bitmap checksums continue to cover one bit per inode. Extent physical
  block mapping remains in filesystem-block units. File-data mappings are
  rejected when their cluster contains a backup superblock/GDT block or a
  group block bitmap, inode bitmap, or inode-table block. Metadata pointers
  are checked at block granularity because multiple metadata blocks may share
  a cluster. Writable mounts are rejected because the current allocator and
  truncate paths are not cluster-granular.
- Read-only support for ext4 `sparse_super2` backup placement. The two
  `s_backup_bgs` values are validated and used to locate backup superblocks
  and classic GDT copies; the same group-selection rules are applied to
  META_BG descriptor backups. The selected backup groups may be in either
  `s_backup_bgs` slot (zero slots are optional). Superblock, GDT, and
  `resize_inode` reserved-GDT blocks are protected from group bitmap/inode-table
  pointers, file extents, indirect mappings, external extent-tree nodes, and
  external xattr pointers. Writable mounts
  remain disabled because the current writer does not update backup copies.
- Read-only compatibility with the ext4 `ea_inode` incompat feature. The
  mount path accepts filesystems using EA inodes as extended-attribute value
  storage, while keeping those storage inodes out of the pathname namespace.
  Ordinary file data and metadata remain readable; EA-inode-backed xattr
  values are still opaque and are not exposed through a get/list-xattr API.
  Writable mounts are rejected because xattr reference counts and EA inode
  lifetimes are not maintained.
- Read-only compatibility for quota (`quota`) and project-quota (`project`)
  feature flags, and for `RO_COMPAT_READONLY`. Quota metadata is left
  untouched, so quota-enabled filesystems require read-only mounts.
  Filesystems with `orphan_file` are accepted read-only only when
  `RO_COMPAT_ORPHAN_PRESENT` is clear; pending orphan recovery is rejected.
  Orphan-file maintenance and recovery are not implemented.
- Linux ext4 extended inode timestamps are decoded and stored using ext4's
  34-bit seconds and nanosecond fields when present in `i_extra_isize`.
  `stat` reports atime/mtime/ctime nanoseconds and the inode creation time
  (`st_birthtim`); `utimes` preserves requested subsecond values when the
  corresponding extra field exists. Older or short-extra inodes retain
  seconds-only timestamps. Invalid extra sizes or nanosecond encodings are
  rejected.
- Read-write support for the `huge_file` feature. The in-core `i_blocks`
  counter is 64-bit; writes serialize the 48-bit on-disk counter in 512-byte
  units when possible, then switch to filesystem-block units and set
  `HUGE_FILE` when the 48-bit sector representation is exceeded.
- Verification and write-time regeneration of legacy `gdt_csum` group-descriptor checksums using ext4's CRC16 algorithm. Both 32-byte descriptors and the 64-byte descriptors used with `64bit` are covered. Read-write mounts can use `gdt_csum` when `metadata_csum` is absent; descriptor checksums are recomputed after updated group counters are serialized. `metadata_csum` takes precedence when both features are present.
- Verification of `metadata_csum` CRC32C checksums on the superblock,
  group descriptors, initialized block/inode bitmaps, Linux inodes, directory
  leaf/htree blocks, external extent-tree nodes, and external xattr blocks.
  The 48-bit `i_file_acl` pointer is range-checked and shared xattr blocks use
  the block-number-based checksum defined by Linux ext4. Bitmap checksums are
  verified for every initialized group during mount, before the filesystem is
  exposed. Both UUID-derived and `metadata_csum_seed` checksum seeds are
  supported. When both `metadata_csum` and legacy `gdt_csum` are present,
  the metadata_csum algorithm takes precedence. Unknown checksum types and
  non-Linux metadata checksum formats are rejected. An invalid inode checksum
  is reported as `EIO` and the inode is not cached. For the supported
  read-write paths, checksums are regenerated for the superblock, group
  descriptors, initialized allocation bitmaps, Linux inodes, ordinary
  directory leaves, and external extent-tree nodes. External xattr blocks are
  verified on read but are not modified by the current filesystem operations.
  Indexed-directory mutation remains unsupported. Writable mounts reject
  groups with `BLOCK_UNINIT` or `INODE_UNINIT`; the allocator cannot yet
  initialize lazy allocation bitmaps safely.
- Directory record lengths, names, and inode numbers are bounds-checked before
  use.
- Directory enumeration maps every ext4 directory `file_type`, including
  Unix-domain sockets (`EXT2_FT_SOCK` to `DT_SOCK`). Filesystems without the
  file-type incompat feature continue to report `DT_UNKNOWN`.
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
  Inline directories can be searched and enumerated read-only. Their
  implicit `.` and `..` entries are synthesized; the 56-byte dirent area
  after the parent inode and the `system.data` continuation are validated as
  separate regions. Inline-directory mutation, EA-inode-backed values, and
  VM page mapping remain unsupported.
- Read-write support for clean filesystems without a journal or indexed
  directories, provided every allocation bitmap is already initialized. The
  block and inode bitmaps, group free counters, inode table, superblock
  counters, and data blocks are updated.
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
- **Metadata checksums:** `metadata_csum` checksums are verified on
  mount/read and regenerated for every metadata structure modified by the
  currently supported write paths: superblock, group descriptors, initialized
  allocation bitmaps, inodes, ordinary directory leaves, and external extent
  nodes. External xattr blocks are verified but have no write/update path in
  the current server. Writable mounts reject any group with
  `BLOCK_UNINIT` or `INODE_UNINIT`, because the allocator does not yet
  synthesize the lazy-initialized bitmaps represented by those flags. Legacy
  `gdt_csum` without `metadata_csum` remains supported for read-write mounts;
  its CRC16 checksum is regenerated whenever group descriptors are written.
- **No indexed-directory updates:** writable mounts reject `dir_index`
  and `large_dir`; htree directory updates are not implemented. Both
  features can be read on a read-only mount, including multi-level
  `large_dir` indexes.
- **BIGALLOC is read-only:** block bitmap interpretation and read-only
  extent mapping understand clusters, but allocation, release, and truncation
  still operate in individual blocks. Writable mounts are therefore rejected.
- **META_BG and sparse_super2 are read-only:** primary descriptor blocks
  are loaded from their metablock-group locations, and backup superblock,
  classic GDT-prefix, and META_BG descriptor blocks are reserved using the
  actual sparse-super or sparse_super2 group selection. Writable mounts are
  rejected because descriptor writes still use the classic contiguous GDT
  layout and do not update backup copies. Alternate-superblock selection is
  rejected for META_BG and sparse_super2.
- **Quota metadata is read-only:** filesystems with `quota` or `project`
  enabled can be mounted read-only, but writable mounts are rejected because
  create, link, unlink, truncate, and allocation paths do not update user,
  group, or project quota records.
- **Orphan files are read-only:** a clean `orphan_file` filesystem can be
  mounted read-only only while `RO_COMPAT_ORPHAN_PRESENT` is clear. When
  that bit is set, mounting is rejected because orphan-file recovery is not
  implemented. Writable mounts are rejected because namespace and truncate
  operations do not maintain the orphan file.
- **Explicit read-only feature:** `RO_COMPAT_READONLY` always rejects a
  writable mount, independent of the requested mount mode.
- **Bounded extent-tree mutation:** an external leaf can be edited only when
  the operation fits in its existing slots and does not change its first
  logical-block key. Operations that need a new node, split/rebalance a node,
  insert before a leaf's first extent, or remove the first block of a leaf
  return `EOPNOTSUPP` or `EFBIG`. These limits avoid changing parent index
  entries without a complete tree-rebalancing implementation.
- Removing the final name of an inode with an external extent tree is rejected
  rather than freeing the inode while leaking its data blocks.
- Existing restrictions still apply: recovery, MMP, inline
  directories, inline-data xattr growth, encryption, casefold, and unsupported feature flags are rejected. The filesystem must be clean,
  with an empty legacy orphan list. `orphan_file` is supported only on
  read-only mounts with no pending orphan recovery, as described above.
- Without `huge_file`, inode writes fail with `EFBIG` rather than silently
  truncating an `i_blocks` value that exceeds 32 bits. With `huge_file`, ext4
  can store a 48-bit sector count, or a 48-bit filesystem-block count when
  the inode's `HUGE_FILE` flag is set.
- The server does not implement ext4 journaling, orphan recovery, extent-tree
  rebalancing, external xattr-block mutation, quota updates, or the full set of
  ext4 features. A power loss or server crash during metadata updates can leave
  the volume inconsistent.

- **Extended timestamp field requirements:** 64-bit seconds and nanoseconds
  require the relevant Linux inode extra field to fit within `i_extra_isize`.
  When an older inode lacks an atime/mtime extra field, `utimes` retains the
  seconds value and rounds subsecond precision down. Explicit dates outside
  the signed 32-bit range are rejected when the matching extended field is
  absent; dates outside ext4's 34-bit seconds range are always rejected.
  ext4's `dtime` remains the legacy 32-bit field.

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

Read-only feature compatibility can be tested with disposable images made
by e2fsprogs. The BIGALLOC, quota, project-quota, and orphan-file images
described below should all mount read-only. The quota and project images, and the clean
orphan-file image, must be rejected for read-write mounts. For the orphan-file
case, also test a copy with `RO_COMPAT_ORPHAN_PRESENT` set: it must be
rejected even read-only because recovery is pending. An image carrying
`RO_COMPAT_READONLY` must likewise reject a writable mount.

```sh
truncate -s 1G ext4-bigalloc-csum-ro.img
mke2fs -F -t ext4 -b 4096 -C 16384 \
  -O bigalloc,metadata_csum,^has_journal ext4-bigalloc-csum-ro.img
dumpe2fs -h ext4-bigalloc-csum-ro.img | grep -E 'Filesystem features|Cluster size|Block size'
e2fsck -fn ext4-bigalloc-csum-ro.img

truncate -s 1G ext4-bigalloc-nocsum-ro.img
mke2fs -F -t ext4 -b 4096 -C 65536 \
  -O bigalloc,^metadata_csum,^has_journal ext4-bigalloc-nocsum-ro.img
dumpe2fs -h ext4-bigalloc-nocsum-ro.img | grep -E 'Filesystem features|Cluster size|Block size'
e2fsck -fn ext4-bigalloc-nocsum-ro.img

truncate -s 256M ext4-quota-ro.img
mke2fs -F -t ext4 -O quota,^has_journal ext4-quota-ro.img

truncate -s 256M ext4-project-ro.img
mke2fs -F -t ext4 -O project,^has_journal ext4-project-ro.img

truncate -s 256M ext4-orphan-ro.img
mke2fs -F -t ext4 -O orphan_file,^has_journal ext4-orphan-ro.img

truncate -s 256M ext4-ea-inode-ro.img
mke2fs -F -t ext4 -O ea_inode,^has_journal ext4-ea-inode-ro.img

e2fsck -fn ext4-quota-ro.img
e2fsck -fn ext4-project-ro.img
e2fsck -fn ext4-orphan-ro.img
e2fsck -fn ext4-ea-inode-ro.img
```

On MINIX, verify pathname lookup, stat, file reads, and directory enumeration
on each read-only mount, including a BIGALLOC volume with metadata checksums
and an `ea_inode` volume containing large xattr values. Confirm ordinary file
contents remain readable, and that EA-inode storage objects cannot be reached
as directory entries. EA-inode-backed xattr get/list operations are not
implemented and must not be inferred to work.
Check that a block bitmap checksum over the cluster-bit length is accepted and
that a deliberately corrupted checksum is rejected. Writable BIGALLOC mounts
must fail before the volume is marked unclean. After unmount, run
`e2fsck -fn` on each image. The pending-orphan and `RO_COMPAT_READONLY` cases should be tested
using disposable copies with the relevant superblock bit changed and its
metadata checksum recomputed when `metadata_csum` is enabled.

For BIGALLOC, also test at least two cluster ratios (for example, 4 and 16
filesystem blocks per cluster), with and without `metadata_csum`. Confirm
that a block bitmap checksum is computed over
`s_clusters_per_group / 8` bytes, not `s_blocks_per_group / 8`, while inode
bitmap checksum coverage remains `s_inodes_per_group / 8`. Malformed cluster
logs, zero clusters-per-group, and a cluster geometry that does not multiply
back to `s_blocks_per_group` must fail the mount. Writable BIGALLOC mounts
must be rejected without changing the clean filesystem state.

For read-only sparse_super2 coverage, prepare a disposable image and inspect its
backup-group list with `dumpe2fs -h`. Confirm that the two listed groups (and
group zero) are the only groups whose superblock/GDT copies are reserved.
Also test an unused zero in either list slot, including a zero first entry
followed by a valid second entry. Nonzero out-of-range group numbers must fail
the mount; alternate-superblock selection is rejected.
Writable mounts must fail before the filesystem is marked unclean.

```sh
truncate -s 1G ext4-sparse-super2.img
mke2fs -F -t ext4 -O sparse_super2,^has_journal ext4-sparse-super2.img
dumpe2fs -h ext4-sparse-super2.img | grep -E 'Filesystem features|Backup block groups'
e2fsck -fn ext4-sparse-super2.img
```

Test sparse_super2 with and without META_BG where supported by the installed
e2fsprogs, including 32-byte and 64-byte descriptors and both checksum modes.
Run pathname lookup, file reads, directory enumeration, and `e2fsck -fn` after
unmount on disposable images.

For read-only META_BG coverage, prepare disposable images with e2fsprogs.
Test both 32-byte and 64-byte group descriptors, classic GDT prefixes with
non-zero `first_meta_bg`, and layouts where all descriptor blocks use META_BG.
Repeat with `sparse_super` enabled and disabled, and with both `metadata_csum`
and legacy `gdt_csum` where supported. Include sparse backup groups whose
numbers are powers of 3, 5, or 7. Verify pathname lookup, file reads, and
external extent-tree traversal. Corrupt a descriptor checksum and confirm the
mount fails before any bitmap or inode-table pointer from that descriptor is
followed. Corrupt bitmap/inode-table pointers to alias primary or backup
META_BG descriptor blocks and confirm mount rejects them. Construct direct
and legacy-indirect data pointers, extent records, external extent-node
pointers, and external xattr pointers that target descriptor blocks in both
the classic prefix/backups and META_BG groups; reads must fail with `EIO`.
Use an image whose group count ends part-way through a metablock group to
confirm that the final partial group does not cause out-of-range backup
descriptor reads. Confirm writable mounts, `sparse_super2` + `meta_bg`,
and alternate-superblock META_BG mounts are rejected. Run `e2fsck -fn` after
each unmounted-image test.

```sh
# 32-byte descriptors (2 descriptor blocks for 256 block groups)
truncate -s 1G ext4-meta-bg-32bit-ro.img
mke2fs -F -t ext4 -b 4096 -g 1024 \
  -O extent,meta_bg,^64bit,^resize_inode,^has_journal \
  ext4-meta-bg-32bit-ro.img

# 64-byte descriptors (4 descriptor blocks for 256 block groups),
# keeping the first descriptor block in the classic GDT prefix.
truncate -s 1G ext4-meta-bg-64bit-ro.img
MKE2FS_FIRST_META_BG=1 mke2fs -F -t ext4 -b 4096 -g 1024 \
  -O extent,64bit,meta_bg,^resize_inode,^has_journal \
  ext4-meta-bg-64bit-ro.img

# Optional: non-multiple group count to exercise a partial final metablock.
truncate -s 1100M ext4-meta-bg-partial-ro.img
MKE2FS_FIRST_META_BG=1 mke2fs -F -t ext4 -b 4096 -g 1024 \
  -O extent,64bit,meta_bg,^resize_inode,^has_journal \
  ext4-meta-bg-partial-ro.img

e2fsck -fn ext4-meta-bg-32bit-ro.img
e2fsck -fn ext4-meta-bg-64bit-ro.img
e2fsck -fn ext4-meta-bg-partial-ro.img
```

A baseline host-side test image can be prepared with e2fsprogs. It disables
journaling, indexed directories, and checksums so basic write operations can
be checked independently of checksum regeneration:

```sh
truncate -s 512M ext4-rw-test.img
mke2fs -F -t ext4 -b 4096 \
  -O extent,64bit,^has_journal,^dir_index,^metadata_csum,^gdt_csum,\
^huge_file,^orphan_file ext4-rw-test.img
e2fsck -fn ext4-rw-test.img
```

For writable `metadata_csum` coverage, prepare a separate disposable image
with initialized allocation bitmaps, no journal, and no indexed directories.
The mount path rejects groups that still carry `BLOCK_UNINIT` or
`INODE_UNINIT`; ensure the group flags are clear before attaching the image:

```sh
truncate -s 512M ext4-metadata-csum-rw-test.img
mke2fs -F -t ext4 -b 4096 \
  -O extent,64bit,metadata_csum,^has_journal,^dir_index,^orphan_file,\
^uninit_bg -E lazy_itable_init=0 ext4-metadata-csum-rw-test.img
e2fsck -fy ext4-metadata-csum-rw-test.img
e2fsck -fn ext4-metadata-csum-rw-test.img
```

Mount it read-write on MINIX and create/remove files and directories. Exercise
block/inode allocation and release, file data writes, ordinary directory
entry insertion/deletion, and writes into an existing external extent leaf.
Unmount cleanly and run `e2fsck -fn`; verify the superblock, group descriptor,
bitmap, inode, directory-leaf, and extent-node checksums all validate.
Corrupt one checksum at a time on copies of the image and confirm the mount is
rejected or the corresponding read reports `EIO`, as appropriate. Do not use
indexed directories or lazy allocation bitmaps in the writable test.

For extended timestamp coverage, use a disposable Linux-created ext4 image
with 256-byte inodes (so the normal `i_extra_isize` timestamp fields fit):

```sh
truncate -s 512M ext4-time-test.img
mke2fs -F -t ext4 -b 4096 -I 256 \
  -O extent,64bit,^has_journal,^dir_index,^metadata_csum,^gdt_csum,\
^huge_file,^orphan_file ext4-time-test.img
e2fsck -fn ext4-time-test.img
```

On MINIX, set atime/mtime to values immediately below and above the 2038
boundary, include non-zero nanoseconds, and test the maximum 34-bit ext4
seconds value. Compare `stat` results before and after an unmount/remount,
including the birth time of newly created files. Run `e2fsck -fn` after the
clean unmount. Repeat with a 128-byte-inode image: subsecond precision should
be discarded and explicit seconds outside the signed 32-bit range should
fail with `EOVERFLOW`.

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
independently and confirm the inode is rejected with `EIO`. Payloads larger
than the in-core 256-byte bound, external xattr-inode values, growth requiring
a larger `system.data` xattr value, and page mapping must remain unsupported.
For inline-directory coverage, create nested small directories so some entries
remain in the 56-byte inode area and others spill into `system.data`. Verify
lookup of ordinary names, `.`, and `..`, plus repeated getdents calls with
small output buffers and seeks to returned cookies. Corrupt the stored parent
inode and each dirent area's record lengths, inode numbers, and names; lookup
or readdir must report `EIO`. Inline-directory create, unlink, rename, and
rmdir mutations must fail without partially changing the inode or parent
directory. Run `e2fsck -fn` on the unmounted image after testing.

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

For directory-entry type coverage, populate a disposable ext4 image on Linux
with regular files, directories, symlinks, FIFOs, block/character devices,
and a bound Unix-domain socket. Mount it read-only on MINIX and inspect the
`d_type` values returned by `readdir`/`getdents`: the socket entry must report
`DT_SOCK` (not `DT_UNKNOWN`). Repeat with an image whose directory entry type
field is set to `EXT2_FT_UNKNOWN`; it must continue to report `DT_UNKNOWN`.
Run `e2fsck -fn` after testing the unmounted image.

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

The META_BG and sparse_super2 read-only layouts, extended timestamp, 64 KiB directory-record,
`huge_file`, `metadata_csum`, bounded external-extent-leaf update, and writable
legacy `gdt_csum` changes have not yet been compiled or runtime-tested in a
MINIX build environment. Treat this as a review candidate and test it on
disposable images only.
