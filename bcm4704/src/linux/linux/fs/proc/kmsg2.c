/*
 *  linux/fs/proc/kmsg2.c
 *
 *  Copyright (C) 2008
 *
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/poll.h>

#include <asm/uaccess.h>
#include <asm/io.h>

extern wait_queue_head_t log_wait2;

extern int do_syslog2(int type, char * bug, int count);

static int kmsg_open(struct inode * inode, struct file * file)
{
	return do_syslog2(1,NULL,0);
}

static int kmsg_release(struct inode * inode, struct file * file)
{
	(void) do_syslog2(0,NULL,0);
	return 0;
}

static ssize_t kmsg_read(struct file * file, char * buf,
			 size_t count, loff_t *ppos)
{
	return do_syslog2(2,buf,count);
}

static unsigned int kmsg_poll(struct file *file, poll_table * wait)
{
	poll_wait(file, &log_wait2, wait);
	if (do_syslog2(9, 0, 0))
		return POLLIN | POLLRDNORM;
	return 0;
}


struct file_operations proc_kmsg2_operations = {
	read:		kmsg_read,
	poll:		kmsg_poll,
	open:		kmsg_open,
	release:	kmsg_release,
};
