# MFS4 Phase 3 block-map tests

Run the portable host suite from this directory:

```sh
./run-mfs4-blockmap-test.sh
```

The suite compiles the same MFS4 block-map translation unit that is linked into
the MFS server. Its block-I/O callbacks use an in-memory image; no device,
root privileges, or MINIX boot is required.

Coverage includes bitmap allocation (including a multi-block bitmap), reserved
root-directory block protection, bad-pointer rejection, direct/single/double/
triple-indirect boundaries, the 1 TiB maximum-file-size boundary, sparse reads
and writes above 4 GiB, subtree pruning, and zeroing the retained partial block
during truncate/extension.

The mapper is a Phase 3 core with a block-I/O callback API. Runtime MFS4 mount
and inode codec dispatch are not wired by this change. Metadata callbacks still
write directly; Phase 4 must back them with transaction-shadow/journal
operations before crash-atomic metadata updates can be claimed. Orphan-based
chunked truncate remains Phase 5 work.
