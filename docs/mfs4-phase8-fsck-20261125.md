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

## Boot-path integration decision

The x86 boot code was checked rather than assuming that MFS server support
also provides bootloader support:

- `sys/arch/amd64/stand/Makefile` builds the UEFI x64 loader from
  `sys/stand/efiboot/bootx64`.
- `sys/stand/efiboot/Makefile.efiboot` defines
  `SUPPORT_MINIXFS3`, and `sys/stand/efiboot/conf.c` registers
  `minixfs3`; there is no MFS4 standalone reader.
- The legacy i386 BIOS standalone loader also defines
  `SUPPORT_MINIXFS3` in `sys/arch/i386/stand/boot/Makefile.boot`.

Therefore this change deliberately does not advertise an MFS4 volume as a
bootloader-readable filesystem. The supported Phase 8 deployment profile is
to keep the kernel, boot configuration, and any boot-time modules on an
MFS3-readable boot partition or on a UEFI FAT/ESP path supported by the EFI
loader, while mounting MFS4 as the system root only after the kernel and MFS
server have started. This follows the safe migration option in design §26.5.
No bootloader source change is made without a separate reader implementation
and QEMU validation.

## Performance measurement harness

`minix/tests/mfs4/mfs4_fs_bench.c` provides a repeatable target-side workload
for comparing an MFS3 mount and an MFS4 mount. Build it on the target with:

    make -C minix/tests/mfs4 mfs4_fs_bench

Run the same binary against disposable test mountpoints, on the same storage
and with the same cache/VM settings:

    ./minix/tests/mfs4/mfs4_fs_bench /mnt/mfs3 MFS3 > mfs3.csv
    ./minix/tests/mfs4/mfs4_fs_bench /mnt/mfs4 MFS4 > mfs4.csv

Optional arguments select the iteration count and sequential-write size in
MiB. The CSV reports create/unlink, rename, 4 KiB writes, 1 MiB sequential
writes (including final fsync), and 4 KiB write+fsync latency. It creates and
removes a private per-process directory below the supplied mountpoint; use
disposable benchmark filesystems, not production data.

The harness does not simulate a power cut and does not report recovery time.
That metric must be measured by the QEMU crash-injection runner, from VM restart
until the filesystem becomes available, for each journal crash point. No
performance numbers are claimed by this source-only change.

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
