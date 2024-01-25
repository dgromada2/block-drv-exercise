// SPDX-License-Identifier: GPL-2.0+
#include <linux/err.h>
#include <linux/limits.h>
#include "sbdd.h"

#define SBDD_SECTOR_SHIFT      9
#define SBDD_SECTOR_SIZE       (1 << SBDD_SECTOR_SHIFT)
#define SBDD_MIB_SECTORS       (1 << (20 - SBDD_SECTOR_SHIFT))
#define SBDD_NAME	       "sbdd"

static struct sbdd             __sbdd;
static int                     __sbdd_major;
static char                    *__sbdd_mode = SBDD_MODE_MEMORY;
static char                    *__sbdd_backing_devs[SBDD_MAX_BACKING_DEVS];
static unsigned int            __sbdd_nr_backing_devs;

struct sbdd_backstore_type {
	const char *mode;
	int min_devs;
	int max_devs;
	int (*init)(struct sbdd *);
	void (*fini)(void);
};

static struct sbdd_backstore_type __sbdd_backstore_types[] = {
	{
		.mode = SBDD_MODE_MEMORY,
		.min_devs = 0,
		.max_devs = 0,
		.init = sbdd_memory_init,
		.fini = sbdd_memory_fini
	},
	{
		.mode = SBDD_MODE_DISK,
		.min_devs = 1,
		.max_devs = 1,
		.init = sbdd_disk_init
	}
};

/*
 * There are no read or write operations. These operations are performed by
 * the request() function associated with the request queue of the disk.
 */
static struct block_device_operations const __sbdd_bdev_ops = {
	.owner = THIS_MODULE,
};

int sbdd_hold(void)
{
	atomic_inc(&__sbdd.refs_cnt);
	if (atomic_read(&__sbdd.deleting)) {
		pr_err("unable to do block I/O while deleting\n");
		if (atomic_dec_and_test(&__sbdd.refs_cnt))
			wake_up(&__sbdd.exitwait);

		return -1;
	}

	return 0;
}

void sbdd_release(void)
{
	if (atomic_dec_and_test(&__sbdd.refs_cnt))
		wake_up(&__sbdd.exitwait);
}

static struct sbdd_backstore_type *sbdd_bstore_type_lookup(void)
{
	struct sbdd_backstore_type *btype = NULL;

	for (int i = 0; i < ARRAY_SIZE(__sbdd_backstore_types); i++) {
		if (strcmp((char *)__sbdd_backstore_types[i].mode,
			   __sbdd_mode) == 0) {
			btype = &__sbdd_backstore_types[i];
			break;
		}
	}

	return btype;
}

static int sbdd_create(void)
{
	int ret = 0;

	/*
	 * This call is somewhat redundant, but used anyways by tradition.
	 * The number is to be displayed in /proc/devices (0 for auto).
	 */
	pr_info("registering blkdev\n");
	__sbdd_major = register_blkdev(0, SBDD_NAME);
	if (__sbdd_major < 0) {
		pr_err("call register_blkdev() failed with %d\n", __sbdd_major);
		return -EBUSY;
	}

	memset(&__sbdd, 0, sizeof(struct sbdd));

	init_waitqueue_head(&__sbdd.exitwait);

#ifdef BLK_MQ_MODE
	pr_info("allocating tag_set\n");
	__sbdd.tag_set = kzalloc(sizeof(struct blk_mq_tag_set), GFP_KERNEL);
	if (!__sbdd.tag_set) {
		pr_err("unable to alloc tag_set\n");
		return -ENOMEM;
	}

	/* Number of hardware dispatch queues */
	__sbdd.tag_set->nr_hw_queues = 1;
	/* Depth of hardware dispatch queues */
	__sbdd.tag_set->queue_depth = 128;
	__sbdd.tag_set->numa_node = NUMA_NO_NODE;
	__sbdd.tag_set->ops = &__sbdd_blk_mq_ops;

	ret = blk_mq_alloc_tag_set(__sbdd.tag_set);
	if (ret) {
		pr_err("call blk_mq_alloc_tag_set() failed with %d\n", ret);
		return ret;
	}

	/*
	 * Creates both the hardware and the software queues and initializes
	 * structs
	 */
	pr_info("initing queue\n");
	__sbdd.q = blk_mq_init_queue(__sbdd.tag_set);
	if (IS_ERR(__sbdd.q)) {
		ret = (int)PTR_ERR(__sbdd.q);
		pr_err("call blk_mq_init_queue() failed witn %d\n", ret);
		__sbdd.q = NULL;
		return ret;
	}
#else
	pr_info("allocating queue\n");
	__sbdd.q = blk_alloc_queue(GFP_KERNEL);
	if (!__sbdd.q) {
		pr_err("call blk_alloc_queue() failed\n");
		return -EINVAL;
	}
#endif /* BLK_MQ_MODE */
	for (uint i = 0; i < __sbdd_nr_backing_devs; i++) {
		char path[64];
		fmode_t mode = FMODE_READ | FMODE_WRITE;
		struct block_device *bdev;

		snprintf(path, sizeof(path), "/dev/%s", __sbdd_backing_devs[i]);
		pr_info("opening %s\n", path);
		bdev = blkdev_get_by_path(path, mode, &__sbdd);
		if (IS_ERR(bdev)) {
			ret = (int)PTR_ERR(bdev);
			pr_err("blkdev_get_by_path() failed with %d\n", ret);
			return ret;
		}
		__sbdd.backing_devs[i] = bdev;
		__sbdd.nr_backing_devs++;
	}

	/* A disk must have at least one minor */
	pr_info("allocating disk\n");
	__sbdd.gd = alloc_disk(1);

	/* Configure gendisk */
	__sbdd.gd->queue = __sbdd.q;
	__sbdd.gd->major = __sbdd_major;
	__sbdd.gd->first_minor = 0;
	__sbdd.gd->fops = &__sbdd_bdev_ops;
	/* Represents name in /proc/partitions and /sys/block */
	scnprintf(__sbdd.gd->disk_name, DISK_NAME_LEN, SBDD_NAME);

	/*
	 * Allocating gd does not make it available, add_disk() required.
	 * After this call, gd methods can be called at any time. Should not be
	 * called before the driver is fully initialized and ready to process
	 * reqs.
	 */
	pr_info("adding disk\n");
	add_disk(__sbdd.gd);

	return 0;
}

