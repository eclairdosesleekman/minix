/*
 * VFS extended-attribute system calls.
 *
 * Public user APIs in this phase expose the user.* namespace. The
 * system.posix_acl_* namespace is reserved for the Phase 7 ACL implementation.
 */
#include "fs.h"
#include <sys/xattr.h>
#include <minix/callnr.h>
#include <minix/vfsif.h>

#include "file.h"
#include "path.h"
#include "vnode.h"
#include "vmnt.h"

static int
vfs_xattr_name_valid(const char *name)
{
	size_t len;

	if (name == NULL)
		return EINVAL;
	len = strlen(name);
	if (len < 6 || len > XATTR_NAME_MAX)
		return ERANGE;
	if (strncmp(name, "user.", 5) != 0)
		return EOPNOTSUPP;
	return OK;
}

int
do_xattr(void)
{
	struct vnode *vp = NULL;
	struct vmnt *vmp = NULL;
	struct filp *flp = NULL;
	struct lookup resolve;
	char fullpath[PATH_MAX];
	char name[XATTR_NAME_MAX + 1];
	vir_bytes path_addr, name_addr, value_addr;
	size_t path_len, name_len, size;
	ssize_t result_size = 0;
	int fd, flags, nofollow, write_access, is_list, from_fd;
	int r, lookup_flags;

	fd = (int)job_m_in.m9_l1;
	flags = (int)job_m_in.m9_l2;
	path_addr = (vir_bytes)job_m_in.m9_ull1;
	path_len = (size_t)job_m_in.m9_l5;
	name_addr = (vir_bytes)job_m_in.m9_ull2;
	name_len = (size_t)job_m_in.m9_s1;
	value_addr = (vir_bytes)job_m_in.m9_l3;
	if (job_m_in.m9_l4 < 0)
		return EINVAL;
	size = (size_t)job_m_in.m9_l4;
	if (size > XATTR_SIZE_MAX)
		return ERANGE;

	write_access = (job_call_nr == VFS_SETXATTR ||
	    job_call_nr == VFS_REMOVEXATTR);
	is_list = (job_call_nr == VFS_LISTXATTR);
	if (job_call_nr != VFS_GETXATTR && job_call_nr != VFS_SETXATTR &&
	    job_call_nr != VFS_LISTXATTR &&
	    job_call_nr != VFS_REMOVEXATTR)
		return ENOSYS;

	nofollow = (flags & VFS_XATTR_NOFOLLOW) != 0;
	flags &= ~VFS_XATTR_NOFOLLOW;
	if (job_call_nr == VFS_SETXATTR) {
		if ((flags & ~(XATTR_CREATE | XATTR_REPLACE)) != 0 ||
		    (flags & XATTR_CREATE && flags & XATTR_REPLACE))
			return EINVAL;
	} else if (flags != 0) {
		return EINVAL;
	}

	if (is_list) {
		if (name_addr != 0 || name_len != 0)
			return EINVAL;
	} else {
		if (name_len < 2 || name_len > XATTR_NAME_MAX + 1)
			return ERANGE;
		if (fetch_name(name_addr, name_len, name) != OK)
			return err_code;
		if (strlen(name) + 1 != name_len)
			return EINVAL;
		if ((r = vfs_xattr_name_valid(name)) != OK)
			return r;
	}

	/* Resolve either an existing descriptor or the requested pathname. */
	from_fd = fd >= 0;
	if (from_fd) {
		if (path_addr != 0 || path_len != 0)
			return EINVAL;
		if ((flp = get_filp(fd, write_access ? VNODE_WRITE :
		    VNODE_READ)) == NULL)
			return err_code;
		vp = flp->filp_vno;
		assert(vp != NULL);
		dup_vnode(vp);
		unlock_filp(flp);
	} else {
		if (fd != -1)
			return EBADF;
		if (path_len == 0 || path_len > sizeof(fullpath))
			return EINVAL;
		if (fetch_name(path_addr, path_len, fullpath) != OK)
			return err_code;
		if (strlen(fullpath) + 1 != path_len)
			return EINVAL;
		lookup_flags = nofollow ? PATH_RET_SYMLINK : PATH_NOFLAGS;
		lookup_init(&resolve, fullpath, lookup_flags, &vmp, &vp);
		resolve.l_vmnt_lock = VMNT_READ;
		resolve.l_vnode_lock = write_access ? VNODE_WRITE : VNODE_READ;
		if ((vp = eat_path(&resolve, fp)) == NULL)
			return err_code;
	}

	/* Reads require ordinary read permission. Mutations require write
	 * permission and ownership (or superuser credentials).
	 */
	if (write_access) {
		r = read_only(vp);
		if (r == OK)
			r = forbidden(fp, vp, W_BIT);
		if (r == OK && fp->fp_effuid != SU_UID &&
		    fp->fp_effuid != vp->v_uid)
			r = EPERM;
	} else {
		r = forbidden(fp, vp, R_BIT);
	}

	if (r == OK) {
		switch (job_call_nr) {
		case VFS_GETXATTR:
			r = req_getxattr(vp->v_fs_e, vp->v_inode_nr, name,
			    who_e, value_addr, size, &result_size);
			break;
		case VFS_SETXATTR:
			r = req_setxattr(vp->v_fs_e, vp->v_inode_nr, name,
			    who_e, value_addr, size, flags);
			break;
		case VFS_LISTXATTR:
			r = req_listxattr(vp->v_fs_e, vp->v_inode_nr,
			    who_e, value_addr, size, &result_size);
			break;
		case VFS_REMOVEXATTR:
			r = req_removexattr(vp->v_fs_e, vp->v_inode_nr, name);
			break;
		default:
			r = ENOSYS;
			break;
		}
	}

	if (from_fd) {
		/* As in fchmod/fchown, the descriptor owns the vnode lock. */
		unlock_filp(flp);
	} else {
		unlock_vnode(vp);
		unlock_vmnt(vmp);
	}
	put_vnode(vp);

	if (r != OK)
		return r;
	if (job_call_nr == VFS_GETXATTR || job_call_nr == VFS_LISTXATTR) {
		if (result_size > INT_MAX)
			return EOVERFLOW;
		return (int)result_size;
	}
	return OK;
}
