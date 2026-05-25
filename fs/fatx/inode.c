// SPDX-License-Identifier: GPL-2.0-only
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/highuid.h>
#include <linux/module.h>
#include <linux/mpage.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "fatx.h"

static struct kmem_cache *fatx_inode_cachep;

static struct inode *fatx_alloc_inode(struct super_block *sb)
{
	struct fatx_inode_info *fi;

	fi = alloc_inode_sb(sb, fatx_inode_cachep, GFP_KERNEL);
	if (!fi)
		return NULL;
	return &fi->vfs_inode;
}

static void fatx_free_inode(struct inode *inode)
{
	kmem_cache_free(fatx_inode_cachep, fatx_i(inode));
}

static void fatx_init_once(void *foo)
{
	struct fatx_inode_info *fi = foo;

	inode_init_once(&fi->vfs_inode);
}

static int fatx_init_inodecache(void)
{
	fatx_inode_cachep = kmem_cache_create("fatx_inode_cache",
					      sizeof(struct fatx_inode_info),
					      0,
					      SLAB_RECLAIM_ACCOUNT |
					      SLAB_ACCOUNT,
					      fatx_init_once);
	return fatx_inode_cachep ? 0 : -ENOMEM;
}

static void fatx_destroy_inodecache(void)
{
	rcu_barrier();
	kmem_cache_destroy(fatx_inode_cachep);
}

static void fatx_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

static int fatx_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct fatx_sb_info *sbi = fatx_sb(sb);

	buf->f_type = FATX_SUPER_MAGIC;
	buf->f_bsize = sbi->cluster_size;
	buf->f_blocks = sbi->cluster_count;
	buf->f_bfree = 0;
	buf->f_bavail = 0;
	buf->f_files = 0;
	buf->f_ffree = 0;
	buf->f_namelen = FATX_NAME_LEN;
	return 0;
}

static const struct super_operations fatx_sops = {
	.alloc_inode = fatx_alloc_inode,
	.free_inode = fatx_free_inode,
	.evict_inode = fatx_evict_inode,
	.statfs = fatx_statfs,
};

sector_t fatx_cluster_block(struct super_block *sb, u32 cluster)
{
	struct fatx_sb_info *sbi = fatx_sb(sb);

	return sbi->cluster1_block + ((sector_t)(cluster - 1) *
				      sbi->cluster_blocks);
}

static bool fatx_eof(struct super_block *sb, u32 value)
{
	struct fatx_sb_info *sbi = fatx_sb(sb);

	if (sbi->entry_size == 4)
		return value >= FATX_EOF32;
	return value >= FATX_EOF16;
}

u32 fatx_next_cluster(struct super_block *sb, u32 cluster)
{
	struct fatx_sb_info *sbi = fatx_sb(sb);
	sector_t fat_byte = FATX_HEADER_SIZE + ((sector_t)cluster *
			     sbi->entry_size);
	sector_t block = fat_byte >> sb->s_blocksize_bits;
	u32 offset = fat_byte & (sb->s_blocksize - 1);
	struct buffer_head *bh;
	u32 next;

	if (cluster < FATX_ROOT_CLUSTER || cluster >= sbi->cluster_count)
		return 0;

	bh = sb_bread(sb, block);
	if (!bh)
		return 0;
	if (sbi->entry_size == 4)
		next = le32_to_cpup((__le32 *)(bh->b_data + offset));
	else
		next = le16_to_cpup((__le16 *)(bh->b_data + offset));
	brelse(bh);

	if (next == FATX_FREE_CLUSTER || fatx_eof(sb, next))
		return 0;
	if (next < FATX_ROOT_CLUSTER || next >= sbi->cluster_count)
		return 0;
	return next;
}

static u32 fatx_file_cluster(struct inode *inode, sector_t iblock)
{
	struct super_block *sb = inode->i_sb;
	struct fatx_sb_info *sbi = fatx_sb(sb);
	struct fatx_inode_info *fi = fatx_i(inode);
	u32 index = iblock >> sbi->cluster_block_bits;
	u32 cluster = fi->start_cluster;

	if (index >= fi->cluster_count)
		return 0;

	if (fi->contiguous) {
		cluster += index;
		if (cluster < FATX_ROOT_CLUSTER || cluster >= sbi->cluster_count)
			return 0;
		return cluster;
	}

	while (index-- && cluster)
		cluster = fatx_next_cluster(sb, cluster);
	return cluster;
}

