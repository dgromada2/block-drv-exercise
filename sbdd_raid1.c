// SPDX-License-Identifier: GPL-2.0+
#include "sbdd.h"

static struct sbdd              *__sbdd;
static struct gendisk           *__sbdd_prev_disk;

#ifdef BLK_MQ_MODE

int sbdd_raid1_init(struct sbdd *sbdd)
{
	pr_err("raid1 mode is not supported along with multiqueue\n")
	return -ENOTSUPP;
}

#else   /* BLK_MQ_MODE */

static struct block_device *sbdd_raid1_choose_bdev(struct gendisk *prev)
{
	return (prev != __sbdd->backing_devs[0]->bd_disk) ?
		__sbdd->backing_devs[0] : __sbdd->backing_devs[1];
}

static void sbdd_raid1_in_bio_done(struct bio *bio)
{
	/*
	 * Try to read from the second block device if the operation has
	 * failed for the first one.
	 */
	struct bio *parent = bio->bi_private;
	struct bio *bs_bio;
	struct block_device *mir_bd;

	mir_bd = sbdd_raid1_choose_bdev(bio->bi_disk);

	if (bio->bi_status == BLK_STS_OK) {
		parent->bi_status = BLK_STS_OK;
		goto out;
	}

	bs_bio = bio_clone_fast(parent, GFP_KERNEL, NULL);
	if (bs_bio == NULL) {
		pr_err("unable to clone bio trying the other block device\n");
		parent->bi_status = BLK_STS_IOERR;
		goto out;
	}

	bio_set_dev(bs_bio, mir_bd);
	sbdd_submit_proxy_bio(bs_bio, parent);

out:
	bio_endio(parent);
	sbdd_release();
}

static blk_qc_t sbdd_raid1_make_request(struct request_queue *q,
					struct bio *bio)
{
	/* chained bio */
	struct bio *ch_bio = NULL;
	/* mirroring bio */
	struct bio *mir_bio = NULL;
	int dir = bio_data_dir(bio);
	struct block_device *bd;

	if (sbdd_hold() != 0) {
		bio_io_error(bio);
		return BLK_STS_IOERR;
	}

	ch_bio = bio_clone_fast(bio, GFP_KERNEL, NULL);
	if (ch_bio == NULL) {
		pr_err("unable to clone bio\n");
		goto out;
	}

	bd = sbdd_raid1_choose_bdev(__sbdd_prev_disk);
	bio_set_dev(ch_bio, bd);

	if (dir) {
		mir_bio = bio_clone_fast(bio, GFP_KERNEL, NULL);
		if (mir_bio == NULL) {
			pr_err("unable to clone bio\n");
			goto out_bio;
		}
		bd = sbdd_raid1_choose_bdev(bd->bd_disk);
		bio_set_dev(mir_bio, bd);

		bio_chain(mir_bio, ch_bio);
		sbdd_submit_proxy_bio(ch_bio, bio);
		submit_bio(mir_bio);
	} else {
		ch_bio->bi_private = bio;
		ch_bio->bi_end_io = sbdd_raid1_in_bio_done;
		submit_bio(ch_bio);
	}

	__sbdd_prev_disk = bd->bd_disk;

	return 0;

out_bio:
	bio_put(ch_bio);
out:
	bio_io_error(bio);
	sbdd_release();

	return BLK_STS_IOERR;
}

int sbdd_raid1_init(struct sbdd *sbdd)
{
	sector_t capacity;

	blk_queue_make_request(sbdd->q, sbdd_raid1_make_request);

	sbdd_backdev_setup_lbsize(sbdd);
	capacity = sbdd_backdev_min_capacity(sbdd);
	set_capacity(sbdd->gd, capacity);

	__sbdd = sbdd;

	return 0;
}

#endif  /* !BLK_MQ_MODE */
