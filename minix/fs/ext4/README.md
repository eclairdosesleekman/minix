# MINIX ext4 file server (read-only)

This is an experimental, read-only ext4 file server kept separate from the
existing ext2 server.

## Implemented

- Extent trees stored in the inode and in external extent nodes (depth up to 5).
- Written extents and unwritten extents (unwritten ranges read as zeroes).
- 64-bit physical block numbers, 64-byte group descriptors, and high block
  counters from the ext4 superblock.
- 64-bit regular-file sizes using the inode size-high field.
- Directory record lengths, names, and inode numbers are bounds-checked before use; 64 KiB blocks are rejected because their extended `rec_len` encoding is not implemented.
- Linear directory traversal of htree leaf blocks; the dx root metadata after
  `..` is skipped in the first directory block.
- VFS writes, metadata mutations, and raw block writes are rejected with
  `EROFS`. Mounts must explicitly be read-only.

## Limitations

This is not a complete ext4 implementation. It does not replay the journal,
recover orphan inodes, update metadata, or validate ext4 metadata checksums.
It rejects unsupported incompatible and read-only-compatible feature flags,
including journal recovery, meta_bg, MMP, inline data, encryption, casefold,
bigalloc, huge_file, gdt_csum, and metadata_csum. It also requires the clean filesystem state and rejects a non-empty
legacy orphan list and the orphan-file feature. Use a clean, unmounted
filesystem and run `e2fsck` before mounting.

Treat the server as experimental and keep a backup of the volume. Do not
attempt a read-write mount.

## Build and test

The normal `minix/fs` build includes this server. Start the installed server
and mount a volume read-only (device name and service path may vary):

```sh
service up /service/ext4
mount -r -t ext4 /dev/c0d0p1 /mnt
```

A host-side test image can be prepared with e2fsprogs:

```sh
truncate -s 512M ext4-test.img
mke2fs -F -t ext4 -b 4096 -O extent,64bit,^metadata_csum,^gdt_csum,^huge_file,^orphan_file ext4-test.img
e2fsck -fn ext4-test.img
```

Populate and check the image on the host before attaching it to MINIX. Test
regular and sparse files, unwritten extents, large files, directories,
symlinks, and reads past extent boundaries. The initial patch has not yet
been built or runtime-tested.
