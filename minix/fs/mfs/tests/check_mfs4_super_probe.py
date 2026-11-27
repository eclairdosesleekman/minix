#!/usr/bin/env python3
"""Static regression checks for the MFS4-vs-MFS3 mount probe."""

from pathlib import Path
import re

source = Path(__file__).resolve().parents[1].joinpath("mfs4_runtime.c").read_text()
match = re.search(
    r"int\\nmfs4_probe_super\\(.*?\\n}\\n(?=\\nint\\nmfs4_read_super\\()",
    source,
    re.S,
)
assert match, "mfs4_probe_super() was not found"
probe = match.group(0)

primary_read = probe.index("lmfs_get_block(&bp, sp->s_dev, 0, NORMAL)")
signature_check = probe.index("primary_magic = memcmp")
geometry_query = probe.index("bdev_ioctl(sp->s_dev, DIOCGETP")
assert primary_read < signature_check < geometry_query, (
    "the primary MFS4 signature must be read before DIOCGETP"
)

# A known MFS4 signature must never be treated as permission to use the MFS3
# decoder when partition geometry or the backup-superblock read fails.
assert "return primary_magic ? r : OK;" in probe
assert "return primary_magic ? EINVAL : OK;" in probe
assert "*is_mfs4 = 1;" in probe
assert "mfs4_decode_super(primary, partition_blocks, &pg)" in probe

# Keep the legacy fallback only for media without an MFS4 signature.
assert "if (!primary_magic && !backup_magic)" in probe
assert "(void)lmfs_set_blocksize_err(old_block_size);" in probe

print("MFS4 superblock probe regression checks passed")
