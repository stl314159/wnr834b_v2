/* 
 * Direct MTD block device access
 *
 * $Id: mtdblock_ro.c,v 1.1.1.1 2010/03/05 07:31:35 reynolds Exp $
 *
 * 02-nov-2000	Nicolas Pitre		Added read-modify-write with cache
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/compatmac.h>

#define MAJOR_NR MTD_BLOCK_MAJOR
#define DEVICE_NAME "mtdblock"
#define DEVICE_REQUEST mtdblock_request
#define DEVICE_NR(device) (device)
#define DEVICE_ON(device)
#define DEVICE_OFF(device)
#define DEVICE_NO_RANDOM
#include <linux/blk.h>
/* for old kernels... */
#ifndef QUEUE_EMPTY
#define QUEUE_EMPTY  (!CURRENT)
#endif
#if LINUX_VERSION_CODE < 0x20300
#define QUEUE_PLUGGED (blk_dev[MAJOR_NR].plug_tq.sync)
#else
#define QUEUE_PLUGGED (blk_dev[MAJOR_NR].request_queue.plugged)
#endif

#ifdef CONFIG_DEVFS_FS
#include <linux/devfs_fs_kernel.h>
static void mtd_notify_add(struct mtd_info* mtd);
static void mtd_notify_remove(struct mtd_info* mtd);
static struct mtd_notifier notifier = {
        mtd_notify_add,
        mtd_notify_remove,
        NULL
};
static devfs_handle_t devfs_dir_handle = NULL;
static devfs_handle_t devfs_ro_handle[MAX_MTD_DEVICES];
#endif

static struct mtdblk_dev {
	struct mtd_info *mtd; /* Locked */
	int count;
} *mtdblks[MAX_MTD_DEVICES];

static spinlock_t mtdblks_lock;

static int mtd_sizes[MAX_MTD_DEVICES];

static int mtdblock_open(struct inode *inode, struct file *file)
{
	struct mtdblk_dev *mtdblk;
	struct mtd_info *mtd;
	int dev;

	DEBUG(MTD_DEBUG_LEVEL1,"mtdblock_open\n");
	
	if (!inode)
		return -EINVAL;
	
	dev = MINOR(inode->i_rdev);
	if (dev >= MAX_MTD_DEVICES)
		return -EINVAL;

	mtd = get_mtd_device(NULL, dev);
	if (!mtd)
		return -ENODEV;
	if (MTD_ABSENT == mtd->type) {
		put_mtd_device(mtd);
		return -ENODEV;
	}
	
	spin_lock(&mtdblks_lock);

	/* If it's already open, no need to piss about. */
	if (mtdblks[dev]) {
		mtdblks[dev]->count++;
		spin_unlock(&mtdblks_lock);
		return 0;
	}
	
	/* OK, it's not open. Try to find it */

	/* First we have to drop the lock, because we have to
	   to things which might sleep.
	*/
	spin_unlock(&mtdblks_lock);

	mtdblk = kmalloc(sizeof(struct mtdblk_dev), GFP_KERNEL);
	if (!mtdblk) {
		put_mtd_device(mtd);
		return -ENOMEM;
	}
	memset(mtdblk, 0, sizeof(*mtdblk));
	mtdblk->count = 1;
	mtdblk->mtd = mtd;

	/* OK, we've created a new one. Add it to the list. */

	spin_lock(&mtdblks_lock);

	if (mtdblks[dev]) {
		/* Another CPU made one at the same time as us. */
		mtdblks[dev]->count++;
		spin_unlock(&mtdblks_lock);
		put_mtd_device(mtdblk->mtd);
		kfree(mtdblk);
		return 0;
	}

	mtdblks[dev] = mtdblk;
	mtd_sizes[dev] = mtdblk->mtd->size/1024;
	set_device_ro (inode->i_rdev, !(mtdblk->mtd->flags & MTD_WRITEABLE));
	
	spin_unlock(&mtdblks_lock);
	
	DEBUG(MTD_DEBUG_LEVEL1, "ok\n");

	return 0;
}