static void sbdd_delete(void)
{
	atomic_set(&__sbdd.deleting, 1);

	wait_event(__sbdd.exitwait, !atomic_read(&__sbdd.refs_cnt));

	/* gd will be removed only after the last reference put */
	if (__sbdd.gd) {
		pr_info("deleting disk\n");
		del_gendisk(__sbdd.gd);
	}

	for (uint i = 0; i < __sbdd.nr_backing_devs; i++)
		blkdev_put(__sbdd.backing_devs[i], FMODE_READ | FMODE_WRITE);

	if (__sbdd.q) {
		pr_info("cleaning up queue\n");
		blk_cleanup_queue(__sbdd.q);
	}

	if (__sbdd.gd)
		put_disk(__sbdd.gd);

#ifdef BLK_MQ_MODE
	if (__sbdd.tag_set && __sbdd.tag_set->tags) {
		pr_info("freeing tag_set\n");
		blk_mq_free_tag_set(__sbdd.tag_set);
	}

	kfree(__sbdd.tag_set);
#endif

	memset(&__sbdd, 0, sizeof(struct sbdd));

	if (__sbdd_major > 0) {
		pr_info("unregistering blkdev\n");
		unregister_blkdev(__sbdd_major, SBDD_NAME);
		__sbdd_major = 0;
	}
}

/*
 * Note __init is for the kernel to drop this function after
 * initialization complete making its memory available for other uses.
 * There is also __initdata note, same but used for variables.
 */
static int __init sbdd_init(void)
{
	int ret = 0;
	struct sbdd_backstore_type *btype;

	pr_info("starting initialization...\n");

	btype = sbdd_bstore_type_lookup();
	if (btype == NULL) {
		pr_err("invalid sbdd mode: %s\n", __sbdd_mode);
		return -EINVAL;
	}

	if ((__sbdd_nr_backing_devs < btype->min_devs) ||
	    (__sbdd_nr_backing_devs > btype->max_devs)) {
		pr_err("number of devices must be in the range (%d, %d)\n",
			   btype->min_devs, btype->max_devs);
		return -EINVAL;
	}

	ret = sbdd_create();
	if (!ret)
		ret = btype->init(&__sbdd);

	if (ret) {
		pr_warn("initialization failed\n");
		sbdd_delete();
	} else {
		pr_info("initialization complete\n");
	}

	return ret;
}

/*
 * Note __exit is for the compiler to place this code in a special ELF section.
 * Sometimes such functions are simply discarded (e.g. when module is built
 * directly into the kernel). There is also __exitdata note.
 */
static void __exit sbdd_exit(void)
{
	struct sbdd_backstore_type *btype;

	pr_info("exiting...\n");
	btype = sbdd_bstore_type_lookup();
	if (btype->fini) {
		btype->fini();
	}
	sbdd_delete();
	pr_info("exiting complete\n");
}

/* Called on module loading. Is mandatory. */
module_init(sbdd_init);

/* Called on module unloading. Unloading module is not allowed without it. */
module_exit(sbdd_exit);

/* Set operating mode */
module_param_named(mode, __sbdd_mode, charp, 0444);
/* Set backing devices */
module_param_array_named(devices, __sbdd_backing_devs, charp,
			 &__sbdd_nr_backing_devs, 0444);

/*
 * Note for the kernel: a free license module.
 * A warning will be outputted without it.
 */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple Block Device Driver");
