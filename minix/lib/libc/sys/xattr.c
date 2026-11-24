#include <sys/cdefs.h>
#include "namespace.h"
#include <lib.h>

#include <sys/xattr.h>
#include <minix/callnr.h>
#include <minix/vfsif.h>
#include <string.h>

static int
xattr_call(int call, int fd, const char *path, const char *name,
    const void *value, size_t size, int flags)
{
	message m;

	memset(&m, 0, sizeof(m));
	m.m9_l1 = fd;
	m.m9_l2 = flags;
	m.m9_ull1 = path != NULL ? (uint64_t)(vir_bytes)path : 0;
	m.m9_l5 = path != NULL ? (long)(strlen(path) + 1) : 0;
	m.m9_ull2 = name != NULL ? (uint64_t)(vir_bytes)name : 0;
	m.m9_s1 = name != NULL ? (short)(strlen(name) + 1) : 0;
	m.m9_l3 = (long)(vir_bytes)value;
	m.m9_l4 = (long)size;

	return _syscall(VFS_PROC_NR, call, &m);
}

int
setxattr(const char *path, const char *name, const void *value,
    size_t size, int flags)
{
	return xattr_call(VFS_SETXATTR, -1, path, name, value, size, flags);
}

int
lsetxattr(const char *path, const char *name, const void *value,
    size_t size, int flags)
{
	return xattr_call(VFS_SETXATTR, -1, path, name, value, size,
	    flags | VFS_XATTR_NOFOLLOW);
}

int
fsetxattr(int fd, const char *name, const void *value, size_t size,
    int flags)
{
	return xattr_call(VFS_SETXATTR, fd, NULL, name, value, size, flags);
}

ssize_t
getxattr(const char *path, const char *name, void *value, size_t size)
{
	return (ssize_t)xattr_call(VFS_GETXATTR, -1, path, name, value, size, 0);
}

ssize_t
lgetxattr(const char *path, const char *name, void *value, size_t size)
{
	return (ssize_t)xattr_call(VFS_GETXATTR, -1, path, name, value, size,
	    VFS_XATTR_NOFOLLOW);
}

ssize_t
fgetxattr(int fd, const char *name, void *value, size_t size)
{
	return (ssize_t)xattr_call(VFS_GETXATTR, fd, NULL, name, value, size, 0);
}

ssize_t
listxattr(const char *path, char *list, size_t size)
{
	return (ssize_t)xattr_call(VFS_LISTXATTR, -1, path, NULL, list, size, 0);
}

ssize_t
llistxattr(const char *path, char *list, size_t size)
{
	return (ssize_t)xattr_call(VFS_LISTXATTR, -1, path, NULL, list, size,
	    VFS_XATTR_NOFOLLOW);
}

ssize_t
flistxattr(int fd, char *list, size_t size)
{
	return (ssize_t)xattr_call(VFS_LISTXATTR, fd, NULL, NULL, list, size, 0);
}

int
removexattr(const char *path, const char *name)
{
	return xattr_call(VFS_REMOVEXATTR, -1, path, name, NULL, 0, 0);
}

int
lremovexattr(const char *path, const char *name)
{
	return xattr_call(VFS_REMOVEXATTR, -1, path, name, NULL, 0,
	    VFS_XATTR_NOFOLLOW);
}

int
fremovexattr(int fd, const char *name)
{
	return xattr_call(VFS_REMOVEXATTR, fd, NULL, name, NULL, 0, 0);
}