static release_t mtdblock_release(struct inode *inode, struct file *file)
{
	int dev;
	struct mtdblk_dev *mtdblk;
   	DEBUG(MTD_DEBUG_LEVEL1, "mtdblock_release\n");

	if (inode == NULL)
		release_return(-ENODEV);

	dev = MINOR(inode->i_rdev);
	mtdblk = mtdblks[dev];

	spin_lock(&mtdblks_lock);
	if (!--mtdblk->count) {
		/* It was the last usage. Free the device */
		mtdblks[dev] = NULL;
		spin_unlock(&mtdblks_lock);
		if (mtdblk->mtd->sync)
			mtdblk->mtd->sync(mtdblk->mtd);
		put_mtd_device(mtdblk->mtd);
		kfree(mtdblk);
	} else {
		spin_unlock(&mtdblks_lock);
	}

	DEBUG(MTD_DEBUG_LEVEL1, "ok\n");

	release_return(0);
}  


/* 
 * This is a special request_fn because it is executed in a process context 
 * to be able to sleep independently of the caller.  The io_request_lock 
 * is held upon entry and exit.
 * The head of our request queue is considered active so there is no need 
 * to dequeue requests before we are done.
 */
static void handle_mtdblock_request(void)
{
	struct request *req;
	struct mtdblk_dev *mtdblk;
	unsigned int res;

	for (;;) {
		INIT_REQUEST;
		req = CURRENT;
		spin_unlock_irq(&io_request_lock);
		mtdblk = mtdblks[MINOR(req->rq_dev)];
		res = 0;

		if (MINOR(req->rq_dev) >= MAX_MTD_DEVICES)
			panic(__FUNCTION__": minor out of bound");

		if ((req->sector + req->current_nr_sectors) > (mtdblk->mtd->size >> 9))
			goto end_req;

		// Handle the request
		switch (req->cmd)
		{
			int err;
			size_t retlen;

			case READ:
			err = MTD_READ (mtdblk->mtd, req->sector << 9, 
					req->current_nr_sectors << 9,
					&retlen, req->buffer);
			if (!err)
				res = 1;
			break;
		}

end_req:
		spin_lock_irq(&io_request_lock);
		end_request(res);
	}
}

static volatile int leaving = 0;
static DECLARE_MUTEX_LOCKED(thread_sem);
static DECLARE_WAIT_QUEUE_HEAD(thr_wq);

int mtdblock_thread(void *dummy)
{
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);

	tsk->session = 1;
	tsk->pgrp = 1;
	/* we might get involved when memory gets low, so use PF_MEMALLOC */
	tsk->flags |= PF_MEMALLOC;
	strcpy(tsk->comm, "mtdblockd");
	tsk->tty = NULL;
	spin_lock_irq(&tsk->sigmask_lock);
	sigfillset(&tsk->blocked);
	recalc_sigpending(tsk);
	spin_unlock_irq(&tsk->sigmask_lock);
	exit_mm(tsk);
	exit_files(tsk);
	exit_sighand(tsk);
	exit_fs(tsk);

	while (!leaving) {
		add_wait_queue(&thr_wq, &wait);
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irq(&io_request_lock);
		if (QUEUE_EMPTY || QUEUE_PLUGGED) {
			spin_unlock_irq(&io_request_lock);
			schedule();
			remove_wait_queue(&thr_wq, &wait); 
		} else {
			remove_wait_queue(&thr_wq, &wait); 
			set_current_state(TASK_RUNNING);
			handle_mtdblock_request();
			spin_unlock_irq(&io_request_lock);
		}
	}

	up(&thread_sem);
	return 0;
}

#if LINUX_VERSION_CODE < 0x20300
#define RQFUNC_ARG void
#else
#define RQFUNC_ARG request_queue_t *q
#endif

static void mtdblock_request(RQFUNC_ARG)
{
	/* Don't do anything, except wake the thread if necessary */
	wake_up(&thr_wq);
}


static int mtdblock_ioctl(struct inode * inode, struct file * file,
		      unsigned int cmd, unsigned long arg)
{
	struct mtdblk_dev *mtdblk;

	mtdblk = mtdblks[MINOR(inode->i_rdev)];

#ifdef PARANOIA
	if (!mtdblk)
		BUG();
#endif

	switch (cmd) {
	case BLKGETSIZE:   /* Return device size */
		return put_user((mtdblk->mtd->size >> 9), (unsigned long *) arg);
	case BLKGETSIZE64:
		return put_user((u64)mtdblk->mtd->size, (u64 *)arg);
		
	case BLKFLSBUF:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,2,0)
		if(!capable(CAP_SYS_ADMIN))
			return -EACCES;
#endif
		fsync_dev(inode->i_rdev);
		invalidate_buffers(inode->i_rdev);
		if (mtdblk->mtd->sync)
			mtdblk->mtd->sync(mtdblk->mtd);
		return 0;

	default:
		return -EINVAL;
	}
}

