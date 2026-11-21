#ifndef EXT2_PROTO_H
#define EXT2_PROTO_H

#define put_block(n) lmfs_put_block(n)

/* Function prototypes. */

/* Structs used in prototypes must be declared as such first. */
struct buf;
struct filp;
struct inode;
struct super_block;

/* balloc.c */
void discard_preallocated_blocks(struct inode *rip);
block64_t alloc_block(struct inode *rip, block64_t goal);
void free_block(struct super_block *sp, block64_t bit);

/* ialloc.c */
struct inode *alloc_inode(struct inode *parent, mode_t bits, uid_t uid,
	gid_t gid);
void free_inode(struct inode *rip);

/* inode.c */
void dup_inode(struct inode *ip);
struct inode *find_inode(dev_t dev, ino_t numb);
int fs_putnode(ino_t ino_nr, unsigned int count);
void init_inode_cache(void);
struct inode *get_inode(dev_t dev, ino_t numb);
void put_inode(struct inode *rip);
void update_times(struct inode *rip);
int rw_inode(struct inode *rip, int rw_flag);

/* link.c */
int fs_trunc(ino_t ino_nr, off_t start, off_t end);
int fs_link(ino_t dir_nr, char *name, ino_t ino_nr);
ssize_t fs_rdlink(ino_t ino_nr, struct fsdriver_data *data, size_t bytes);
int fs_rename(ino_t old_dir_nr, char *old_name, ino_t new_dir_nr,
	char *new_name);
int fs_unlink(ino_t dir_nr, char *name, int call);
int truncate_inode(struct inode *rip, off_t len);

/* misc.c */
void fs_sync(void);

/* mount.c */
int fs_mount(dev_t dev, unsigned int flags, struct fsdriver_node *root_node,
	unsigned int *res_flags);
void fs_unmount(void);
int fs_mountpt(ino_t ino_nr);

/* open.c */
int fs_create(ino_t dir_nr, char *name, mode_t mode, uid_t uid, gid_t gid,
	struct fsdriver_node *node);
int fs_mkdir(ino_t dir_nr, char *name, mode_t mode, uid_t uid, gid_t gid);
int fs_mknod(ino_t dir_nr, char *name, mode_t mode, uid_t uid, gid_t gid,
	dev_t dev);
int fs_slink(ino_t dir_nr, char *name, uid_t uid, gid_t gid,
	struct fsdriver_data *data, size_t bytes);
void fs_seek(ino_t ino_nr);

/* path.c */
int fs_lookup(ino_t dir_nr, char *name, struct fsdriver_node *node,
	int *is_mountpt);
struct inode *advance(struct inode *dirp, const char *string);
int search_dir(struct inode *ldir_ptr, const char *string, ino_t *numb,
	int flag, int ftype);

/* protect.c */
int fs_chmod(ino_t ino_nr, mode_t *mode);
int fs_chown(ino_t ino_nr, uid_t uid, gid_t gid, mode_t *mode);

/* read.c */
ssize_t fs_readwrite(ino_t ino_nr, struct fsdriver_data *data, size_t bytes,
	off_t pos, int call);
block64_t rd_indir(struct buf *bp, int index);
block64_t read_map(struct inode *rip, off_t pos, int opportunistic);
struct buf *get_block_map(register struct inode *rip, u64_t position);
ssize_t fs_getdents(ino_t ino_nr, struct fsdriver_data *data, size_t bytes,
	off_t *posp);

/* stadir.c */
int fs_stat(ino_t ino_nr, struct stat *statbuf);
int fs_statvfs(struct statvfs *st);

/* super.c */
unsigned int get_block_size(dev_t dev);
struct super_block *get_super(dev_t dev);
int read_super(struct super_block *sp);
void write_super(struct super_block *sp);
int ext4_update_bitmap_checksum(struct super_block *sp, unsigned int group,
	const unsigned char *bitmap, int inode_bitmap);
int ext4_check_rw_mount(struct super_block *sp);
struct group_desc* get_group_desc(unsigned int bnum);

/* time.c */
int fs_utime(ino_t ino, struct timespec *atime, struct timespec *mtime);

/* utility.c */
struct buf *get_block(dev_t dev, block64_t block, int how);
u32_t ext4_crc32c(u32_t crc, const void *data, size_t len);
u32_t ext4_inode_csum_seed(struct inode *rip);
int ext4_verify_inode_csum(struct super_block *sp, ino_t ino,
	const unsigned char *raw, size_t inode_size);
int ext4_update_inode_csum(struct super_block *sp, ino_t ino,
	unsigned char *raw, size_t inode_size);
int ext4_validate_xattr_block(struct super_block *sp, dev_t dev,
	block64_t block);
int ext4_verify_dir_block(struct inode *rip, const unsigned char *data,
	unsigned int logical_block);
int ext4_update_dir_block_csum(struct inode *rip, unsigned char *data);
unsigned conv2(int norm, int w);
long conv4(int norm, long x);
unsigned int ext4_dir_rec_len(const struct ext2_disk_dir_desc *dp,
	unsigned int block_size);
void ext4_set_dir_rec_len(struct ext2_disk_dir_desc *dp,
	unsigned int rec_len, unsigned int block_size);
int ext4_valid_dirent(const struct ext2_disk_dir_desc *dp,
	unsigned int offset, unsigned int block_size);
int ansi_strcmp(register const char* ansi_s, register const char *s2,
	register size_t ansi_s_length);
bit_t setbit(bitchunk_t *bitmap, bit_t max_bits, unsigned int word);
bit_t setbyte(bitchunk_t *bitmap, bit_t max_bits);
int unsetbit(bitchunk_t *bitmap, bit_t bit);

/* write.c */
struct buf *new_block(struct inode *rip, off_t position);
void zero_block(struct buf *bp);
int write_map(struct inode *, off_t, block64_t, int);
int ext4_extent_write_map(struct inode *, off_t, block64_t, int);

#endif /* EXT2_PROTO_H */
