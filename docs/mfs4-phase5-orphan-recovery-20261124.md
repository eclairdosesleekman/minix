# MFS4 Phase 5 orphan recovery implementation

Date: 2026-11-24

Design basis: `MFS4_detailed_design_ja_v1.0.md`, sections 12.1, 12.3,
13.1–13.4, 10.8, and 24 (Phase 5 acceptance).

## Implemented

- Defined `I4_ORPHAN` and `I4_TRUNCATING` and added codec invariants:
  only these flags are accepted, truncating implies orphan, non-orphan inodes
  cannot carry `orphan_next`, and an allocated zero-link inode must be
  orphaned.
- Added state-block validation and head-update helpers. They verify magic,
  version, immutable volume generation, CRC32C, reserved zero bytes, allowed
  flags, and the inode-number range. Head updates leave volume generation
  unchanged and recalculate CRC32C.
- MFS4 truncate shrink commits the new size plus orphan flags first. It then
  reclaims only mappings at/after `ceil(new_size / 4096)`. All MFS4 shrink
  operations currently take this orphan path, including small truncates, to
  avoid trying to predict the exact number of journal images in advance.
  Linked inodes keep their xattr chain and have only the truncate flags cleared.
- MFS4 unlink/rmdir/rename-overwrite enqueue the inode in the same transaction
  that deletes the directory entry and decrements nlink to zero.
- The orphan worker searches the mapping tree from high logical blocks to low
  and frees at most 32 logical blocks per transaction. The cursor is bounded
  to the range below the previously committed chunk, avoiding a repeated scan
  of already reclaimed high blocks. Empty indirect blocks are pruned by the
  existing MFS4 mapper.
- The orphan-list head remains on the inode during chunk processing. The final
  transaction removes the list head, clears flags, and, for nlink-zero inodes,
  clears inode contents and frees the inode bitmap bit. A crash before that
  transaction leaves enough state to resume.
- MFS4 `put_inode()` no longer runs the legacy immediate-free path for a
  zero-link inode. It keeps the orphan cached until the final close or mount
  recovery worker reclaims it. MFS3 close-time deletion remains unchanged.
- Writable mount replays the journal, then recovers the orphan list before
  marking the volume dirty for normal operation. Read-only mount refuses dirty
  or orphaned volumes. Unmount refuses to mark the filesystem clean while the
  orphan list is non-empty.

## Tests

Added `mfs4_orphan_test` to `make -C minix/tests/mfs4 clean test` and a
dedicated GitHub Actions workflow. The host tests cover state-block validation,
head update/CRC recomputation, reserved-field and generation corruption, bad
head ranges, root-inode exclusion, and reclaim eligibility for open versus
closed orphans.

A local C99 host harness compiled the orphan-core helper and ran its matching
unit cases successfully. The repository's full MINIX build, the MFS server
integration path, QEMU power-loss testing, and the full crash-injection matrix
were not executed in this environment.

## Remaining limits

- MFS4 v1 xattr-chain encoding/cleanup is not yet implemented. If a zero-link
  orphan has a nonzero `xattr_head`, the worker returns `EOPNOTSUPP` and
  preserves the inode/orphan record rather than freeing an inode with
  unhandled xattrs. Linked-inode truncate does not discard or rewrite the
  xattr chain. Completing zero-link cleanup requires the xattr phase's chain
  format and validation.
- Runtime VFS/LMFS tests and crash injection at every Phase 4 journal barrier
  remain necessary before claiming Phase 5 acceptance.
- The orphan host tests validate the portable state helpers and reclaim policy;
  they are not end-to-end tests of the inode-cache, LMFS, journal, or device
  integration.

