# MINIX ext4 file server

This is an experimental ext4 file server kept separate from the existing
ext2 server. It supports read-only mounts and a deliberately restricted
read-write mode.

## Implemented

- Extent trees stored in the inode and in external extent nodes (read path,
  depth up to 5).
- Writable extent-tree root growth for a full depth-one inode root. When
  splitting a full external leaf requires a fifth inode-root index entry, the
  four existing entries and new leaf entry are moved into a new external
  depth-one index node and the inode root is promoted to depth two. The new
  index checksum (when enabled) and inode `i_blocks` are updated. Additional
  full external index-node splitting is bounded by available ancestor slots.
- Writable splitting of a full external depth-one index node beneath
  depth-two and deeper roots when its depth-two ancestor has a free entry.
  The full depth-one parent and the newly added leaf entry are divided
  between the existing node and a new sibling; the sibling index is inserted
  immediately after the old parent in the depth-two ancestor, preserving the
  old first key. The modified ancestor and both sibling index nodes have their
  checksums regenerated when `metadata_csum` is enabled. `i_blocks` accounts
  for the data block, new leaf, and new index node. If both the depth-one
  parent and its depth-two ancestor are full, a depth-three inode root with a
  free entry can accept a new depth-two sibling after both levels are split.
  At root depth four, a full depth-two ancestor can instead split beneath its
  depth-three external parent when that node has room; the root pointer and
  inherited key bounds are revalidated. A full depth-three parent, deeper
  cascades, inode-root growth beyond the documented depth-one-to-depth-two
  promotion, and root rebalancing remain unsupported.
- Written extents and unwritten extents (unwritten ranges read as zeroes).
- 64-bit physical block numbers, 64-byte group descriptors, and high block
  counters from the ext4 superblock. Group descriptor bitmap and inode-table
  pointers are range-checked before any per-group metadata blocks are read.
  Cross-group bitmap/inode-table overlap is rejected, while `flex_bg`
  placement outside the nominal group is preserved.
- 64-bit regular-file sizes using the inode size-high field.
- Full 32-bit Linux ext4 owner and group IDs. The low 16-bit fields and
  `osd2.linux2.l_i_uid_high`/`l_i_gid_high` words are decoded into 32-bit
  in-core IDs and written back independently of the adjacent high block-count
  and external-xattr pointer fields. ext2 and non-Linux ext4 creator formats
  retain their existing 16-bit UID/GID behavior.
- Legacy direct and single/double/triple-indirect block mappings are range-checked
  against the complete ext4 metadata map before any pointer block or data block
  is read. Indirect pointer blocks are checked at block granularity; mapped
  data blocks are also rejected when their BIGALLOC allocation cluster contains
  a backup superblock/GDT copy, group bitmap, or inode table. Zero pointers
  continue to represent holes.
- Read-write support for ext4 `meta_bg` group-descriptor placement.
  The descriptor table is assembled in logical order from the classic
  contiguous prefix and the primary descriptor blocks in later metablock
  groups. Descriptor checksums are verified after all descriptor blocks are
  gathered and regenerated when counters change. Primary descriptor writes
  use the first group of each metablock group rather than assuming a
  contiguous table. Primary and backup descriptor blocks in the first,
  second, and last groups of each metablock group are reserved against group
  bitmap/inode-table pointers, file-data extents, external extent-tree nodes,
  and external xattr pointers. Classic-prefix descriptor copies in the
  primary and sparse-super backup groups are reserved as well. A
  `resize_inode` + `meta_bg` combination is rejected, matching the e2fsprogs
  restriction. Normal metadata updates write primary descriptor blocks;
  online resize and regeneration of backup descriptor copies are not
  implemented. Alternate-superblock selection and malformed
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
- Read-write support for ext4 `sparse_super2` backup placement. The two
  `s_backup_bgs` values are validated and used to locate backup superblocks
  and classic GDT copies; the same group-selection rules are applied to
  META_BG descriptor backups. The selected backup groups may be in either
  `s_backup_bgs` slot (zero slots are optional). Superblock, GDT, and
  `resize_inode` reserved-GDT blocks are protected from group bitmap/inode-table
  pointers, file extents, indirect mappings, external extent-tree nodes, and
  external xattr pointers. The primary superblock and GDT retain their classic
  locations, so regular allocation and metadata updates can use the existing
  writer. As with other sparse-super layouts, ordinary writes update primary
  metadata; online resize and backup-copy regeneration are not implemented.
  Alternate-superblock selection remains unsupported.