#if LINUX_VERSION_CODE < 0x20326
static struct file_operations mtd_fops =
{
	open: mtdblock_open,
	ioctl: mtdblock_ioctl,
	release: mtdblock_release,
	read: block_read,
	write: block_write
};
#else
static struct block_device_operations mtd_fops = 
{
	owner: THIS_MODULE,
	open: mtdblock_open,
	release: mtdblock_release,
	ioctl: mtdblock_ioctl
};
#endif

#ifdef CONFIG_DEVFS_FS
/* Notification that a new device has been added. Create the devfs entry for
 * it. */

static void mtd_notify_add(struct mtd_info* mtd)
{
        char name[8];

        if (!mtd || mtd->type == MTD_ABSENT)
                return;

        sprintf(name, "%d", mtd->index);
        devfs_ro_handle[mtd->index] = devfs_register(devfs_dir_handle, name,
                        DEVFS_FL_DEFAULT, MTD_BLOCK_MAJOR, mtd->index,
                        S_IFBLK | S_IRUGO | S_IWUGO,
                        &mtd_fops, NULL);
}

static void mtd_notify_remove(struct mtd_info* mtd)
{
        if (!mtd || mtd->type == MTD_ABSENT)
                return;

        devfs_unregister(devfs_ro_handle[mtd->index]);
}
#endif

int __init init_mtdblock(void)
{
	int i;

	spin_lock_init(&mtdblks_lock);
#ifdef CONFIG_DEVFS_FS
	if (devfs_register_blkdev(MTD_BLOCK_MAJOR, DEVICE_NAME, &mtd_fops))
	{
		printk(KERN_NOTICE "Can't allocate major number %d for Memory Technology Devices.\n",
			MTD_BLOCK_MAJOR);
		return -EAGAIN;
	}

	devfs_dir_handle = devfs_mk_dir(NULL, DEVICE_NAME, NULL);
	register_mtd_user(&notifier);
#else
	if (register_blkdev(MAJOR_NR,DEVICE_NAME,&mtd_fops)) {
		printk(KERN_NOTICE "Can't allocate major number %d for Memory Technology Devices.\n",
		       MTD_BLOCK_MAJOR);
		return -EAGAIN;
	}
#endif
	
	/* We fill it in at open() time. */
	for (i=0; i< MAX_MTD_DEVICES; i++) {
		mtd_sizes[i] = 0;
	}
	init_waitqueue_head(&thr_wq);
	/* Allow the block size to default to BLOCK_SIZE. */
	blksize_size[MAJOR_NR] = NULL;
	blk_size[MAJOR_NR] = mtd_sizes;
	
	blk_init_queue(BLK_DEFAULT_QUEUE(MAJOR_NR), &mtdblock_request);
	kernel_thread (mtdblock_thread, NULL, CLONE_FS|CLONE_FILES|CLONE_SIGHAND);
	return 0;
}

static void __exit cleanup_mtdblock(void)
{
	leaving = 1;
	wake_up(&thr_wq);
	down(&thread_sem);
#ifdef CONFIG_DEVFS_FS
	unregister_mtd_user(&notifier);
	devfs_unregister(devfs_dir_handle);
	devfs_unregister_blkdev(MTD_BLOCK_MAJOR, DEVICE_NAME);
#else
	unregister_blkdev(MAJOR_NR,DEVICE_NAME);
#endif
	blk_cleanup_queue(BLK_DEFAULT_QUEUE(MAJOR_NR));
	blksize_size[MAJOR_NR] = NULL;
	blk_size[MAJOR_NR] = NULL;
}

module_init(init_mtdblock);
module_exit(cleanup_mtdblock);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nicolas Pitre <nico@cam.org> et al.");
MODULE_DESCRIPTION("Caching read/erase/writeback block device emulation access to MTD devices");