static int fatx_get_block(struct inode *inode, sector_t iblock,
			  struct buffer_head *bh, int create)
{
	struct super_block *sb = inode->i_sb;
	struct fatx_sb_info *sbi = fatx_sb(sb);
	u32 cluster = fatx_file_cluster(inode, iblock);
	sector_t offset;

	if (!cluster)
		return 0;

	offset = iblock & (sbi->cluster_blocks - 1);
	map_bh(bh, sb, fatx_cluster_block(sb, cluster) + offset);
	return 0;
}

static int fatx_read_folio(struct file *file, struct folio *folio)
{
	return block_read_full_folio(folio, fatx_get_block);
}

static sector_t fatx_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, fatx_get_block);
}

const struct address_space_operations fatx_aops = {
	.read_folio = fatx_read_folio,
	.bmap = fatx_bmap,
};

static u32 fatx_cluster_count_for_size(struct super_block *sb, loff_t size)
{
	struct fatx_sb_info *sbi = fatx_sb(sb);
	u64 clusters;

	if (size <= 0)
		return 0;

	clusters = ((u64)size + sbi->cluster_size - 1) >> sbi->cluster_bits;
	if (clusters > sbi->cluster_count)
		return sbi->cluster_count;
	return (u32)clusters;
}

static bool fatx_chain_is_contiguous(struct super_block *sb,
				     u32 start_cluster, u32 cluster_count)
{
	struct fatx_sb_info *sbi = fatx_sb(sb);
	u32 cluster = start_cluster;
	u32 index;

	if (!cluster_count)
		return false;
	if (start_cluster < FATX_ROOT_CLUSTER ||
	    start_cluster >= sbi->cluster_count)
		return false;
	if (cluster_count == 1)
		return true;

	for (index = 1; index < cluster_count; index++) {
		u32 expected = cluster + 1;
		u32 next;

		if (expected >= sbi->cluster_count)
			return false;

		next = fatx_next_cluster(sb, cluster);
		if (next != expected)
			return false;
		cluster = next;
	}

	return true;
}

struct inode *fatx_iget(struct super_block *sb, u32 start_cluster, u32 attr,
			loff_t size)
{
	struct inode *inode;
	struct fatx_inode_info *fi;
	unsigned long ino = start_cluster == FATX_ROOT_CLUSTER ?
			    FATX_ROOT_INO : start_cluster + 1;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	fi = fatx_i(inode);
	fi->start_cluster = start_cluster;
	fi->cluster_count = fatx_cluster_count_for_size(sb, size);
	fi->attr = attr;
	fi->contiguous = !(attr & FATX_ATTR_DIR) &&
			 fatx_chain_is_contiguous(sb, start_cluster,
						  fi->cluster_count);

	i_uid_write(inode, 0);
	i_gid_write(inode, 0);
	inode_set_atime_to_ts(inode, current_time(inode));
	inode_set_mtime_to_ts(inode, current_time(inode));
	inode_set_ctime_to_ts(inode, current_time(inode));

	if (attr & FATX_ATTR_DIR) {
		inode->i_mode = S_IFDIR | 0755;
		set_nlink(inode, 2);
		inode->i_size = size ? size : fatx_sb(sb)->cluster_size;
		inode->i_op = &fatx_dir_inode_operations;
		inode->i_fop = &fatx_dir_operations;
	} else {
		inode->i_mode = S_IFREG | 0644;
		set_nlink(inode, 1);
		inode->i_size = size;
		inode->i_fop = &generic_ro_fops;
		inode->i_data.a_ops = &fatx_aops;
	}
	inode->i_blocks = (inode->i_size + 511) >> 9;

	unlock_new_inode(inode);
	return inode;
}

