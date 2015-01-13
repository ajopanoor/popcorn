/*
 * Remote processor messaging - client module for ping pong test.
 *
 * Ajo Jose Panoor <ajo.jose.panoor@huawai.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rpmsg.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include "rpmsg_client_ioctl.h"
#include "rpmsg_client.h"

struct rpmsg_perf {
	char *rbuf;
	char *sbuf;
	int rlen;
	int slen;
	int times;
	enum rpmsg_ptest type;
	struct rpmsg_channel *rpdev;
	void (*cb)(struct rpmsg_channel *rpdev, void *data, int len,
			void *priv, unsigned int src);
};

static struct rpmsg_perf grpt;
static struct rpmsg_client_stats gstats;

static void inline __fill_data(char *buf, int len)
{
	memset(buf, 'a', len);
}

static void rpmsg_ping_cleanup(struct rpmsg_perf *rpt)
{
	vfree(rpt->rbuf);
	vfree(rpt->sbuf);
}

void rpmsg_client_cb(struct rpmsg_channel *rpdev, void *data, int len,
						void *priv, u32 src)
{
	s64 t;
	struct rpmsg_perf *rpt = &grpt; // later we should use priv for this.

	LOG_TIME(recv_end_time);

	nrecv++;
	brecv += (len + sizeof(struct rpmsg_hdr));

	UPDATE_ROUND_TRIP_STATS();

	dev_info(&rpdev->dev, "%d bytes from 0x%x seq=%d t= %ld rtt=%ld us\n",
			len, src, nrecv, t, triptime);

	rpt->cb(rpdev, data, len, (void *)rpt, src);
}
//EXPORT_SYMBOL_GPL(rpmsg_client_cb);
static void rpmsg_client_fixed_size_cb(struct rpmsg_channel *rpdev, void *data,
	       					int len, void *priv, u32 src)
{
	int ret;
	struct rpmsg_perf *rpt = priv;

#if 0
	print_hex_dump(KERN_DEBUG, __func__, DUMP_PREFIX_NONE, 16, 1,
		       data, len,  true);
#endif
	if (nrecv >= rpt->times) {
		PRINT_TEST_SUMMARY();
		rpmsg_ping_cleanup(rpt);
		return;
	}

	LOG_TIME(send_start_time);

	__fill_data((char *)(rpt->sbuf + sizeof(struct rpmsg_hdr)),
					(rpt->slen - sizeof(struct rpmsg_hdr)));
	ret = rpmsg_send(rpdev, rpt->sbuf, rpt->slen);
	if (ret)
		dev_err(&rpdev->dev, "rpmsg_send failed: %d\n", ret);

	LOG_TIME(send_end_time);
	nsend++;
	bsend += rpt->slen;
}

static void rpmsg_client_var_size_cb(struct rpmsg_channel *rpdev, void *data,
	       					int len, void *priv, u32 src)
{
	int ret;
	struct rpmsg_perf *rpt = priv;

#if 0
	print_hex_dump(KERN_DEBUG, __func__, DUMP_PREFIX_NONE, 16, 1,
		       data, len,  true);

#endif
	if (nrecv >= rpt->times) {
		PRINT_TEST_SUMMARY();
		rpmsg_ping_cleanup(rpt);
		return;
	}

	LOG_TIME(send_start_time);

	__fill_data((char *)(rpt->sbuf + sizeof(struct rpmsg_hdr)),
					(rpt->slen - sizeof(struct rpmsg_hdr)));
	ret = rpmsg_send_recv(rpdev, rpt->sbuf, rpt->slen, rpt->rbuf, rpt->rlen);
	if (ret)
		dev_err(&rpdev->dev, "rpmsg_send_recv failed: %d\n", ret);

	LOG_TIME(send_end_time);
	nsend++;
	bsend += rpt->slen;
}

void rpmsg_client_ping(struct rpmsg_client_vdev *rvdev,
		 				struct rpmsg_test_args *targs)
{
	int ret = 0;
	struct rpmsg_perf *rpt = &grpt;
	struct rpmsg_channel *rpdev = rvdev->rcdev->rpdev;

	INIT_STATS();

	rpt->slen = targs->sbuf_size;
	rpt->rlen = targs->rbuf_size;
	rpt->type = targs->test_type;
	rpt->times = targs->num_runs;
	rpt->rpdev = rpdev;

	rpt->sbuf = vmalloc(rpt->slen);
	rpt->rbuf = vmalloc(rpt->rlen);

	LOG_TIME(send_start_time);

	switch (rpt->type) {
		case RPMSG_FIXED_SIZE_LATENCY:
			rpt->cb = rpmsg_client_fixed_size_cb;
			ret = rpmsg_send(rpdev, rpt->sbuf, rpt->slen);
			if (ret) {
				dev_err(&rpdev->dev, "rpmsg_send failed: %d\n",
					       	ret);
				return NULL;
			}
			break;
		case RPMSG_VAR_SIZE_LATENCY:
			rpt->cb = rpmsg_client_var_size_cb;
			ret = rpmsg_send_recv(rpdev, rpt->sbuf, rpt->slen,
					 rpt->rbuf, rpt->rlen);
			if (ret) {
				dev_err(&rpdev->dev, "rpmsg_send_recv failed:"
						" %d\n", ret);
				return NULL;
			}
			break;
		case RPMSG_NULL_TEST:
		default:
			dev_err(&rpdev->dev, "unknown rpmsg test type\n");
			return NULL;
	}
	LOG_TIME(send_end_time);
	nsend++;
	bsend += rpt->slen;
	return;
}
//EXPORT_SYMBOL_GPL(rpmsg_client_ping);
