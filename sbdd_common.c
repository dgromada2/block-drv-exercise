// SPDX-License-Identifier: GPL-2.0+
#include "sbdd.h"

static void sbdd_proxy_bio_done(struct bio *bio)
{
	struct bio *orig_bio = (struct bio *)bio->bi_private;

	orig_bio->bi_status = bio->bi_status;
	bio_endio(orig_bio);
	sbdd_release();
}

void sbdd_backdev_setup_lbsize(struct sbdd *sbdd)
{
	unsigned short lbs = 0;
	unsigned short tmp;

	for (uint i = 0; i < sbdd->nr_backing_devs; i++) {
		tmp = bdev_logical_block_size(sbdd->backing_devs[i]);
		if (tmp > lbs)
			lbs = tmp;
	}

	blk_queue_logical_block_size(sbdd->q, lbs);
}

sector_t sbdd_backdev_min_capacity(struct sbdd *sbdd)
{
	sector_t ret = INT_MAX;
	sector_t tmp;

	for (uint i = 0; i < sbdd->nr_backing_devs; i++) {
		tmp = get_capacity(sbdd->backing_devs[i]->bd_disk);
		if (tmp < ret)
			ret = tmp;
	}

	return ret;
}

void sbdd_submit_proxy_bio(struct bio *bio, struct bio *parent)
{
	bio->bi_private = parent;
	bio->bi_end_io = sbdd_proxy_bio_done;
	submit_bio(bio);
}
