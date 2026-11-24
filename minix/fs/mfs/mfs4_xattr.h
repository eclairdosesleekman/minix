#ifndef _MFS4_XATTR_H
#define _MFS4_XATTR_H

#include <sys/types.h>
#include <minix/fsdriver.h>

struct inode;

ssize_t mfs4_getxattr(struct inode *rip, const char *name,
    struct fsdriver_data *data, size_t size);
int mfs4_setxattr(struct inode *rip, const char *name,
    struct fsdriver_data *data, size_t size, int flags);
ssize_t mfs4_listxattr(struct inode *rip, struct fsdriver_data *data,
    size_t size);
int mfs4_removexattr(struct inode *rip, const char *name);

/* Called from the orphan finalization transaction; must not start a tx. */
int mfs4_xattr_free_chain(struct inode *rip);

#endif /* _MFS4_XATTR_H */
