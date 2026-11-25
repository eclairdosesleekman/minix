# MFS4 Phase 7 — POSIX ACL

This change implements the Phase 7 ACL layer on top of the Phase 6 MFS4 xattr
format. The implementation follows the MFS4 v1.1 design: MFS stores and
validates ACL values, while VFS owns the final permission decision.

## On-disk value and validation

system.posix_acl_access and system.posix_acl_default are stored in the existing
XAT4 TLV chain. The value format is version 2, a little-endian 32-bit entry
count, then 8-byte little-endian (tag, perm, id) entries. Permissions are
restricted to rwx. USER_OBJ, GROUP_OBJ and OTHER are required; a MASK is
required whenever named USER or GROUP entries are present. Duplicate base
entries and duplicate named IDs, invalid tags, reserved IDs and malformed
lengths are rejected. The maximum value is 3,801 bytes, matching the MFS4
single-xattr value bound.

Default ACLs are accepted only on directories. The MFS4 xattr update path
updates the access mode bits from USER_OBJ, MASK (or GROUP_OBJ), and OTHER in
the same transaction as the access ACL. chmod changes USER_OBJ, MASK (or
GROUP_OBJ when no MASK exists), and OTHER while preserving the named entries.

## Access checks and inheritance

VFS reads system.posix_acl_access through the existing FS xattr request
(grant-based payload) and evaluates it in this order:

1. Effective owner UID selects USER_OBJ.
2. A matching named USER selects that entry AND MASK, and terminates lookup.
3. Otherwise, all matching GROUP_OBJ/named GROUP permissions are unioned and
   ANDed with MASK.
4. If no group class matches, OTHER applies.

Group membership uses the caller's primary and supplementary groups. If no
access ACL exists, the existing mode-bit path is unchanged. A malformed stored
ACL fails closed with EIO.

When a parent directory has a default ACL, create/mkdir copies it to the new
inode's access ACL and intersects the owner/group-class/other permissions with
the requested creation mode. A newly created directory also receives the
parent's default ACL. The access ACL, optional default ACL and xattr chain
updates execute inside the existing create/mkdir metadata transaction.

## User interface

The libc API in sys/acl.h includes ACL allocation/editing, validation, text
conversion, path/fd access and update operations. When an access ACL is absent
(or the mounted filesystem does not support ACL xattrs), acl_get_file returns
the equivalent three-entry ACL synthesized from mode bits. Default ACL lookup
continues to report ENODATA when none exists.

getfacl [-d] file ... prints access ACLs and any default ACLs; -d selects
default ACL output. setfacl [-d] -s acl_text file ... replaces the selected
ACL, and setfacl [-d] -b file ... removes it. acl_from_text accepts the
complete newline-separated entry form emitted by acl_to_text; named user and
group qualifiers may be numeric IDs or account names.

MFS3 remains mode-bit-only: it continues to return the existing unsupported
xattr response for ACL attributes, and access checks fall back to legacy mode
bits. No MFS3 disk layout is changed.

## Validation and remaining integration work

minix/tests/mfs4/mfs4_acl_test.c exercises wire validation, ACL selection,
group union/masking, mode synchronization and create-mode restriction. It is
registered in the existing make -C minix/tests/mfs4 clean test host suite.

The repository's existing MFS4 GitHub Actions workflow is triggered by changes
under minix/tests/mfs4. This environment did not execute a MINIX cross-build,
QEMU boot, or live VFS/MFS permission integration test. Those remain required
before the Phase 7 access-semantics acceptance gate can be considered complete.
The text parser intentionally handles complete ACLs (replace semantics), not
incremental setfacl -m/-x editing.
