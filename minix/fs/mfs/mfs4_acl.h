#ifndef _MFS4_ACL_H
#define _MFS4_ACL_H

#include <sys/types.h>

struct inode;

/* Caller is inside the callback's MFS4 transaction. */
int mfs4_acl_inherit(struct inode *parent, struct inode *child,
    int child_is_directory);
int mfs4_acl_chmod(struct inode *inode, mode_t new_mode);

#endif /* _MFS4_ACL_H */
