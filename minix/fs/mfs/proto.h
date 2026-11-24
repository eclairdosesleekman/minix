#ifndef __MFS_PROTO_H__
#define __MFS_PROTO_H__

/* Some shortcuts to functions in -lminixfs */
#define put_block(b) lmfs_put_block(b)

/* Function prototypes. */

/* Structs used in prototypes must be declared as such first. */
struct buf;
struct filp;		
struct inode;
struct super_block;

/* cache.c */
zone_t alloc_zone(dev_t dev, zone_t z);
void free_zone(dev_t dev, zone_t numb);
struct buf *get_block(dev_t dev, block_t block, int how);

/* inode.c */
struct inode *alloc_inode(dev_t dev, mode_t bits, uid_t uid, gid_t gid);
void dup_inode(struct inode *ip);
struct inode *find_inode(dev_t dev, ino_t numb);
int fs_putnode(ino_t ino_nr, unsigned int count);
void init_inode_cache(void);
struct inode *get_inode(dev_t dev, ino_t numb);
void put_inode(struct inode *rip);
void update_times(struct inode *rip);
int rw_inode(struct inode *rip, int rw_flag);
bit_t mfs4_alloc_bit(struct super_block *sp, int map, bit_t origin);
void mfs4_free_bit(struct super_block *sp, int map, bit_t bit_returned);
bit_t mfs4_count_free_bits(struct super_block *sp, int map);
int mfs4_probe_super(struct super_block *sp, int *is_mfs4);
int mfs4_next_generation(ino_t inum, uint32_t *generation);
int mfs4_rw_inode(struct inode *rip, int rw_flag);
int mfs4_encode_rdev(dev_t native_dev, u64_t *disk_rdev);
int mfs4_decode_rdev(u64_t disk_rdev, dev_t *native_dev);
int mfs4_read_map_checked(struct inode *rip, off_t position,
	block_t *block_out);
block_t mfs4_read_map(struct inode *rip, off_t position);
int mfs4_write_map(struct inode *rip, off_t position, zone_t new_block, int op);
int mfs4_free_map_range(struct inode *rip, u64_t first_block, u64_t end_block);
int mfs4_find_last_allocated_block(struct inode *rip, u64_t first_block,
	int *found_out, u64_t *last_block_out);
int mfs4_zero_eof_tail(struct inode *rip, u64_t old_eof, u64_t new_eof);
int mfs4_journal_mount(struct super_block *sp, int readonly);
int mfs4_journal_sync(void);
int mfs4_journal_unmount(void);
int mfs4_journal_mark_dirty(void);
int mfs4_tx_is_active(void);
int mfs4_tx_note_block_delta(int delta);
int mfs4_tx_note_inode(struct inode *rip);
int mfs4_tx_begin(void);
int mfs4_tx_get_meta(u64_t home_block, u32_t metadata_type,
    uint8_t **image_out);
int mfs4_tx_find_meta(u64_t home_block, const uint8_t **image_out);
int mfs4_tx_commit(void);
void mfs4_tx_abort(void);
int mfs3_inode_decode(struct inode *rip, const d2_inode *dip, int norm);
int mfs3_inode_encode(const struct inode *rip, d2_inode *dip, int norm);

/* link.c */
int fs_trunc(ino_t ino_nr, off_t start, off_t end);
int fs_link(ino_t dir_nr, char *name, ino_t ino_nr);
ssize_t fs_rdlink(ino_t ino_nr, struct fsdriver_data *data, size_t bytes);
int fs_rename(ino_t old_dir_nr, char *old_name, ino_t new_dir_nr,
	char *new_name);
int fs_unlink(ino_t dir_nr, char *name, int call);
int truncate_inode(struct inode *rip, off_t len);

/* misc.c */
int fs_sync(void);

/* mount.c */
int fs_mount(dev_t dev, unsigned int flags, struct fsdriver_node *root_node,
	unsigned int *res_flags);
int fs_unmount(void);
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
	int flag);

/* protect.c */
int fs_chmod(ino_t ino, mode_t *mode);
int fs_chown(ino_t ino, uid_t uid, gid_t gid, mode_t *mode);

/* read.c */
ssize_t fs_readwrite(ino_t ino_nr, struct fsdriver_data *data, size_t bytes,
	off_t pos, int call);
ssize_t fs_readwrite_offset(ino_t ino_nr, struct fsdriver_data *data,
	size_t bytes, off_t pos, int call, size_t data_offset);
block_t read_map(struct inode *rip, off_t pos, int opportunistic);
struct buf *get_block_map(register struct inode *rip, u64_t position);
zone_t rd_indir(struct buf *bp, int index);
ssize_t fs_getdents(ino_t ino_nr, struct fsdriver_data *data, size_t bytes,
	off_t *posp);

/* stadir.c */
int fs_stat(ino_t ino_nr, struct stat *statbuf);
int fs_statvfs(struct statvfs *st);

/* super.c */
bit_t alloc_bit(struct super_block *sp, int map, bit_t origin);
void free_bit(struct super_block *sp, int map, bit_t bit_returned);
unsigned int get_block_size(dev_t dev);
int read_super(struct super_block *sp);
int write_super(struct super_block *sp);

/* stats.c */
bit_t count_free_bits(struct super_block *sp, int map);

/* time.c */
int fs_utime(ino_t ino_t, struct timespec *atime, struct timespec *mtime);

/* utility.c */
unsigned conv2(int norm, int w);
long conv4(int norm, long x);

/* write.c */
void clear_zone(struct inode *rip, off_t pos, int flag);
struct buf *new_block(struct inode *rip, off_t position);
void zero_block(struct buf *bp);
int write_map(struct inode *, off_t, zone_t, int);

#endif