static int fatx_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct fatx_sb_info *sbi;
	struct buffer_head *bh;
	struct inode *root;
	u32 sectors_per_cluster;
	u32 root_cluster;
	u64 fat_bytes;

	if (!sb_set_blocksize(sb, 512))
		return -EINVAL;

	bh = sb_bread(sb, 0);
	if (!bh) {
		pr_err("FATX-fs: unable to read header block\n");
		return -EIO;
	}
	if (memcmp(bh->b_data, FATX_MAGIC, 4)) {
		pr_err("FATX-fs: missing FATX magic\n");
		brelse(bh);
		return -EINVAL;
	}

	sectors_per_cluster = le32_to_cpup((__le32 *)(bh->b_data + 8));
	root_cluster = le32_to_cpup((__le32 *)(bh->b_data + 12));
	brelse(bh);

	if (!sectors_per_cluster || root_cluster != FATX_ROOT_CLUSTER) {
		pr_err("FATX-fs: unsupported header sectors_per_cluster=%u root_cluster=%u\n",
		       sectors_per_cluster, root_cluster);
		return -EINVAL;
	}

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sbi->sectors_per_cluster = sectors_per_cluster;
	sbi->cluster_size = sectors_per_cluster << 9;
	if (!is_power_of_2(sbi->cluster_size)) {
		pr_err("FATX-fs: unsupported cluster size %u\n",
		       sbi->cluster_size);
		kfree(sbi);
		return -EINVAL;
	}
	sbi->cluster_bits = ilog2(sbi->cluster_size);
	sbi->cluster_blocks = sectors_per_cluster;
	sbi->cluster_block_bits = ilog2(sbi->cluster_blocks);
	sbi->root_cluster = root_cluster;
	sbi->cluster_count = i_size_read(sb->s_bdev->bd_mapping->host) >>
			     sbi->cluster_bits;
	if (sbi->cluster_size == 16384 &&
	    sbi->cluster_count > FATX_STD_E_CLUSTERS)
		sbi->cluster_count = FATX_STD_E_CLUSTERS;
	sbi->entry_size = sbi->cluster_count >= 0xFFF4 ? 4 : 2;
	fat_bytes = roundup((u64)sbi->cluster_count * sbi->entry_size,
			    FATX_HEADER_SIZE);
	sbi->fat_block = FATX_HEADER_SIZE >> 9;
	sbi->fat_size = fat_bytes;
	sbi->cluster1_block = (FATX_HEADER_SIZE + fat_bytes) >> 9;

	sb->s_fs_info = sbi;
	sb->s_magic = FATX_SUPER_MAGIC;
	sb->s_op = &fatx_sops;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_flags |= SB_RDONLY;

	root = fatx_iget(sb, FATX_ROOT_CLUSTER, FATX_ATTR_DIR,
			 sbi->cluster_size);
	if (IS_ERR(root)) {
		kfree(sbi);
		sb->s_fs_info = NULL;
		return PTR_ERR(root);
	}
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		kfree(sbi);
		sb->s_fs_info = NULL;
		return -ENOMEM;
	}

	pr_info("FATX-fs: mounted read-only, cluster_size=%u clusters=%u\n",
		sbi->cluster_size, sbi->cluster_count);
	return 0;
}

static int fatx_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, fatx_fill_super);
}

static int fatx_reconfigure(struct fs_context *fc)
{
	fc->sb_flags |= SB_RDONLY;
	return 0;
}

static const struct fs_context_operations fatx_context_ops = {
	.get_tree = fatx_get_tree,
	.reconfigure = fatx_reconfigure,
};

static int fatx_init_fs_context(struct fs_context *fc)
{
	fc->ops = &fatx_context_ops;
	return 0;
}

static void fatx_kill_sb(struct super_block *sb)
{
	struct fatx_sb_info *sbi = sb->s_fs_info;

	kill_block_super(sb);
	kfree(sbi);
}

static struct file_system_type fatx_fs_type = {
	.owner = THIS_MODULE,
	.name = "fatx",
	.init_fs_context = fatx_init_fs_context,
	.kill_sb = fatx_kill_sb,
	.fs_flags = FS_REQUIRES_DEV,
};

static int __init fatx_init(void)
{
	int ret;

	ret = fatx_init_inodecache();
	if (ret)
		return ret;
	ret = register_filesystem(&fatx_fs_type);
	if (ret)
		fatx_destroy_inodecache();
	return ret;
}

static void __exit fatx_exit(void)
{
	unregister_filesystem(&fatx_fs_type);
	fatx_destroy_inodecache();
}

module_init(fatx_init);
module_exit(fatx_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("Read-only original Xbox FATX filesystem");
