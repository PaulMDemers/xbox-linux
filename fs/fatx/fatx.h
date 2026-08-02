/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _FATX_H
#define _FATX_H

#include <linux/fs.h>
#include <linux/types.h>

#define FATX_SUPER_MAGIC 0x58544146
#define FATX_MAGIC "FATX"
#define FATX_HEADER_SIZE 0x1000
#define FATX_DIRENT_SIZE 0x40
#define FATX_ROOT_INO 1
#define FATX_ROOT_CLUSTER 1
#define FATX_STD_E_CLUSTERS 0x4c7c0
#define FATX_NAME_LEN 42
#define FATX_ATTR_DIR 0x10
#define FATX_FREE_CLUSTER 0x00000000
#define FATX_EOF32 0xFFFFFFF8
#define FATX_EOF16 0xFFF8

struct fatx_sb_info {
	u32 sectors_per_cluster;
	u32 cluster_size;
	u32 cluster_bits;
	u32 cluster_blocks;
	u32 cluster_block_bits;
	u32 root_cluster;
	u32 cluster_count;
	u32 entry_size;
	sector_t fat_block;
	u32 fat_size;
	sector_t cluster1_block;
};

struct fatx_inode_info {
	u32 start_cluster;
	u32 cluster_count;
	u32 attr;
	bool contiguous;
	struct inode vfs_inode;
};

static inline struct fatx_sb_info *fatx_sb(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct fatx_inode_info *fatx_i(struct inode *inode)
{
	return container_of(inode, struct fatx_inode_info, vfs_inode);
}

extern const struct inode_operations fatx_dir_inode_operations;
extern const struct file_operations fatx_dir_operations;
extern const struct address_space_operations fatx_aops;

struct inode *fatx_iget(struct super_block *sb, u32 start_cluster, u32 attr,
			loff_t size);
u32 fatx_next_cluster(struct super_block *sb, u32 cluster);
sector_t fatx_cluster_block(struct super_block *sb, u32 cluster);

#endif
