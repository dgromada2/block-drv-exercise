/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _SBDD_H
#define _SBDD_H

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/bio.h>
#include <linux/bvec.h>
#include <linux/init.h>
#include <linux/wait.h>
#include <linux/stat.h>
#include <linux/slab.h>
#include <linux/numa.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/moduleparam.h>
#include <linux/spinlock_types.h>

#define SBDD_MODE_MEMORY        "memory"
#define SBDD_MODE_DISK          "disk"
#define SBDD_MODE_RAID0         "raid0"
#define SBDD_MODE_RAID1         "raid1"

#define SBDD_MAX_BACKING_DEVS   8

struct sbdd {
	wait_queue_head_t       exitwait;
	atomic_t                deleting;
	atomic_t                refs_cnt;
	struct gendisk          *gd;
	struct request_queue    *q;
	struct block_device     *backing_devs[SBDD_MAX_BACKING_DEVS];
	unsigned int            nr_backing_devs;
#ifdef BLK_MQ_MODE
	struct blk_mq_tag_set   *tag_set;
#endif
};

int sbdd_hold(void);
void sbdd_release(void);

void sbdd_backdev_setup_lbsize(struct sbdd *sbdd);
sector_t sbdd_backdev_min_capacity(struct sbdd *sbdd);
void sbdd_submit_proxy_bio(struct bio *bio, struct bio *parent);

int sbdd_memory_init(struct sbdd *sbdd);
void sbdd_memory_fini(void);

int sbdd_disk_init(struct sbdd *sbdd);

#endif /* _SBDD_H */
