# MINIX ext4 file server

This is an experimental ext4 file server kept separate from the existing
ext2 server.

## Implemented

- Extent trees stored in the inode and in external extent nodes (depth up to 5).
- Written extents and unwritten extents (unwritten ranges read as zeroes).
- 64-bit physical block numbers, 64-byte group descriptors, and high block
  counters from the ext4 superblock.
- 64-bit regular-file sizes using the inode size-high field.
- Directory record lengths, names, and inode numbers are bounds-checked before use.
- Linear directory traversal of htree leaf blocks; the dx root metadata after
  `..` is skipped in the first directory block.
- 64 KiB blocks are rejected because their extended `rec_len` encoding is
  not implemented.
- Read-only mounts support clean ext4 volumes with the feature set documented
  by the mount-time checks.
- An opt-in writable mount mode supports in-place overwrites of existing data
  in allocated, written extents on clean, unjournaled volumes.

## Limitations of writable mode

Writable mode is deliberately data-only. It does not modify ext4 metadata and
is not a general-purpose read-write ext4 implementation.

- The volume must be clean and must not have the ext4 journal feature enabled.
- Writes are accepted only for regular files and only when the entire write
  range is already below EOF and maps to allocated, written extents.
- Writes to holes or unwritten extents, file growth, allocation, truncation,
  file/directory creation, unlink, rename, link, chmod, chown, and utimes are
  rejected with `EROFS`.
- Inode timestamps, extent trees, block/inode bitmaps, group descriptors, and
  the superblock remain untouched.
- Dirty data buffers are flushed by `sync` and at unmount. This mode does not
  provide journaling or power-failure atomicity for file contents.

This implementation still does not replay a journal, update extent trees,
recover orphan inodes, or validate ext4 metadata checksums. Unsupported
incompatible and read-only-compatible features are rejected at mount time;
these include journal recovery, meta_bg, MMP, inline data, encryption,
casefold, bigalloc, huge_file, gdt_csum, and metadata_csum. A non-empty
legacy orphan list and the orphan-file feature are also rejected. Run
`e2fsck` and use a clean, unmounted filesystem before attaching it to MINIX.

For normal read-write filesystem use, journal replay and transactions,
extent-tree updates, allocation, metadata checksums, orphan handling, and
superblock/group-descriptor updates must be implemented first. Do not treat
this limited mode as a substitute for full ext4 read-write support. Keep a
backup and run `e2fsck` before and after testing.

## Build and test

The normal `minix/fs` build includes this server. Start the installed server
and mount read-only:

```sh
service up /service/ext4
mount -r -t ext4 /dev/c0d0p1 /mnt
```

To exercise the limited writable mode, prepare a clean unjournaled image on a
host with e2fsprogs, populate it with a file before attaching it to MINIX, and
run `e2fsck` after unmounting it:

```sh
truncate -s 512M ext4-test.img
mke2fs -F -t ext4 -b 4096 \
  -O extent,64bit,^has_journal,^metadata_csum,^gdt_csum,^huge_file,^orphan_file \
  ext4-test.img
mkdir -p ext4-seed
dd if=/dev/zero of=ext4-seed/overwrite.bin bs=4096 count=4
debugfs -w -R 'write ext4-seed/overwrite.bin /overwrite.bin' ext4-test.img
e2fsck -fn ext4-test.img
```

After mounting without `-r`, overwrite bytes inside the existing file without
truncating it:

```sh
dd if=/dev/zero of=/mnt/overwrite.bin bs=512 count=1 conv=notrunc
sync
```

Do not use a journaled or mounted-on-the-host image. Test on disposable images
and verify the result with `e2fsck -fn` before relying on the data.