- Read-only compatibility with the ext4 `ea_inode` incompat feature.
  EA-inode-backed values referenced from external xattr blocks and inode-body
  xattr entries are validated on inode read: the referenced inode checksum,
  regular-file type, link and reference counts, absence of nested xattrs,
  exact value size, complete data allocation, CRC32C value hash, and xattr
  entry hash are checked. Inode-body xattr entry tables and inline value
  offsets are bounded before EA inode references are followed. Legacy
  Lustre-style EA inode backpointers are recognized. EA storage inodes remain
  outside the pathname namespace, and attribute values are not exposed through
  a get/list-xattr API. Inline-data `system.data` values stored in EA inodes
  are decoded and readable on read-only mounts, but EA reference counts and
  lifetimes are not maintained. Writable mounts are rejected.

- External xattr blocks are structurally validated before an inode is exposed,
  even when `metadata_csum` is disabled. The 32-byte header, reference count,
  single-block length, zero-terminated entry array, nonempty NUL-free names,
  value sizes, and 4-byte-padded inline value ranges are checked. EA-inode
  references require the `ea_inode` incompat feature and a valid non-root
  inode number. When `metadata_csum` is enabled, the block-number-based
  CRC32C checksum is also verified. Attribute values are still not exposed
  through a get/list-xattr API.
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
  The 48-bit `i_file_acl` pointer is range-checked; external xattr entry/value
  structure is validated even without `metadata_csum`, while shared blocks also
  use the block-number-based checksum when that feature is enabled. Bitmap checksums are
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
  Indexed-directory mutation remains unsupported. Writable mounts now
  materialize groups carrying `BLOCK_UNINIT` or `INODE_UNINIT`, verify the
  generated free-bit counts, update bitmap checksums, flush bitmap blocks, and
  clear the descriptor flags before enabling writes.
- Directory record lengths, names, and inode numbers are bounds-checked before
  use.
  Live entries must also have nonempty names without NUL or slash bytes, and
  ext4 `file_type` values must be one of the eight defined encodings. Empty
  inode-zero slots and HTree placeholders remain valid.
- Directory enumeration maps every ext4 directory `file_type`, including
  Unix-domain sockets (`EXT2_FT_SOCK` to `DT_SOCK`). Filesystems without the
  file-type incompat feature continue to report `DT_UNKNOWN`.
- 64 KiB blocks are supported; ext4's extended `rec_len` encoding is
  decoded and encoded consistently by directory traversal and mutation paths.
- Indexed-directory lookup uses the ext4 HTree for the first candidate leaf
  with legacy, half-MD4, and TEA hashes (signed and unsigned variants). When
  `metadata_csum` is enabled, root and internal-node checksums are verified.
  Count/limit arrays, hash ordering, child logical-block bounds, and depth are
  validated for every traversed index node. A candidate miss falls back to
  the complete linear scan so hash-collision continuation leaves and unusual
  layouts remain discoverable. SipHash/hash-in-dirent modes continue to use
  linear lookup. Directory enumeration remains linear. Namespace mutations
  on directories carrying `EXT2_INDEX_FL` are rejected; non-indexed
  directories on the same filesystem remain writable.
- Linear directory traversal of HTree leaf blocks; the dx root metadata after
  `..` is skipped in the first directory block. The `large_dir` incompat
  feature is accepted for both read-only and writable mounts: internal HTree
  nodes are skipped as inode-zero dummy directory entries, so lookup and
  readdir support the HTree depth limit (two levels normally, three with
  `large_dir`). Namespace mutations on directories carrying
  `EXT2_INDEX_FL` remain rejected, while ordinary non-indexed directories
  and regular files can be modified on a writable mount. The root's depth
  field is checked even without `metadata_csum`.
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
  separate regions. EA-inode-backed `system.data` values are read and
  checksum/hash validated for inline files, symlinks, and directories on
  read-only mounts, up to the 64 KiB xattr-value limit. No get/list-xattr API
  is provided; inline-directory mutation and VM page mapping remain
  unsupported.
- Read-write support for clean filesystems without a journal or indexed
  directories. Lazy `BLOCK_UNINIT` and `INODE_UNINIT` bitmaps are synthesized
  from backup-super/GDT locations and each group's block bitmap, inode bitmap,
  and inode-table range, checked against group free counters, and
  persisted before their group-descriptor flags are cleared. The
  block and inode bitmaps, group free counters, inode table, superblock
  counters, and data blocks are updated.
