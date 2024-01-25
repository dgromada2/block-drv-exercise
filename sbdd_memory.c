// SPDX-License-Identifier: GPL-2.0+
#ifdef BLK_MQ_MODE
#include <linux/blk-mq.h>
#endif
#include "sbdd.h"

#define SBDD_SECTOR_SHIFT      9
#define SBDD_SECTOR_SIZE       (1 << SBDD_SECTOR_SHIFT)
#define SBDD_MIB_SECTORS       (1 << (20 - SBDD_SECTOR_SHIFT))

static unsigned long           __sbdd_capacity_mib = 100;
static spinlock_t              __sbdd_datalock;
static u8                      *__sbdd_data;

static inline sector_t sbdd_capacity(void)
{
	return (sector_t)__sbdd_capacity_mib * SBDD_MIB_SECTORS;
}

static sector_t sbdd_xfer(struct bio_vec *bvec, sector_t pos, int dir)
{
	void *buff = page_address(bvec->bv_page) + bvec->bv_offset;
	sector_t len = bvec->bv_len >> SBDD_SECTOR_SHIFT;
	sector_t capacity = sbdd_capacity();
	size_t offset;
	size_t nbytes;

	if (pos + len > capacity)
		len = capacity - pos;

	offset = pos << SBDD_SECTOR_SHIFT;
	nbytes = len << SBDD_SECTOR_SHIFT;

	spin_lock(&__sbdd_datalock);

	if (dir)
		memcpy(__sbdd_data + offset, buff, nbytes);
	else
		memcpy(buff, __sbdd_data + offset, nbytes);

	spin_unlock(&__sbdd_datalock);

	pr_debug("pos=%6llu len=%4llu %s\n", pos, len,
		 dir ? "written" : "read");

	return len;
}

#ifdef BLK_MQ_MODE

static void sbdd_xfer_rq(struct request *rq)
{
	struct req_iterator iter;
	struct bio_vec bvec;
	int dir = rq_data_dir(rq);
	sector_t pos = blk_rq_pos(rq);

	rq_for_each_segment(bvec, rq, iter)
		pos += sbdd_xfer(&bvec, pos, dir);
}

static blk_status_t sbdd_queue_rq(struct blk_mq_hw_ctx *hctx,
				  struct blk_mq_queue_data const *bd)
{
	if (sbdd_hold() != 0)
		return BLK_STS_IOERR;

	blk_mq_start_request(bd->rq);
	sbdd_xfer_rq(bd->rq);
	blk_mq_end_request(bd->rq, BLK_STS_OK);

	sbdd_release();

	return BLK_STS_OK;
}

static struct blk_mq_ops const __sbdd_blk_mq_ops = {
	/*
	 * The function receives requests for the device as arguments
	 * and can use various functions to process them. The functions
	 * used to process requests in the handler are described below:
	 *
	 * blk_mq_start_request()   - must be called before processing a request
	 * blk_mq_requeue_request() - to re-send the request in the queue
	 * blk_mq_end_request()     - to end request processing and notify upper
	 *                            layers
	 */
	.queue_rq = sbdd_queue_rq,
};

#else

static void sbdd_xfer_bio(struct bio *bio)
{
	struct bvec_iter iter;
	struct bio_vec bvec;
	int dir = bio_data_dir(bio);
	sector_t pos = bio->bi_iter.bi_sector;

	bio_for_each_segment(bvec, bio, iter)
		pos += sbdd_xfer(&bvec, pos, dir);
}

static blk_qc_t sbdd_make_request(struct request_queue *q, struct bio *bio)
{
	if (sbdd_hold() != 0) {
		bio_io_error(bio);
		return BLK_STS_IOERR;
	}

	sbdd_xfer_bio(bio);
	bio_endio(bio);

	sbdd_release();

	return BLK_STS_OK;
}

#endif /* BLK_MQ_MODE */

int sbdd_memory_init(struct sbdd *sbdd)
{
	sector_t capacity = sbdd_capacity();
#ifndef BLK_MQ_MODE
	blk_queue_make_request(sbdd->q, sbdd_make_request);
#endif

	/* Configure queue */
	blk_queue_logical_block_size(sbdd->q, SBDD_SECTOR_SIZE);

	pr_info("allocating data\n");
	__sbdd_data = vzalloc(capacity << SBDD_SECTOR_SHIFT);
	if (!__sbdd_data) {
		pr_err("unable to alloc data\n");
		return -ENOMEM;
	}
	spin_lock_init(&__sbdd_datalock);
	set_capacity(sbdd->gd, capacity);

	return 0;
}

void sbdd_memory_fini(void)
{
	if (__sbdd_data) {
		pr_info("freeing data\n");
		vfree(__sbdd_data);
		__sbdd_data = NULL;
	}
}

/* Set desired capacity with insmod */
module_param_named(capacity_mib, __sbdd_capacity_mib, ulong, 0444);
