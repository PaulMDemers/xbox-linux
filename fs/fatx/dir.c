// SPDX-License-Identifier: GPL-2.0-only
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "fatx.h"

struct fatx_dirent {
	u8 name_len;
	u8 attr;
	u8 name[FATX_NAME_LEN];
	u8 reserved[0x2c - 2 - FATX_NAME_LEN];
	__le32 start_cluster;
	__le32 size;
} __packed;

static int fatx_read_cluster(struct super_block *sb, u32 cluster, void *buf)
{
	struct fatx_sb_info *sbi = fatx_sb(sb);
	sector_t block = fatx_cluster_block(sb, cluster);
	u8 *out = buf;
	u32 i;

	for (i = 0; i < sbi->cluster_blocks; i++) {
		struct buffer_head *bh = sb_bread(sb, block + i);

		if (!bh)
			return -EIO;
		memcpy(out + (i << sb->s_blocksize_bits), bh->b_data,
		       sb->s_blocksize);
		brelse(bh);
	}
	return 0;
}

static bool fatx_name_equal(const struct fatx_dirent *de,
			    const struct qstr *name)
{
	if (de->name_len != name->len)
		return false;
	return !strncasecmp(de->name, name->name, name->len);
}

static int fatx_find_entry(struct inode *dir, const struct qstr *name,
			   struct fatx_dirent *found)
{
	struct super_block *sb = dir->i_sb;
	struct fatx_sb_info *sbi = fatx_sb(sb);
	u8 *cluster_buf;
	u32 cluster = fatx_i(dir)->start_cluster;
	int ret = -ENOENT;

	cluster_buf = kmalloc(sbi->cluster_size, GFP_KERNEL);
	if (!cluster_buf)
		return -ENOMEM;

	while (cluster) {
		u32 offset;

		ret = fatx_read_cluster(sb, cluster, cluster_buf);
		if (ret)
			break;

		for (offset = 0; offset < sbi->cluster_size;
		     offset += FATX_DIRENT_SIZE) {
			struct fatx_dirent *de =
				(struct fatx_dirent *)(cluster_buf + offset);

			if (de->name_len == 0x00 || de->name_len == 0xff) {
				ret = -ENOENT;
				goto out;
			}
			if (de->name_len == 0xe5 || de->name_len > FATX_NAME_LEN)
				continue;
			if (fatx_name_equal(de, name)) {
				memcpy(found, de, sizeof(*found));
				ret = 0;
				goto out;
			}
		}
		cluster = fatx_next_cluster(sb, cluster);
	}

out:
	kfree(cluster_buf);
	return ret;
}

static struct dentry *fatx_lookup(struct inode *dir, struct dentry *dentry,
				  unsigned int flags)
{
	struct fatx_dirent de;
	struct inode *inode = NULL;
	int ret;

	if (dentry->d_name.len > FATX_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	ret = fatx_find_entry(dir, &dentry->d_name, &de);
	if (!ret) {
		inode = fatx_iget(dir->i_sb, le32_to_cpu(de.start_cluster),
				  de.attr, le32_to_cpu(de.size));
		if (IS_ERR(inode))
			return ERR_CAST(inode);
	} else if (ret != -ENOENT) {
		return ERR_PTR(ret);
	}

	d_add(dentry, inode);
	return NULL;
}

const struct inode_operations fatx_dir_inode_operations = {
	.lookup = fatx_lookup,
};

static int fatx_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *dir = file_inode(file);
	struct super_block *sb = dir->i_sb;
	struct fatx_sb_info *sbi = fatx_sb(sb);
	u8 *cluster_buf;
	u32 cluster = fatx_i(dir)->start_cluster;
	u64 entry_index = 0;
	int ret = 0;

	if (!dir_emit_dots(file, ctx))
		return 0;

	cluster_buf = kmalloc(sbi->cluster_size, GFP_KERNEL);
	if (!cluster_buf)
		return -ENOMEM;

	while (cluster) {
		u32 offset;

		ret = fatx_read_cluster(sb, cluster, cluster_buf);
		if (ret)
			break;

		for (offset = 0; offset < sbi->cluster_size;
		     offset += FATX_DIRENT_SIZE, entry_index++) {
			struct fatx_dirent *de =
				(struct fatx_dirent *)(cluster_buf + offset);
			unsigned int d_type;
			u32 start_cluster;

			if (entry_index < ctx->pos)
				continue;
			if (de->name_len == 0x00 || de->name_len == 0xff)
				goto out;
			if (de->name_len == 0xe5 || de->name_len > FATX_NAME_LEN)
				continue;

			start_cluster = le32_to_cpu(de->start_cluster);
			d_type = (de->attr & FATX_ATTR_DIR) ? DT_DIR : DT_REG;
			if (!dir_emit(ctx, de->name, de->name_len,
				      start_cluster + 1, d_type))
				goto out;
			ctx->pos = entry_index + 1;
		}
		cluster = fatx_next_cluster(sb, cluster);
	}

out:
	kfree(cluster_buf);
	return ret;
}

const struct file_operations fatx_dir_operations = {
	.llseek = generic_file_llseek,
	.iterate_shared = fatx_readdir,
	.read = generic_read_dir,
};