- 64-bit physical block allocation and release.
- Extent nodes are validated across all records in each traversed node. Leaf
  entries must be sorted and non-overlapping, fit in the 32-bit logical-block
  address space, and map wholly within non-reserved filesystem blocks. Index
  keys must be strictly increasing and every child pointer must reference an
  in-range non-reserved block. When an external child node is traversed, its
  first extent or index key must equal the parent index key used to reach it,
  matching ext4's node-key invariant. Each selected child also inherits an
  exclusive logical-block upper bound from the next parent index key; leaf
  extents must end within that range, and intermediate index keys must remain
  below it. This prevents extents from overlapping the next sibling subtree.
  Empty external nodes are rejected; an empty inline root remains valid.
  Sibling subtrees are checked when traversed rather than eagerly scanned at
  mount.
- Inline extent roots and existing external extent-tree leaves can be updated
  in place while allocating, writing, and truncating files. Full external
  leaves can be split at any supported extent-tree depth when the immediate
  parent index node has a free slot. The selected leaf's exclusive logical
  upper bound is propagated from every ancestor and checked before splitting,
  including for the final child of an intermediate index node. The original
  leaf keeps its first logical-block key; the new leaf key is inserted
  immediately after it in the parent index. External parent-index and leaf
  checksums are regenerated as required. The inode block count includes the
  extra extent-tree node. Removing or shortening the first extent in an
  external leaf updates its parent key when that update does not require
  changing an ancestor key. A one-extent external leaf is removed from its
  parent and its metadata block is freed when the parent remains nonempty and
  no ancestor key must change. If it is the inode root's only external child,
  the root is collapsed back to an empty depth-zero inline extent header. At
  depth two, an intermediate node containing only the leaf being removed can
  also be detached from the inode root; its data block, leaf, and metadata
  block are freed, and a sole-child root becomes depth zero. At greater depths,
  a one-leaf depth-one node can be removed from a non-first slot in its
  depth-two parent when that parent has another child; the
  depth-two parent's checksum is refreshed and its first key is unchanged.
  Emptying that depth-two parent or changing a first-child key that requires
  ancestor propagation remains unsupported. Full-parent splitting is limited
  to the bounded depth-one and depth-two cascades described above; full
  depth-three ancestors, deeper cascades, and root rebalancing remain
  unsupported. Writes
  into unwritten extents convert the affected block to an initialized extent.
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
  the current server. Lazy block/inode bitmap initialization is supported for
  non-BIGALLOC writable filesystems: generated bitmaps are checked against the
  descriptor's free counters, their checksums are updated, and bitmap blocks
  are flushed before the lazy-init flags are cleared. Legacy
  `gdt_csum` without `metadata_csum` remains supported for read-write mounts;
  its CRC16 checksum is regenerated whenever group descriptors are written.
- **No HTree directory updates:** the `dir_index` compat flag is a
  filesystem capability and no longer prevents a writable mount by itself.
  Namespace operations that would modify a directory carrying
  `EXT2_INDEX_FL` return `EOPNOTSUPP` before changing that directory;
  ordinary unindexed directories remain writable. Renaming an indexed
  directory within the same parent is allowed, but moving one across parents
  or replacing an indexed directory is rejected before either parent is
  modified. HTree lookup and linear enumeration remain available. The
  `large_dir` incompat feature is also accepted on writable mounts: it only
  extends the HTree depth limit and does not change ordinary directory-entry
  layout. Indexed directories, including multi-level indexes, remain
  read-only; non-indexed directories on the same volume remain writable.
- **BIGALLOC is read-only:** block bitmap interpretation and read-only
  extent mapping understand clusters, but allocation, release, and truncation
  still operate in individual blocks. Writable mounts are therefore rejected.
