/* $Id: sun_uflash.c,v 1.1.1.1 2010/03/05 07:31:35 reynolds Exp $
 *
 * sun_uflash - Driver implementation for user-programmable flash
 * present on many Sun Microsystems SME boardsets.
 *
 * This driver does NOT provide access to the OBP-flash for
 * safety reasons-- use <linux>/drivers/sbus/char/flash.c instead.
 *
 * Copyright (c) 2001 Eric Brower (ebrower@usa.net)
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <asm/ebus.h>
#include <asm/oplib.h>
#include <asm/uaccess.h>
#include <asm/io.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/map.h>

#define UFLASH_OBPNAME	"flashprom"
#define UFLASH_DEVNAME 	"userflash"

#define UFLASH_WINDOW_SIZE	0x200000
#define UFLASH_BUSWIDTH		1			/* EBus is 8-bit */

MODULE_AUTHOR
	("Eric Brower <ebrower@usa.net>");
MODULE_DESCRIPTION
	("User-programmable flash device on Sun Microsystems boardsets");
MODULE_SUPPORTED_DEVICE
	("userflash");
MODULE_LICENSE
	("GPL");

static LIST_HEAD(device_list);
struct uflash_dev {
	char *			name;	/* device name */
	struct map_info 	map;	/* mtd map info */
	struct mtd_info *	mtd;	/* mtd info */
	struct list_head	list;
};

__u8 uflash_read8(struct map_info *map, unsigned long ofs)
{
	return(__raw_readb(map->map_priv_1 + ofs));
}

__u16 uflash_read16(struct map_info *map, unsigned long ofs)
{
	return(__raw_readw(map->map_priv_1 + ofs));
}

__u32 uflash_read32(struct map_info *map, unsigned long ofs)
{
	return(__raw_readl(map->map_priv_1 + ofs));
}

void uflash_copy_from(struct map_info *map, void *to, unsigned long from, 
		      ssize_t len)
{
	memcpy_fromio(to, map->map_priv_1 + from, len);
}

void uflash_write8(struct map_info *map, __u8 d, unsigned long adr)
{
	__raw_writeb(d, map->map_priv_1 + adr);
}

void uflash_write16(struct map_info *map, __u16 d, unsigned long adr)
{
	__raw_writew(d, map->map_priv_1 + adr);
}

void uflash_write32(struct map_info *map, __u32 d, unsigned long adr)
{
	__raw_writel(d, map->map_priv_1 + adr);
}

void uflash_copy_to(struct map_info *map, unsigned long to, const void *from,
		    ssize_t len)
{
	memcpy_toio(map->map_priv_1 + to, from, len);
}

struct map_info uflash_map_templ = {
		name:		"SUNW,???-????",
		size:		UFLASH_WINDOW_SIZE,
		buswidth:	UFLASH_BUSWIDTH,
		read8:		uflash_read8,
		read16:		uflash_read16,
		read32:		uflash_read32,
		copy_from:	uflash_copy_from,
		write8:		uflash_write8,
		write16:	uflash_write16,
		write32:	uflash_write32,
		copy_to:	uflash_copy_to
};

int uflash_devinit(struct linux_ebus_device* edev)
{
	int iTmp, nregs;
	struct linux_prom_registers regs[2];
	struct uflash_dev *pdev;

	iTmp = prom_getproperty(
		edev->prom_node, "reg", (void *)regs, sizeof(regs));
	if ((iTmp % sizeof(regs[0])) != 0) {
		printk("%s: Strange reg property size %d\n", 
			UFLASH_DEVNAME, iTmp);
		return -ENODEV;
	}

	nregs = iTmp / sizeof(regs[0]);

	if (nregs != 1) {
		/* Non-CFI userflash device-- once I find one we
		 * can work on supporting it.
		 */
		printk("%s: unsupported device at 0x%lx (%d regs): " \
			"email ebrower@usa.net\n", 
			UFLASH_DEVNAME, edev->resource[0].start, nregs);
		return -ENODEV;
	}

	if(0 == (pdev = kmalloc(sizeof(struct uflash_dev), GFP_KERNEL))) {
		printk("%s: unable to kmalloc new device\n", UFLASH_DEVNAME);
		return(-ENOMEM);
	}
	
	/* copy defaults and tweak parameters */
	memcpy(&pdev->map, &uflash_map_templ, sizeof(uflash_map_templ));
	pdev->map.size = regs[0].reg_size;

	iTmp = prom_getproplen(edev->prom_node, "model");
	pdev->name = kmalloc(iTmp, GFP_KERNEL);
	prom_getstring(edev->prom_node, "model", pdev->name, iTmp);
	if(0 != pdev->name && 0 < strlen(pdev->name)) {
		pdev->map.name = pdev->name;
	}

	pdev->map.map_priv_1 = 
		(unsigned long)ioremap_nocache(edev->resource[0].start, pdev->map.size);
	if(0 == pdev->map.map_priv_1) {
		printk("%s: failed to map device\n", __FUNCTION__);
		kfree(pdev->name);
		kfree(pdev);
		return(-1);
	}

	/* MTD registration */
	pdev->mtd = do_map_probe("cfi_probe", &pdev->map);
	if(0 == pdev->mtd) {
		iounmap((void *)pdev->map.map_priv_1);
		kfree(pdev->name);
		kfree(pdev);
		return(-ENXIO);
	}

	list_add(&pdev->list, &device_list);

	pdev->mtd->module = THIS_MODULE;

	add_mtd_device(pdev->mtd);
	return(0);
}

static int __init uflash_init(void)
{
	struct linux_ebus *ebus = NULL;
	struct linux_ebus_device *edev = NULL;

	for_each_ebus(ebus) {
		for_each_ebusdev(edev, ebus) {
			if (!strcmp(edev->prom_name, UFLASH_OBPNAME)) {
				if(0 > prom_getproplen(edev->prom_node, "user")) {
					DEBUG(2, "%s: ignoring device at 0x%lx\n",
							UFLASH_DEVNAME, edev->resource[0].start);
				} else {
					uflash_devinit(edev);
				}
			}
		}
	}

	if(list_empty(&device_list)) {
		printk("%s: unable to locate device\n", UFLASH_DEVNAME);
		return -ENODEV;
	}
	return(0);
}

static void __exit uflash_cleanup(void)
{
	struct list_head *udevlist;
	struct uflash_dev *udev;

	list_for_each(udevlist, &device_list) {
		udev = list_entry(udevlist, struct uflash_dev, list);
		DEBUG(2, "%s: removing device %s\n", 
			UFLASH_DEVNAME, udev->name);

		if(0 != udev->mtd) {
			del_mtd_device(udev->mtd);
			map_destroy(udev->mtd);
		}
		if(0 != udev->map.map_priv_1) {
			iounmap((void*)udev->map.map_priv_1);
			udev->map.map_priv_1 = 0;
		}
		if(0 != udev->name) {
			kfree(udev->name);
		}
		kfree(udev);
	}	
}

module_init(uflash_init);
module_exit(uflash_cleanup);
