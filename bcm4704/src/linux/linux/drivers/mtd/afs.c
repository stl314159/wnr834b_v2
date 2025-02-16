/*======================================================================

    drivers/mtd/afs.c: ARM Flash Layout/Partitioning
  
    Copyright (C) 2000 ARM Limited
  
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
  
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
  
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
  
   This is access code for flashes using ARM's flash partitioning 
   standards.

   $Id: afs.c,v 1.1.1.1 2010/03/05 07:31:35 reynolds Exp $

======================================================================*/

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/init.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/map.h>
#include <linux/mtd/partitions.h>

struct footer_struct {
	u32 image_info_base;	/* Address of first word of ImageFooter  */
	u32 image_start;	/* Start of area reserved by this footer */
	u32 signature;		/* 'Magic' number proves it's a footer   */
	u32 type;		/* Area type: ARM Image, SIB, customer   */
	u32 checksum;		/* Just this structure                   */
};

struct image_info_struct {
	u32 bootFlags;		/* Boot flags, compression etc.          */
	u32 imageNumber;	/* Unique number, selects for boot etc.  */
	u32 loadAddress;	/* Address program should be loaded to   */
	u32 length;		/* Actual size of image                  */
	u32 address;		/* Image is executed from here           */
	char name[16];		/* Null terminated                       */
	u32 headerBase;		/* Flash Address of any stripped header  */
	u32 header_length;	/* Length of header in memory            */
	u32 headerType;		/* AIF, RLF, s-record etc.               */
	u32 checksum;		/* Image checksum (inc. this struct)     */
};

static int
afs_read_footer(struct mtd_info *mtd, u_int *img_start, u_int *iis_start,
		u_int off, u_int mask)
{
	struct footer_struct fs;
	u_int ptr = off + mtd->erasesize - sizeof(fs);
	size_t sz;
	int ret;

	ret = mtd->read(mtd, ptr, sizeof(fs), &sz, (u_char *) &fs);
	if (ret >= 0 && sz != sizeof(fs))
		ret = -EINVAL;

	if (ret < 0) {
		printk(KERN_ERR "AFS: mtd read failed at 0x%x: %d\n",
			ptr, ret);
		return ret;
	}

	/*
	 * Does it contain the magic number?
	 */
	if (fs.signature != 0xa0ffff9f)
		ret = 1;

	/*
	 * Don't touch the SIB.
	 */
	if (fs.type == 2)
		ret = 1;

	*iis_start = fs.image_info_base & mask;
	*img_start = fs.image_start & mask;

	/*
	 * Check the image info base.  This can not
	 * be located after the footer structure.
	 */
	if (*iis_start >= ptr)
		ret = 1;

	/*
	 * Check the start of this image.  The image
	 * data can not be located after this block.
	 */
	if (*img_start > off)
		ret = 1;

	return ret;
}

static int
afs_read_iis(struct mtd_info *mtd, struct image_info_struct *iis, u_int ptr)
{
	size_t sz;
	int ret;

	memset(iis, 0, sizeof(*iis));
	ret = mtd->read(mtd, ptr, sizeof(*iis), &sz, (u_char *) iis);
	if (ret >= 0 && sz != sizeof(*iis))
		ret = -EINVAL;
	if (ret < 0)
		printk(KERN_ERR "AFS: mtd read failed at 0x%x: %d\n",
			ptr, ret);

	return ret;
}

int parse_afs_partitions(struct mtd_info *mtd, struct mtd_partition **pparts)
{
	struct mtd_partition *parts;
	u_int mask, off, idx, sz;
	int ret = 0;
	char *str;

	/*
	 * This is the address mask; we use this to mask off out of
	 * range address bits.
	 */
	mask = mtd->size - 1;

	/*
	 * First, calculate the size of the array we need for the
	 * partition information.  We include in this the size of
	 * the strings.
	 */
	for (idx = off = sz = 0; off < mtd->size; off += mtd->erasesize) {
		struct image_info_struct iis;
		u_int iis_ptr, img_ptr;

		ret = afs_read_footer(mtd, &img_ptr, &iis_ptr, off, mask);
		if (ret < 0)
			break;
		if (ret == 1)
			continue;

		ret = afs_read_iis(mtd, &iis, iis_ptr);
		if (ret < 0)
			break;

		sz += sizeof(struct mtd_partition);
		sz += strlen(iis.name) + 1;
		idx += 1;
	}

	if (!sz)
		return ret;

	parts = kmalloc(sz, GFP_KERNEL);
	if (!parts)
		return -ENOMEM;

	str = (char *)(parts + idx);

	/*
	 * Identify the partitions
	 */
	for (idx = off = 0; off < mtd->size; off += mtd->erasesize) {
		struct image_info_struct iis;
		u_int iis_ptr, img_ptr, size;

		/* Read the footer. */
		ret = afs_read_footer(mtd, &img_ptr, &iis_ptr, off, mask);
		if (ret < 0)
			break;
		if (ret == 1)
			continue;

		/* Read the image info block */
		ret = afs_read_iis(mtd, &iis, iis_ptr);
		if (ret < 0)
			break;

		strcpy(str, iis.name);
		size = mtd->erasesize + off - img_ptr;

		/*
		 * In order to support JFFS2 partitions on this layout,
		 * we must lie to MTD about the real size of JFFS2
		 * partitions; this ensures that the AFS flash footer
		 * won't be erased by JFFS2.  Please ensure that your
		 * JFFS2 partitions are given image numbers between
		 * 1000 and 2000 inclusive.
		 */
		if (iis.imageNumber >= 1000 && iis.imageNumber < 2000)
			size -= mtd->erasesize;

		parts[idx].name		= str;
		parts[idx].size		= size;
		parts[idx].offset	= img_ptr;
		parts[idx].mask_flags	= 0;

		printk("  mtd%d: at 0x%08x, %5dKB, %8u, %s\n",
			idx, img_ptr, parts[idx].size / 1024,
			iis.imageNumber, str);

		idx += 1;
		str = str + strlen(iis.name) + 1;
	}

	if (!idx) {
		kfree(parts);
		parts = NULL;
	}

	*pparts = parts;
	return idx ? idx : ret;
}

EXPORT_SYMBOL(parse_afs_partitions);

MODULE_AUTHOR("ARM Ltd");
MODULE_DESCRIPTION("ARM Firmware Suite partition parser");
MODULE_LICENSE("GPL");
