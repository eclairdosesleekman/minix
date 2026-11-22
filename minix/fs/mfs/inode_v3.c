/*
 * MFS3 inode disk codec.
 *
 * Keep the legacy 64-byte d2_inode layout isolated from the common in-memory
 * inode. In particular, never memcpy a struct inode to or from disk.
 */

#include "fs.h"
#include "inode.h"
#include "type.h"

int
mfs3_inode_decode(struct inode *rip, const d2_inode *dip, int norm)
{
    int i;
    i32_t size;

    /* The legacy UID field is signed in the C declaration, but its on-disk
     * representation is an unsigned 16-bit value.
     */
    rip->i_mode = (u16_t)conv2(norm, dip->d2_mode);
    rip->i_uid = (u32_t)conv2(norm, (u16_t)dip->d2_uid);
    rip->i_nlinks = (u32_t)conv2(norm, dip->d2_nlinks);
    rip->i_gid = (u32_t)conv2(norm, dip->d2_gid);

    /* MFS3's size field is a signed 32-bit value. Negative sizes are corrupt. */
    size = (i32_t)conv4(norm, (long)dip->d2_size);
    if (size < 0)
        return(EIO);
    rip->i_size = (u64_t)size;

    /* Keep the legacy signed-seconds interpretation, including pre-epoch
     * timestamps, while extending it to the common 64-bit representation.
     */
    rip->i_atime = (i64_t)(i32_t)conv4(norm, (long)dip->d2_atime);
    rip->i_mtime = (i64_t)(i32_t)conv4(norm, (long)dip->d2_mtime);
    rip->i_ctime = (i64_t)(i32_t)conv4(norm, (long)dip->d2_ctime);

    rip->i_ndzones = V2_NR_DZONES;
    rip->i_nindirs = V2_INDIRECTS(rip->i_sp->s_block_size);
    for (i = 0; i < V2_NR_TZONES; i++)
        rip->i_zone[i] =
            (zone_t)conv4(norm, (long)dip->d2_zone[i]);

    return(OK);
}

int
mfs3_inode_encode(const struct inode *rip, d2_inode *dip, int norm)
{
    int i;

    /* Validate every narrowing conversion before changing the disk image. */
    if (rip->i_uid > 0xffffU || rip->i_gid > 0xffffU)
        return(EOVERFLOW);
    if (rip->i_nlinks > 0xffffU)
        return(EMLINK);
    if (rip->i_size > 0x7fffffffULL)
        return(EFBIG);
    if (rip->i_atime < -2147483648LL || rip->i_atime > 2147483647LL ||
        rip->i_mtime < -2147483648LL || rip->i_mtime > 2147483647LL ||
        rip->i_ctime < -2147483648LL || rip->i_ctime > 2147483647LL)
        return(EOVERFLOW);

    dip->d2_mode = (u16_t)conv2(norm, rip->i_mode);
    dip->d2_uid = (i16_t)conv2(norm, (u16_t)rip->i_uid);
    dip->d2_nlinks = (u16_t)conv2(norm, (u16_t)rip->i_nlinks);
    dip->d2_gid = (u16_t)conv2(norm, (u16_t)rip->i_gid);
    dip->d2_size = (i32_t)conv4(norm, (long)(i32_t)rip->i_size);
    dip->d2_atime = (i32_t)conv4(norm, (long)(i32_t)rip->i_atime);
    dip->d2_mtime = (i32_t)conv4(norm, (long)(i32_t)rip->i_mtime);
    dip->d2_ctime = (i32_t)conv4(norm, (long)(i32_t)rip->i_ctime);
    for (i = 0; i < V2_NR_TZONES; i++)
        dip->d2_zone[i] =
            (zone_t)conv4(norm, (long)rip->i_zone[i]);

    return(OK);
}
