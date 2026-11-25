#ifndef _FSCK_MFS4_H
#define _FSCK_MFS4_H

#define FSCK_MFS4_NOT_MFS4 (-1)

/* Return FSCK_MFS4_NOT_MFS4 for a legacy volume, 0 on success, or nonzero. */
int fsck_mfs4_dispatch(int argc, char **argv, const char *device);

#endif /* _FSCK_MFS4_H */
