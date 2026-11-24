# MFS4 Phase 6 — XAT4 xattr format and API

Date: 2026-11-24

Design basis: `MFS4_detailed_design_ja_v1.0.md`, §15.1–15.4,
§16.3, §23.2–23.3, and Phase 6 in §24.

## Implemented

- Added the 4096-byte XAT4 block constants and offsets to the shared
  byte-oriented `mfs4_disk.h` format header.
- Added a portable TLV/block codec. It validates the 32-byte header, whole
  block CRC32C with the CRC field zeroed, owner inode and inode generation,
  next-block bounds at chain traversal, zero flags/reserved bytes, used-byte
  range, 8-byte alignment padding, name/value lengths, supported namespaces,
  duplicate names, and complete entry boundaries.
- Added MFS4 chain reading with a 16-block maximum and cycle detection. Every
  block is bound to the owning inode number and generation.
- Added get, set, list, and remove MFS4 operations. Updates build new chain
  block images, update zmap, switch inode `xattr_head`, and free the previous
  chain through one redo-journal transaction. A single transaction is
  sufficient because the bounded worst case is 16 new XAT4 images, up to 16
  new zmap images, up to 16 old-chain zmap images, and one inode image: at most
  49 unique journal images, within the 64-image journal limit. This ensures
  the new chain is not visible until the atomic metadata transaction commits.
- Added the four FS protocol request numbers, four `struct fsdriver`
  callbacks, libfsdriver dispatch, VFS request wrappers, VFS handlers, and
  libc implementations of all 12 APIs declared in `<sys/xattr.h>`:
  path, no-follow path, and descriptor variants of get/set/list/remove.
- VFS resolves path or descriptor targets, handles no-follow final symlinks,
  checks ordinary read/write access and owner/superuser restrictions, and
  passes variable-length names and values through grants rather than inline
  IPC payloads.
- Extended Phase 5 orphan finalization to free a zero-link inode's validated
  xattr chain in the final orphan transaction. MFS3 returns
  `EOPNOTSUPP` for these new xattr operations.

## Namespace and Phase boundary

The public Phase 6 API exposes only `user.*`. The MFS storage codec accepts
`system.posix_acl_access` and `system.posix_acl_default` for the Phase 7 ACL
implementation, but the Phase 6 public VFS API does not expose these names.
`trusted.*` and `security.*` remain unsupported as specified. ACL access
enforcement, default ACL inheritance, chmod/chown ACL consistency, and
userland ACL tools are Phase 7 work and are not claimed here.

## Tests and validation

Added `mfs4_xattr_test` to `make -C minix/tests/mfs4 clean test` and the
existing MFS4 host-test workflow. On commit
`675b4cc97dd6d338833d982c6da60f920c34134f`, both the `MFS4 redo journal`
and `MFS4 orphan recovery` GitHub Actions workflows passed. Each workflow
built and ran `mfs4_journal_test`, `mfs4_orphan_test`, and
`mfs4_xattr_test`; all three reported `all tests passed`. The host cases
cover:
- user and reserved namespace acceptance/rejection;
- TLV append/find/remove, duplicate prevention, and empty values;
- the 255-byte maximum name and 3801-byte maximum value fitting exactly in a
  4064-byte XAT4 payload;
- CRC, owner inode, generation, reserved-field, and malformed-TLV failures.

This test exercises the portable XAT4 codec only. The full MINIX/VFS/MFS build,
mounted MFS4 xattr end-to-end tests, real LMFS/journal allocation and replay,
xattr chain-switch crash injection, and QEMU power-loss tests have not yet been
run in this environment. Therefore this PR is a Phase 6 implementation
increment with passing host-format tests expected from CI, not a claim that the
Phase 6 end-to-end acceptance condition has already been demonstrated.

