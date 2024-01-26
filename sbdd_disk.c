// SPDX-License-Identifier: GPL-2.0+
#include "sbdd.h"

struct sbdd 	*__sbdd;

#ifdef BLK_MQ_MODE

int sbdd_disk_init(struct sbdd *sbdd)
{
	pr_err("disk mode is not supported along with multiqueue\n")
	return -ENOTUPP;
}

#else   /* BLK_MQ_MODE */

static blk_qc_t sbdd_disk_make_request(struct request_queue *q,
				       struct bio *bio)
{
	struct bio *bs_bio;

	if (sbdd_hold() != 0) {
		bio_io_error(bio);
		return BLK_STS_IOERR;
	}

	bs_bio = bio_clone_fast(bio, GFP_KERNEL, NULL);
	if (bs_bio == NULL) {
		pr_err("unable to clone bio\n");
		bio_io_error(bio);
		sbdd_release();
		return BLK_STS_IOERR;
	}

	bio_set_dev(bs_bio, __sbdd->backing_devs[0]);
	sbdd_submit_proxy_bio(bs_bio, bio);

	return 0;
}

int sbdd_disk_init(struct sbdd *sbdd)
{
	sector_t capacity;

	blk_queue_make_request(sbdd->q, sbdd_disk_make_request);

	sbdd_backdev_setup_lbsize(sbdd);
	capacity = sbdd_backdev_min_capacity(sbdd);
	set_capacity(sbdd->gd, capacity);

	__sbdd = sbdd;

	return 0;
}

#endif  /* !BLK_MQ_MODE */