- **META_BG write limitations:** primary descriptor blocks are written
  to their correct metablock-group locations, and backup superblock,
  classic GDT-prefix, and META_BG descriptor blocks are reserved using the
  actual sparse-super or sparse_super2 group selection. The
  `resize_inode` + META_BG combination and alternate-superblock selection
  remain unsupported. Ordinary metadata updates do not regenerate redundant
  backup GDT copies; online filesystem resizing is not implemented. A
  `sparse_super2` filesystem with or without META_BG can be writable, subject
  to the same no-journal, clean-filesystem restrictions.
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
- **Bounded extent-tree mutation:** full-leaf splitting is supported at any
  extent-tree depth accepted by the reader when its immediate parent has room.
  A full depth-one parent can also split under a depth-two inode root with a
  free entry, and full depth-one plus depth-two nodes can split together under
  a depth-three inode root or a depth-four root's depth-three external node
  when that ancestor has room. Exclusive logical-block bounds inherited from
  every ancestor are revalidated before publication. A full depth-two or
  depth-three inode root when a new root entry is required, a full depth-three
  external ancestor, a deeper cascade, or an insertion that would change
  the selected leaf's first key without supported ancestor propagation is
  rejected with `EOPNOTSUPP` or `EFBIG` before changing the mapping. General
  index-node rebalancing and inode-root growth beyond the documented
  depth-one-to-depth-two promotion remain unsupported. Removing the first
  block of an external leaf is supported
  when its parent key can be updated without propagating a key change to an
  ancestor. A leaf containing a single one-block extent can also be detached
  and its block freed when the immediate parent has another child and the
  parent entry is not its first child at depths greater than one. When the
  only external leaf under a depth-one inode root becomes empty, collapse the
  inode root to an empty depth-zero extent header instead of rejecting it. At
  depth two, a one-child intermediate node can also be detached from the
  inode root, freeing the data, leaf, and intermediate index blocks; a root
  with no remaining child is reset to depth zero. At greater depths, a
  non-first depth-one child can be detached from a depth-two parent that
  retains at least one sibling. Emptying that depth-two parent or changing its
  first-child key remains unsupported because either operation would require
  ancestor propagation.
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

To verify full-width owner and group IDs, create a no-journal Linux ext4
image, create a test inode, and set its IDs above 65535 with `debugfs`.
Mount the image read-write on MINIX: `ls -ln` must show the full values.
Then change them with `chown`, unmount, and use `debugfs stat` to confirm
that the updated values survived. Run `e2fsck -fn` after unmount to verify
the inode checksum and the rest of the filesystem.

```sh
truncate -s 256M ext4-uidgid.img
mke2fs -F -t ext4 -O 64bit,metadata_csum,^has_journal,^orphan_file,^dir_index \
  ext4-uidgid.img
debugfs -w -R "write README.md /uidgid-test" ext4-uidgid.img
debugfs -w -R "set_inode_field /uidgid-test uid 70000" ext4-uidgid.img
debugfs -w -R "set_inode_field /uidgid-test gid 80000" ext4-uidgid.img
debugfs -R "stat /uidgid-test" ext4-uidgid.img | grep -E 'User:|Group:'
e2fsck -fn ext4-uidgid.img
```

To exercise writable lazy-bitmap initialization, create a fresh ext4 image
with uninitialized block/inode bitmaps and no journal. Confirm that
`dumpe2fs` reports `BLOCK_UNINIT` and/or `INODE_UNINIT` on nonzero groups.
Mount it read-write on MINIX, create files and directories, allocate and
truncate data across multiple block groups, then unmount cleanly. Run
`e2fsck -fn` before and after the test. Repeat with `metadata_csum` and
legacy `gdt_csum` where supported; the first write mount must materialize
each lazy bitmap, update its checksum, and clear only the corresponding group
flags. Corrupt a group free counter in a disposable copy and verify that the
writable mount is rejected before the filesystem is marked unclean.

```sh
truncate -s 1G ext4-lazy-bitmap-rw.img
mke2fs -F -t ext4 -b 4096 -O uninit_bg,metadata_csum,^has_journal \
  ext4-lazy-bitmap-rw.img
dumpe2fs ext4-lazy-bitmap-rw.img | grep -E 'BLOCK_UNINIT|INODE_UNINIT'
e2fsck -fn ext4-lazy-bitmap-rw.img
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
and an `ea_inode` volume containing large xattr values. Exercise EA-inode
references in both external xattr blocks and inode-body xattr entries where
the test image places them. Corrupt each stored inode number, value size, or
entry hash and verify that inode loading returns `EIO`. Confirm ordinary file
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

For sparse_super2 coverage, prepare disposable images and inspect their
backup-group lists with `dumpe2fs -h`. Confirm that the two listed groups (and
group zero) are the only groups whose superblock/GDT copies are reserved.
Also test an unused zero in either list slot, including a zero first entry
followed by a valid second entry. Nonzero out-of-range group numbers must fail
the mount; alternate-superblock selection is rejected.

```sh
truncate -s 1G ext4-sparse-super2.img
mke2fs -F -t ext4 -O sparse_super2,^has_journal,^orphan_file ext4-sparse-super2.img
dumpe2fs -h ext4-sparse-super2.img | grep -E 'Filesystem features|Backup block groups'
e2fsck -fn ext4-sparse-super2.img
```

With `sparse_super2` and without META_BG, mount the image read-write on MINIX.
Create, write, rename, and remove regular files and create/remove ordinary
directories. Unmount cleanly and run `e2fsck -fn`; repeat with 64-byte group
descriptors and both `metadata_csum` and legacy `gdt_csum` configurations.
A `sparse_super2` + META_BG image must remain read-only. Also verify that an
unused zero in either backup-group slot is accepted, including a zero first
entry followed by a valid second entry.

For writable META_BG coverage, create disposable volumes without a
journal, resize inode, or orphan file. Use multiple descriptor blocks so both
the classic prefix and discontiguous META_BG primary descriptor locations are
exercised. Mount read-write on MINIX, create and remove files and directories,
and write enough file data to change block counters. After clean unmount, run
`e2fsck -fn` and verify that every primary descriptor checksum and free
counter is valid. Repeat with 32-byte and 64-byte descriptors, with
`metadata_csum` and legacy `gdt_csum` where supported, and with a partial
final metablock group. Also test a `sparse_super2` + `meta_bg` image. The
`resize_inode` + `meta_bg` combination and alternate-superblock mounts must
remain rejected.

```sh
# 32-byte descriptors, all descriptor blocks in META_BG groups.
truncate -s 1G ext4-meta-bg-32bit-rw.img
mke2fs -F -t ext4 -b 4096 -g 1024 \
  -O extent,meta_bg,^64bit,^resize_inode,^has_journal,^orphan_file \
  ext4-meta-bg-32bit-rw.img

