# MFS4 Phase 8 — fsck foundation and release-gate status

This change adds an MFS4-specific checker path without changing the legacy
MFS2/MFS3 scanner. The common fsck_mfs executable detects the MFS4 magic
before entering the old checker.

## Implemented

- Primary and backup superblock magic, version, CRC32C, fixed sizes, feature
  fields, and converged region geometry are validated. If one copy is valid,
  it is used and the damaged peer is reported; neither copy is rewritten.
- The fixed 66-block redo journal is inspected. A valid commit record requires
  matching sequence/count/header CRC, valid descriptor ranges and unique home
  blocks, per-image CRCs, and the aggregate image CRC.
- Default operation and -n are read-only. A pending committed transaction
  causes a diagnostic and no replay. -a, -y, and -p replay only a fully
  validated committed journal transaction; replay happens before metadata
  scanning and is idempotent.
- The checker validates imap/zmap reserved and tail bits, inode allocation
  consistency, 64-bit size bounds, nanosecond ranges, inode flags and
  generation, direct/single/double/triple-indirect pointer ranges and duplicate
  allocation, directory references and . / .., link counts, orphan state,
  XAT4 owner/generation/CRC/TLV chains, and ACL payload/mode consistency.
- The scanner uses the server's journal, orphan, xattr, and ACL codec helpers.
  It never uses native C structure serialization for disk records.
- Host tests build a small MFS4 image in memory and check a clean image,
  directory corruption, state-block CRC corruption, and the no-write invariant.
- The MFS4 mkfs state-block CRC range is corrected to bytes 0..4091, matching
  the state codec and the on-disk state-block contract.

## Safety and current limits

The Phase 8 checker intentionally does not yet repair structural metadata.
Only a committed journal transaction may be replayed. Bitmap reconstruction,
inode/link-count correction, directory repair, xattr repair, and clean-state
rewriting are not attempted. -n opens the device read-only and the verifier
has no write callback path unless replay was explicitly requested.

A pending commit with an invalid header or image is never discarded. Unknown
incompat features are rejected. A single valid superblock copy may be used for
checking, but the damaged copy is left untouched.

## Build and test

Run:

    make -C minix/tests/mfs4 clean test

The GitHub Actions host-test workflow now runs when the MFS4 checker, codec,
or mkfs implementation changes.

## Release gate still required

This source change does not claim Phase 8's full release acceptance. The
following work still requires a bootable MINIX image and target hardware or
QEMU:

1. Cross-build the amd64 system and run the fsck command against images
   produced by mkfs.mfs -V 4.
2. Exercise journal replay and corruption cases on a disposable QEMU disk,
   including crash injection around commit and replay barriers.
3. Verify the actual boot path. MFS server support does not imply that the
   bootloader can read MFS4. Until a bootloader MFS4 reader is validated,
   retain an MFS3 or otherwise bootloader-supported /boot volume.
4. Record MFS3/MFS4 measurements for create/unlink/rename, small writes,
   sequential writes, fsync, and recovery time. No performance result is
   asserted by this change.
5. Run the same smoke tests on supported physical storage drivers and confirm
   DIOCFLUSH behavior; this host-only checker test cannot establish device
   durability.

The pull request should remain draft until these release gates pass.
