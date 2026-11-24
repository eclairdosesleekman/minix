# MFS4 Phase 1–4 source review

- Review date: 2026-11-24
- Repository: eclairdosesleekman/minix
- Reviewed base: 631b7de23164d5522ea4f9268c375d6ac5cc511b (master)
- Specification: MFS4_detailed_design_ja_v1.0.md, especially §§7, 9–13,
  17–18, 21, 23–24.

## Summary

The Phase 1–4 implementation is present in master, including the common inode
and MFS3 codec split, MFS4 formatter/runtime metadata, triple-indirect mapping,
and the redo journal integrated with VFS mutation callbacks. The review found
one concrete width bug in link-count admission and fixes it in this branch.

This is not a Phase 1–4 acceptance sign-off. The design's acceptance gates
include MFS3 golden-image regression, integrated 64-bit I/O/truncate checks,
and the full crash-injection matrix. Those are not demonstrated by the current
host journal tests or the available recorded CI runs.

## Fixed in this PR: MFS4 link count was capped at the legacy LINK_MAX

The design's §7 and §14.3 define the persistent MFS4 hard-link count as
unsigned 32-bit and require EMLINK only when an increment would exceed
UINT32_MAX. The common in-memory inode and MFS4 disk codec already preserve
that width, but three MFS callback checks still used the legacy LINK_MAX:

- link.c: fs_link() — normal hard-link creation.
- open.c: new_node() — creation of a subdirectory (parent link count).
- link.c: fs_rename() — moving a directory to a different parent.

That made the MFS4 32-bit field unreachable above the legacy limit. This PR
adds the shared MFS4_MAX_NLINK format constant and applies it only when the
mounted format is V4. MFS3 retains its former LINK_MAX behavior.

## Phase-by-phase review

### Phase 1 — common inode and MFS3 codec separation

The source has a format-independent in-memory inode and a separate 64-byte
MFS3 codec (inode_v3.c). MFS3 UID/GID/nlink/size/time narrowing is checked
before encoding, and MFS4 uses its own 128-byte byte-oriented codec. The link
limit defect above was a remaining MFS4 width mismatch.

Not verified: the design's §23.1 MFS3 golden-image and filesystem-operation
regressions have not been run in this environment. Thus MFS3 compatibility
cannot be signed off from source inspection alone.

### Phase 2 — MFS4 on-disk format and mkfs

The MFS4 disk constants/codecs are shared through
minix/include/minix/mfs4_disk.h. The mkfs.mfsv3 build includes
v3/mfs4.c, which implements -V 4, 4 KiB geometry, primary/backup
superblocks, bitmaps, inode table, state block, journal reservation, and root
directory initialization. MFS3 remains the default formatter.

Not verified: there is no recorded formatter geometry/image regression in
the current MFS4 test workflows. The fsck MFS4 dispatcher is a Phase 8 item in
the design, so its absence is not counted as a Phase 2 defect.

### Phase 3 — allocator, triple-indirect mapping, 64-bit I/O/truncate

The active MFS service code routes MFS4 reads through
mfs4_read_map_checked() and writes/frees through mfs4_write_map() /
mfs4_free_map_range() in mfs4_map.c. The VFS wrapper in table.c
limits ordinary writes to bounded transactions.

A test-coverage gap remains: minix/fs/mfs/tests/run-mfs4-blockmap-test.sh
builds and tests the separate callback-based mfs4_blockmap.c component.
That component is linked into the service, but the active read.c/write.c
inode path calls mfs4_map.c; the standalone block-map test does not execute
that integrated code path.

Not verified: no 64-bit end-to-end read/write/lseek/truncate test, 1 TiB
runtime boundary test, amd64 MINIX build, or QEMU run was performed here.

### Phase 4 — fixed redo journal, shadow images, barriers, recovery

The active mutation table wraps create/mkdir/mknod/link/unlink/rmdir/rename,
metadata changes, putnode, truncate and write callbacks in MFS4 transactions.
The journal adapter flushes LMFS and then issues DIOCFLUSH; the portable
journal core validates descriptors/images before replay and fails closed on
post-commit errors.

The journal host tests cover successful commit, ignoring uncommitted images,
replay after a home-write failure, replay after home flush/before clear,
corrupt-image rejection, de-duplication, and the 64-image limit. The recorded
MFS4 redo journal workflow runs on the recent MFS4 changes are successful.

Not verified: §23.3's full crash-injection matrix at every data/header/
image/commit/home/clear persistence point; actual AHCI and virtio flush
behavior; flush-disabled devices; device timeouts/removal; and QEMU power-loss
recovery. A passing memory-disk test does not prove real-device durability or
the whole VFS path.

## Known boundary / follow-up

The design's Phase 5 table explicitly assigns orphan-list recovery and
chunked unlink/truncate cleanup to Phase 5. The current mount code rejects a
non-empty orphan list rather than silently mounting it. However, current
mfs4_trunc_request() wraps fs_trunc() as one transaction, while
mfs4_free_map_range() can touch more than 64 unique metadata images.
Such a truncate fails with E2BIG and aborts instead of using the bounded
chunk/orphan policy described in §12.1. The existing Phase 4 integration PR
also records this as outstanding. This PR does not attempt to implement the
separate Phase 5 recovery machinery.

## Validation performed for this review

- Source inspection of the Phase 1–4 MFS, formatter, block-map, journal, mount,
  and VFS callback paths against the uploaded v1.0 design.
- Confirmed the repository has successful recorded MFS4 journal workflow runs.
- Checked local tool availability: a host C compiler is present, but
  bmake, nbmake, qemu-system-x86_64, and mkfs.mfs are unavailable.
- The changed MFS sources and the MINIX build were therefore not compiled in
  this environment; no MFS3, runtime MFS4, or QEMU regression is claimed.

## Conclusion

One source-level MFS4 32-bit link-count defect is fixed by this PR. The source
implementation for Phases 1–4 is substantial, but the design's test-based
acceptance conditions remain unverified, and large truncate/orphan handling
remains a known follow-up. Do not treat this review as proof that MFS4 1.0 is
release-ready.