# 64-byte descriptors, with the first descriptor block in the classic prefix.
truncate -s 1G ext4-meta-bg-64bit-rw.img
MKE2FS_FIRST_META_BG=1 mke2fs -F -t ext4 -b 4096 -g 1024 \
  -O extent,64bit,meta_bg,^resize_inode,^has_journal,^orphan_file \
  ext4-meta-bg-64bit-rw.img

e2fsck -fn ext4-meta-bg-32bit-rw.img
e2fsck -fn ext4-meta-bg-64bit-rw.img
```

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
For a filesystem containing a legacy indirect-format inode, also construct
direct and single/double/triple-indirect pointers that target a block bitmap,
inode bitmap, inode-table block, an out-of-range block, and a valid data block
inside a BIGALLOC cluster containing reserved metadata. Each invalid mapping
must fail with `EIO`; zero pointers must remain ordinary holes. Verify the
same paths with valid non-reserved data blocks to ensure the stricter checks do
not reject ordinary legacy indirect files. Use an image whose group count ends
part-way through a metablock group to
confirm that the final partial group does not cause out-of-range backup
descriptor reads. Confirm that `sparse_super2` + `meta_bg` read-write mounts work with the
primary descriptor locations, while alternate-superblock META_BG mounts are
rejected. Run `e2fsck -fn` after each unmounted-image test.

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
containing several hundred extents but fewer than four full external leaves,
so its tree has a depth-one inode root. Attach that image to MINIX and write
one block into a hole in a full external leaf, choosing a physical allocation
that cannot merge with either neighboring extent. The write should split the
leaf, add a root index entry, and remain readable after unmount/remount.
Verify the resulting extent tree with `debugfs`/ `filefrag` and `e2fsck -fn`,
both with and without `metadata_csum`.

For root-growth coverage, first prepare a depth-one file whose inode-resident
extent root uses all four index entries and whose selected external leaf is
full. Write one block into a hole in that leaf, choosing a physical allocation
that cannot merge with its neighbors. The operation must split the leaf,
copy the four old root entries plus the new leaf entry into a newly allocated
depth-one external index node, and change the inode root to depth two with one
child. Verify the root key, all five child keys/pointers, both leaf checksums,
the new index-node checksum when `metadata_csum` is enabled, and `i_blocks`
(accounting for the data block, new leaf, and new index node). Repeat without
`metadata_csum`. The new external index node must have room for the fifth
entry; further inode-root promotion beyond depth two is unsupported and must
fail without changing the tree. Unmount and run
`e2fsck -fn`.

For a full depth-one external index node under a depth-two inode root,
prepare a fragmented file with a root entry that points to an external index
node whose leaf-entry array is full, while the inode root still has an unused
slot. Fill one external leaf and write a block into a hole in that leaf so
the leaf split must also split the full depth-one index node. The existing
external node must retain the lower half of the sorted child array; a new
external depth-one node must contain the upper half; the inode root must gain
the new sibling entry immediately after the original child. Verify that the
new root key equals the first key in the new index node, the old root key is
unchanged, all child pointers are in order, both index-node checksums and both
leaf checksums validate with `metadata_csum`, and `i_blocks` accounts for
the data block plus two new metadata blocks. Repeat without `metadata_csum`.
A depth-two inode root with no free entry, or an ancestor chain beyond the
bounded root-depth-three and depth-four cases below, must remain unsupported
and leave the image unchanged.
Unmount and run `e2fsck -fn`.

For depth-three and deeper root trees, prepare a full depth-one external
index node below a depth-two ancestor node that still has one free entry.
Trigger a full leaf split under that node. The depth-one node must split into
a left node retaining its original first key and a right sibling; the
depth-two ancestor must receive the sibling key/pointer immediately after
the original child, including when the original child is entry zero. Verify
the new sibling key is within the selected parent's inherited upper bound,
the ancestor first key remains unchanged, all five modified external
node/leaf checksums are regenerated with `metadata_csum`, and `i_blocks` includes the new leaf,
sibling index node, and data block. Repeat at root depths three, four, and
five when fixtures can be generated, with and without `metadata_csum`.
A full depth-two ancestor may split only in the depth-three-root and
root-depth-four cases described below. At root depth five or greater, or when
the required higher-level node has no free entry, the split must fail without
altering the tree. Unmount and run `e2fsck -fn`.

For a depth-three tree with both its selected depth-one parent and
depth-two ancestor full, but with a free inode-root slot, write into a full
leaf to force the two-level split. The depth-one parent and depth-two
ancestor must each retain their lower half; new right siblings must contain
the upper halves. The inode root must receive the new depth-two sibling
immediately after the original ancestor, with the sibling's first logical
key. Verify all six modified/new external nodes (two leaves, two depth-one
nodes, two depth-two nodes) pass checksums with `metadata_csum`, the original
first keys and inherited bounds remain intact, and `i_blocks` includes the
data block plus three metadata blocks. Repeat without `metadata_csum`.
A depth-three inode root with no free entry, a full depth-two ancestor under
a depth-four root whose selected depth-three node has no free slot, or any
further cascading split remains unsupported. Run `e2fsck -fn` after unmount.

For a depth-four inode root, prepare a tree whose selected depth-one parent and
depth-two ancestor are full while their containing depth-three external node
has a free entry. Force a full-leaf split. Verify that the depth-one and
depth-two nodes are divided into ordered left/right halves, the new depth-two
sibling is inserted immediately after the original child in the depth-three
node, and the inode-root pointer to that depth-three node is unchanged. Check
that the depth-three node, both depth-two nodes, both depth-one nodes, and both
leaves have valid checksums with `metadata_csum`; inherited upper bounds and
`i_blocks` (data block plus three metadata nodes) must remain correct. Repeat
without `metadata_csum`. A full depth-three node and any attempt to cascade a
split into the inode root or a deeper ancestor must fail without changing the
tree. Run `e2fsck -fn` after unmount.

For depth-two and deeper coverage, prepare fragmented files whose inode roots
have depths two and three (and greater, when the host tools can generate them).
Choose a selected depth-one external index node with at least one free entry
and one full external leaf below it. Write a block into a hole inside that
leaf, ensuring the allocated physical block cannot merge with neighboring
extents. The leaf should split and the new key should be inserted into the
immediate parent index node; no ancestor index entry should change. Include a
case where the selected leaf is the final child of its parent and inherits a
tighter upper bound from an ancestor, plus a case where a parent has no free
entry. The supported split must preserve all ancestor bounds; a full immediate
parent must fail with `EFBIG` and leave the image unchanged. Verify all
modified parent/leaf checksums with `metadata_csum`, inspect the full tree,
and run `e2fsck -fn`. Repeat a supported deeper-tree split without
`metadata_csum`.

On supported depth-one and deeper images, also exercise freeing the first
block of the first extent in a nonempty external leaf. The parent index key
must advance to the new first logical block and its checksum must be
regenerated when `metadata_csum` is enabled. Repeat when the first extent
has length one but later extents remain, so the next extent becomes the new
first record. At depth one, include the first inode-root index entry; at
greater depths, include a nonzero parent index slot whose key can change
without altering an ancestor key. Freeing the only extent in a leaf, or
changing the first key of a parent node's first child when an ancestor update
would be required, must fail without modifying the mapping. Also test
removing a leaf that contains exactly one one-block extent: with multiple
inode-root children at depth one, deleting the first, middle, or final child
must remove the root entry and free both the data block and leaf metadata
block. Also test a depth-one inode root with exactly one external leaf: after
deleting its only one-block extent, the inode root must become an empty
depth-zero extent header and both blocks must be freed. At depth two, test a
leaf that is the sole child of its intermediate index node; deleting it must
remove that now-empty node from the first, middle, or final inode-root slot and
free the data block, leaf, and intermediate index block. The root depth must
remain two when sibling intermediate nodes remain, and become zero when the
removed node was its only child. At greater depths, deleting a non-first child
from a parent that has another child must compact that parent, refresh its
checksum, and free both blocks. Also test depth-three and deeper trees where a
depth-one node contains only the selected leaf and is a non-first child of its
depth-two parent: deleting the leaf must remove the depth-one node, free the
data, leaf, and depth-one metadata blocks, compact the depth-two parent, and
refresh its checksum while leaving its first key unchanged. A first child of
that depth-two parent, or its only child, must remain unsupported because
removal would require ancestor propagation or empty that parent. Verify
`i_blocks`, group and free-block counters, and `e2fsck -fn` after unmount, with and without
`metadata_csum`. Also exercise writes into holes within a non-full leaf,
unwritten-extent conversion, and tail truncation. Full-parent splitting must
succeed only in the bounded cases above; unsupported cascades and index-node
rebalancing must fail without damaging the filesystem. Also corrupt the first
logical-block key in an external child extent node without changing its parent
index entry; lookup of a block in that subtree must fail with `EIO`, even when
the child checksum is recomputed. Repeat for an intermediate index node and a
leaf node, and verify that a matching parent/child key remains readable.
Construct a leaf whose final extent crosses the next parent index key; mapping
within that subtree must fail with `EIO` even if all node checksums are valid.
Also verify that final children at multiple tree depths inherit the correct
exclusive upper bound and that extents ending exactly at the boundary remain
valid. Corrupt a leaf extent so it crosses an ancestor-imposed upper bound
while keeping all checksums valid; lookup and write mapping must reject it
with `EIO`. Unmount
cleanly and run `e2fsck -fn`. For the 64 KiB image, exercise
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
for read-write testing. Use a second image with `ea_inode` enabled and
metadata checksums enabled to exercise the read-only EA-backed payload path:

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
60-byte boundary and at EOF. On the EA-inode image, create files, inline
symlinks, and inline directories whose `system.data` values are stored in EA
inodes, including values crossing filesystem-block boundaries and values
near 64 KiB. Compare file contents, symlink targets, directory lookup, and
getdents output with the Linux source image. Corrupt the EA inode checksum,
value extent, value hash, and referencing entry hash one at a time; each
referencing inode must fail with `EIO` before returning payload bytes. Also
verify that an `ea_inode` filesystem is rejected for read-write mounting.
On the ordinary inline-data image, overwrite bytes in short files and in
existing `system.data` payloads, extend a file only up to its already
allocated inline payload capacity, and shrink/extend it with truncate. Writes or truncates beyond the current
inline capacity must fail
without changing the inode. Corrupt the inode-body xattr header, entry
terminator, duplicate `system.data` entry, value offset/size, or inode size
independently and confirm the inode is rejected with `EIO`. Payloads larger
than the in-core 256-byte bound, non-inline EA-inode xattr values remain
validation-only, growth requiring a larger inline `system.data` value, and
page mapping remain unsupported.
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

For directory-entry validation, test ordinary and inline directories with
valid names at the minimum and maximum supported lengths. On disposable image
copies, corrupt one live entry at a time with an empty name, an embedded NUL,
a slash, and reserved ext4 `file_type` values (for example 8 and 255). Lookup
and `getdents` must fail with `EIO`; inode-zero unused entries and HTree
placeholders with empty names must remain skippable. Run `e2fsck -fn` on the
unmounted images after testing.

For directory-entry type coverage, populate a disposable ext4 image on Linux
with regular files, directories, symlinks, FIFOs, block/character devices,
and a bound Unix-domain socket. Mount it read-only on MINIX and inspect the
`d_type` values returned by `readdir`/`getdents`: the socket entry must report
`DT_SOCK` (not `DT_UNKNOWN`). Repeat with an image whose directory entry type
field is set to `EXT2_FT_UNKNOWN`; it must continue to report `DT_UNKNOWN`.
Run `e2fsck -fn` after testing the unmounted image.

For EA-inode-backed xattr value validation, create disposable Linux ext4
images with `ea_inode` enabled and `metadata_csum` both enabled and disabled.
Add large `user.*` attributes so e2fsprogs stores their values in EA inodes.
Verify that files referencing valid EA inodes remain readable. On unmounted
copies, corrupt the EA inode flag, mode, link count, refcount, size, extent
mapping (including a hole), stored value CRC32C, and the referring xattr
entry hash independently. For checksummed images, recompute both the parent
xattr-block checksum and EA inode checksum after each targeted edit, so the
test reaches the corresponding EA validation. Each malformed reference must
cause the referencing inode to fail with `EIO`; an EA inode number without
the `ea_inode` incompat feature must also be rejected. Keep the EA inode out
of the pathname namespace and run `e2fsck -fn` after tests.

For external xattr-block validation, create a disposable ext4 image without
`metadata_csum`, populate files with `user.*` attributes large enough to force
external xattr blocks, and verify that valid files remain readable. On an
unmounted copy, corrupt one field at a time: the xattr header magic, refcount,
block count, entry terminator, name bytes (including an embedded NUL), value
size, value offset, and an EA-inode number. Reattach the image read-only and
verify that an inode referencing each malformed block fails with `EIO`.
Repeat on a `metadata_csum` image and recompute the block checksum after each
byte-level edit, so rejection is attributable to malformed xattr structure
rather than a stale checksum. Also test a valid `ea_inode` image and a block with an
EA-inode reference but without the incompat feature. Run `e2fsck -fn` on the
unmounted images after testing.

For indexed-directory lookup, create disposable Linux ext4 images with
`dir_index` enabled and separate legacy, half-MD4, and TEA hash defaults.
Populate directories until their HTree spans multiple leaf blocks; also create
a `large_dir` image with multiple internal-index levels. Verify lookups for
names at the beginning and end of each hash range, long names, and names with
high-bit bytes. Include duplicate-hash names that may be split across
continuation leaves; a candidate-leaf miss must still be found by the linear
fallback. Repeat on `metadata_csum` images and corrupt the dx root and internal
node count/limit fields, hash ordering, logical child block numbers, and dx
checksums one at a time; malformed index metadata must report `EIO`. Compare
lookup results with a complete directory listing and run `e2fsck -fn` on
unmounted images after testing. SipHash/hash-in-dirent mode remains on the
linear path.

For writable compatibility with the `dir_index` capability flag, create a
disposable image without a journal or `large_dir`. Populate one directory
until `e2fsck -D` creates an HTree, and keep a second directory unindexed.
Mount the image read-write on MINIX. Creating, renaming, and removing entries
in the ordinary directory must work; HTree lookup and readdir must still work
for the indexed directory, while create/link/unlink/rename within that
directory, moving an indexed directory across parents, replacing an indexed
directory during rename, and removing the indexed directory itself must fail
with `EOPNOTSUPP` without changing either directory. A same-parent rename of
an indexed directory may succeed because it does not change that directory's
contents or `..` entry. After unmount, run
`e2fsck -fn` and verify that the image is clean.

For read-only `large_dir` coverage, create an ext4 image with
`large_dir` and `dir_index` enabled (and `orphan_file` disabled), populate
a directory on Linux until `debugfs -R 'htree_dump /path' image` reports a
three-level HTree, then run `e2fsck -fn`. Attach the image to MINIX and verify
both pathname lookup and `getdents`/directory listing across the full
directory. Include enough entries to force enumeration across multiple leaf
and internal-index blocks. Mount read-only; modifying indexed directories is
still intentionally rejected. Run `e2fsck -fn` again after testing.

For writable `large_dir` compatibility, create a disposable ext4
image with `large_dir` and `dir_index` enabled and journaling/orphan-file
support disabled. On Linux, create a large indexed directory and a separate
small, non-indexed directory. Populate the indexed directory until
`debugfs -R 'htree_dump /indexed' image` reports a three-level HTree, run
`e2fsck -fn`, and attach the image read-write on MINIX. Confirm that ordinary
files can be created, written, renamed, and removed, and that entries can be
created and removed in the non-indexed directory. HTree lookup and
`getdents` must still work in the indexed directory, but create/link/unlink/
rename operations that modify that directory (including `rmdir` of an
indexed directory) must fail with `EOPNOTSUPP` without changing either the
directory or its parent. Unmount cleanly and run `e2fsck -fn` on the image.

The large-file read path should be tested with files larger than 2 TiB on
4 KiB-block volumes and with sizes near the 32-bit logical-block boundary on
larger-block volumes. Verify reported `st_blocks` in 512-byte units. As with
the rest of this experimental server, run `e2fsck -fn` after clean unmount.

The META_BG read-write layout, extended timestamp, 64 KiB directory-record,
`huge_file`, `metadata_csum`, bounded external-extent-leaf update, writable
legacy `gdt_csum`, writable `large_dir`, and writable `sparse_super2`
mount-compatibility changes have not yet been compiled or runtime-tested in a
MINIX build environment. Treat this as a review candidate and test it on
disposable images only.
