/*
 * Copyright (c) 2013-2015 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <stdint.h>
#include <ofi_mem.h>

#include "verbs_ofi.h"


enum ibv_wc_opcode vrb_wr2wc_opcode(enum ibv_wr_opcode wr)
{
	static enum ibv_wc_opcode wc[] = {
		[IBV_WR_RDMA_WRITE] = IBV_WC_RDMA_WRITE,
		[IBV_WR_RDMA_WRITE_WITH_IMM] = IBV_WC_RDMA_WRITE,
		[IBV_WR_SEND] = IBV_WC_SEND,
		[IBV_WR_SEND_WITH_IMM] = IBV_WC_SEND,
		[IBV_WR_RDMA_READ] = IBV_WC_RDMA_READ,
		[IBV_WR_ATOMIC_CMP_AND_SWP] = IBV_WC_COMP_SWAP,
		[IBV_WR_ATOMIC_FETCH_AND_ADD] = IBV_WC_FETCH_ADD,
	};

	return (wr < ARRAY_SIZE(wc)) ? wc[wr] : IBV_WC_SEND;
}

static void vrb_cq_read_context_entry(struct ibv_wc *wc, void *buf)
{
	struct fi_cq_entry *entry = buf;

	entry->op_context = (void *) (uintptr_t) wc->wr_id;
}

static void vrb_cq_read_msg_entry(struct ibv_wc *wc, void *buf)
{
	struct fi_cq_msg_entry *entry = buf;

	entry->op_context = (void *) (uintptr_t) wc->wr_id;

	switch (wc->opcode) {
	case IBV_WC_SEND:
		entry->flags = (FI_SEND | FI_MSG);
		break;
	case IBV_WC_RDMA_WRITE:
		entry->flags = (FI_RMA | FI_WRITE);
		break;
	case IBV_WC_RDMA_READ:
		entry->flags = (FI_RMA | FI_READ);
		break;
	case IBV_WC_COMP_SWAP:
		entry->flags = FI_ATOMIC;
		break;
	case IBV_WC_FETCH_ADD:
		entry->flags = FI_ATOMIC;
		break;
	case IBV_WC_RECV:
		entry->len = wc->byte_len;
		entry->flags = (FI_RECV | FI_MSG);
		break;
	case IBV_WC_RECV_RDMA_WITH_IMM:
		entry->len = wc->byte_len;
		entry->flags = (FI_RMA | FI_REMOTE_WRITE);
		break;
	default:
		break;
	}
}

static void vrb_cq_read_data_entry(struct ibv_wc *wc, void *buf)
{
	struct fi_cq_data_entry *entry = buf;

	/* fi_cq_data_entry can cast to fi_cq_msg_entry */
	vrb_cq_read_msg_entry(wc, buf);
	if ((wc->wc_flags & IBV_WC_WITH_IMM) &&
	    (wc->opcode & IBV_WC_RECV)) {
		entry->data = ntohl(wc->imm_data);
		entry->flags |= FI_REMOTE_CQ_DATA;
	}
}

static ssize_t
vrb_cq_readerr(struct fid_cq *cq_fid, struct fi_cq_err_entry *entry,
		  uint64_t flags)
{
	struct vrb_cq *cq;
	struct vrb_wc_entry *wce;
	struct slist_entry *slist_entry;
	uint32_t api_version;

	cq = container_of(cq_fid, struct vrb_cq, util_cq.cq_fid);

	ofi_genlock_lock(vrb_cq2_progress(cq)->active_lock);
	if (slist_empty(&cq->saved_wc_list))
		goto err;

	wce = container_of(cq->saved_wc_list.head, struct vrb_wc_entry, entry);
	if (!wce->wc.status)
		goto err;

	api_version = cq->util_cq.domain->fabric->fabric_fid.api_version;

	slist_entry = slist_remove_head(&cq->saved_wc_list);
	ofi_genlock_unlock(vrb_cq2_progress(cq)->active_lock);

	wce = container_of(slist_entry, struct vrb_wc_entry, entry);

	entry->op_context = (void *)(uintptr_t)wce->wc.wr_id;
	entry->prov_errno = wce->wc.status;
	if (wce->wc.status == IBV_WC_WR_FLUSH_ERR)
		entry->err = FI_ECANCELED;
	else
		entry->err = EIO;

	/* fi_cq_err_entry can cast to fi_cq_data_entry */
	vrb_cq_read_data_entry(&wce->wc, (void *) entry);

	if ((FI_VERSION_GE(api_version, FI_VERSION(1, 5))) &&
		entry->err_data && entry->err_data_size) {
		entry->err_data_size = MIN(entry->err_data_size,
			sizeof(wce->wc.vendor_err));
		memcpy(entry->err_data, &wce->wc.vendor_err, entry->err_data_size);
	} else {
		memcpy(&entry->err_data, &wce->wc.vendor_err,
			sizeof(wce->wc.vendor_err));
	}

	ofi_buf_free(wce);
	return 1;
err:
	ofi_genlock_unlock(vrb_cq2_progress(cq)->active_lock);
	return -FI_EAGAIN;
}

static inline int
vrb_poll_events(struct vrb_cq *_cq, int timeout)
{
	int ret, rc;
	void *context;
	struct pollfd fds[2];

	fds[0].fd = _cq->channel->fd;
	fds[1].fd = fd_signal_get(&_cq->signal);

	fds[0].events = fds[1].events = POLLIN;

	rc = poll(fds, 2, timeout);
	if (rc == 0)
		return -FI_EAGAIN;
	else if (rc < 0)
		return -errno;

	if (fds[0].revents & POLLIN) {
		ret = ibv_get_cq_event(_cq->channel, &_cq->cq, &context);
		if (ret)
			return ret;

		ofi_atomic_inc32(&_cq->nevents);
		rc--;
	}
	if (fds[1].revents & POLLIN) {
		fd_signal_reset(&_cq->signal);
		ret = -FI_EAGAIN;
		rc--;
	}
	if (rc) {
		VRB_WARN(FI_LOG_CQ, "Unknown poll error: check revents\n");
		return -FI_EOTHER;
	}

	return ret;
}

static ssize_t
vrb_cq_sread(struct fid_cq *cq, void *buf, size_t count, const void *cond,
		int timeout)
{
	ssize_t ret = 0, cur;
	ssize_t  threshold;
	struct vrb_cq *_cq;
	uint8_t *p;

	p = buf;
	_cq = container_of(cq, struct vrb_cq, util_cq.cq_fid);

	if (!_cq->channel)
		return -FI_ENOSYS;

	threshold = (_cq->wait_cond == FI_CQ_COND_THRESHOLD) ?
		MIN((ssize_t) cond, count) : 1;

	for (cur = 0; cur < threshold; ) {
		if (vrb_cq_trywait(_cq) == FI_SUCCESS) {
			ret = vrb_poll_events(_cq, timeout);
			if (ret)
				break;
		}

		ret = _cq->util_cq.cq_fid.ops->read(&_cq->util_cq.cq_fid, p, count - cur);
		if (ret > 0) {
			p += ret * _cq->entry_size;
			cur += ret;
			if (cur >= threshold)
				break;
		} else if (ret != -FI_EAGAIN) {
			break;
		}
	}

	return cur ? cur : ret;
}

int vrb_poll_cq(struct vrb_cq *cq, struct ibv_wc *wc)
{
	struct vrb_context *ctx;
	struct vrb_ep *ep;
	int ret;

	assert(ofi_genlock_held(vrb_cq2_progress(cq)->active_lock));
	do {
		ret = ibv_poll_cq(cq->cq, 1, wc);
		if (ret <= 0)
			break;

		ctx = (struct vrb_context *) (uintptr_t) wc->wr_id;
		wc->wr_id = (uintptr_t) ctx->user_ctx;
		if (wc->status != IBV_WC_SUCCESS)
			vrb_shutdown_qp_in_err(ctx->ep);
		if (ctx->op_queue == VRB_OP_SQ) {
			ep = ctx->ep;
			assert(ep);
			assert(!slist_empty(&ep->sq_list));
			assert(ep->sq_list.head == &ctx->entry);
			(void) slist_remove_head(&ep->sq_list);
			ep->sq_credits++;

			/* workaround incorrect opcode reported by verbs */
			wc->opcode = vrb_wr2wc_opcode(ctx->sq_opcode);

		} else if (ctx->op_queue == VRB_OP_RQ) {
			ep = ctx->ep;
			assert(ep);
			assert(!slist_empty(&ep->rq_list));
			assert(ep->rq_list.head == &ctx->entry);
			(void) slist_remove_head(&ep->rq_list);
			if (wc->status)
				wc->opcode = IBV_WC_RECV;
		} else {
			assert(ctx->op_queue == VRB_OP_SRQ);
			wc->opcode = IBV_WC_RECV;
		}

		vrb_free_ctx(vrb_cq2_progress(cq), ctx);
	} while (wc->wr_id == VERBS_NO_COMP_FLAG);

	return ret;
}

int vrb_save_wc(struct vrb_cq *cq, struct ibv_wc *wc)
{
	struct vrb_wc_entry *wce;

	assert(ofi_genlock_held(vrb_cq2_progress(cq)->active_lock));
	wce = ofi_buf_alloc(cq->wce_pool);
	if (!wce) {
		FI_WARN(&vrb_prov, FI_LOG_CQ,
			"Unable to save completion, completion lost!\n");
		return -FI_ENOMEM;
	}

	wce->wc = *wc;
	slist_insert_tail(&wce->entry, &cq->saved_wc_list);
	return FI_SUCCESS;
}

static void vrb_flush_cq(struct vrb_cq *cq)
{
	struct ibv_wc wc;
	ssize_t ret;

	assert(ofi_genlock_held(vrb_cq2_progress(cq)->active_lock));
	while (1) {
		ret = vrb_poll_cq(cq, &wc);
		if (ret <= 0)
			break;

		vrb_save_wc(cq, &wc);
	};
}

void vrb_cleanup_cq(struct vrb_ep *ep)
{
	assert(ofi_genlock_held(vrb_ep2_progress(ep)->active_lock));
	if (ep->util_ep.rx_cq) {
		vrb_flush_cq(container_of(ep->util_ep.rx_cq,
					  struct vrb_cq, util_cq));
	}
	if (ep->util_ep.tx_cq) {
		vrb_flush_cq(container_of(ep->util_ep.tx_cq,
					  struct vrb_cq, util_cq));
	}
}

static ssize_t vrb_cq_read(struct fid_cq *cq_fid, void *buf, size_t count)
{
	struct vrb_cq *cq;
	struct vrb_wc_entry *wce;
	struct slist_entry *entry;
	struct ibv_wc wc;
	ssize_t ret = 0, i;

	cq = container_of(cq_fid, struct vrb_cq, util_cq.cq_fid);

	ofi_genlock_lock(vrb_cq2_progress(cq)->active_lock);
	for (i = 0; i < count; i++) {
		if (!slist_empty(&cq->saved_wc_list)) {
			wce = container_of(cq->saved_wc_list.head,
					   struct vrb_wc_entry, entry);
			if (wce->wc.status) {
				ret = -FI_EAVAIL;
				break;
			}
			entry = slist_remove_head(&cq->saved_wc_list);
			wce = container_of(entry, struct vrb_wc_entry, entry);
			cq->read_entry(&wce->wc, (char *) buf + i * cq->entry_size);
			ofi_buf_free(wce);
			continue;
		}

		ret = vrb_poll_cq(cq, &wc);
		if (ret <= 0)
			break;

		if (wc.status) {
			wce = ofi_buf_alloc(cq->wce_pool);
			if (!wce) {
				ofi_genlock_unlock(vrb_cq2_progress(cq)->active_lock);
				return -FI_ENOMEM;
			}
			memset(wce, 0, sizeof(*wce));
			memcpy(&wce->wc, &wc, sizeof wc);
			slist_insert_tail(&wce->entry, &cq->saved_wc_list);
			ret = -FI_EAVAIL;
			break;
		}

		cq->read_entry(&wc, (char *)buf + i * cq->entry_size);
	}

	ofi_genlock_unlock(vrb_cq2_progress(cq)->active_lock);
	return i ? i : (ret < 0 ? ret : -FI_EAGAIN);
}

static const char *
vrb_cq_strerror(struct fid_cq *eq, int prov_errno, const void *err_data,
		   char *buf, size_t len)
{
	if (buf && len)
		strncpy(buf, ibv_wc_status_str(prov_errno), len);
	return ibv_wc_status_str(prov_errno);
}

int vrb_cq_signal(struct fid_cq *cq)
{
	struct vrb_cq *_cq;

	_cq = container_of(cq, struct vrb_cq, util_cq.cq_fid);

	fd_signal_set(&_cq->signal);

	return 0;
}

int vrb_cq_trywait(struct vrb_cq *cq)
{
	struct ibv_wc wc;
	void *context;
	int ret = -FI_EAGAIN, rc;

	if (!cq->channel) {
		VRB_WARN(FI_LOG_CQ, "No wait object object associated with CQ\n");
		return -FI_EINVAL;
	}

	ofi_genlock_lock(vrb_cq2_progress(cq)->active_lock);
	if (!slist_empty(&cq->saved_wc_list))
		goto out;

	rc = vrb_poll_cq(cq, &wc);
	if (rc) {
		if (rc > 0)
			vrb_save_wc(cq, &wc);
		goto out;
	}

	while (!ibv_get_cq_event(cq->channel, &cq->cq, &context))
		ofi_atomic_inc32(&cq->nevents);

	rc = ibv_req_notify_cq(cq->cq, 0);
	if (rc) {
		VRB_WARN(FI_LOG_CQ, "ibv_req_notify_cq error: %d\n", ret);
		ret = -errno;
		goto out;
	}

	/* Read again to fetch any completions that we might have missed
	 * while rearming */
	rc = vrb_poll_cq(cq, &wc);
	if (rc) {
		if (rc > 0)
			vrb_save_wc(cq, &wc);
		goto out;
	}

	ret = FI_SUCCESS;
out:
	ofi_genlock_unlock(vrb_cq2_progress(cq)->active_lock);
	return ret;
}

static struct fi_ops_cq vrb_cq_ops = {
	.size = sizeof(struct fi_ops_cq),
	.read = vrb_cq_read,
	.readfrom = fi_no_cq_readfrom,
	.readerr = vrb_cq_readerr,
	.sread = vrb_cq_sread,
	.sreadfrom = fi_no_cq_sreadfrom,
	.signal = vrb_cq_signal,
	.strerror = vrb_cq_strerror
};

static int vrb_cq_control(fid_t fid, int command, void *arg)
{
	struct fi_wait_pollfd *pollfd;
	struct vrb_cq *cq;
	int ret;

	cq = container_of(fid, struct vrb_cq, util_cq.cq_fid);
	switch(command) {
	case FI_GETWAIT:
		if (!cq->channel) {
			ret = -FI_ENODATA;
			break;
		}

		if (cq->wait_obj == FI_WAIT_FD) {
			*(int *) arg = cq->channel->fd;
			return 0;
		}

		pollfd = arg;
		if (pollfd->nfds >= 1) {
			pollfd->fd[0].fd = cq->channel->fd;
			pollfd->fd[0].events = POLLIN;
			ret = 0;
		} else {
			ret = -FI_ETOOSMALL;
		}
		pollfd->nfds = 1;
		break;
	case FI_GETWAITOBJ:
		*(enum fi_wait_obj *) arg = cq->wait_obj;
		ret = 0;
		break;
	default:
		ret = -FI_ENOSYS;
		break;
	}

	return ret;
}

static int vrb_cq_close(fid_t fid)
{
	struct vrb_wc_entry *wce;
	struct slist_entry *entry;
	struct vrb_cq *cq = container_of(fid, struct vrb_cq, util_cq.cq_fid);
	struct vrb_srx *srx;
	struct dlist_entry *srx_temp;
	int ret;

	if (ofi_atomic_get32(&cq->nevents))
		ibv_ack_cq_events(cq->cq, ofi_atomic_get32(&cq->nevents));

	/* Since an RX CQ and SRX context can be destroyed in any order,
	 * and the XRC SRQ references the RX CQ, we must destroy any
	 * XRC SRQ using this CQ before destroying the CQ. */
	ofi_genlock_lock(vrb_cq2_progress(cq)->active_lock);
	dlist_foreach_container_safe(&cq->xrc.srq_list, struct vrb_srx,
				     srx, xrc.srq_entry, srx_temp) {
		ret = vrb_xrc_close_srq(srx);
		if (ret) {
			ofi_genlock_unlock(vrb_cq2_progress(cq)->active_lock);
			return -ret;
		}
	}

	while (!slist_empty(&cq->saved_wc_list)) {
		entry = slist_remove_head(&cq->saved_wc_list);
		wce = container_of(entry, struct vrb_wc_entry, entry);
		ofi_buf_free(wce);
	}
	ofi_genlock_unlock(vrb_cq2_progress(cq)->active_lock);

	ofi_bufpool_destroy(cq->wce_pool);

	if (cq->cq) {
		ret = ibv_destroy_cq(cq->cq);
		if (ret)
			return -ret;
	}

	if (cq->wait_obj != FI_WAIT_NONE)
		fd_signal_free(&cq->signal);

	ofi_cq_cleanup(&cq->util_cq);

	if (cq->channel)
		ibv_destroy_comp_channel(cq->channel);

	free(cq);
	return 0;
}

static struct fi_ops vrb_cq_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = vrb_cq_close,
	.bind = fi_no_bind,
	.control = vrb_cq_control,
	.ops_open = fi_no_ops_open,
};

static void vrb_util_cq_progress_noop(struct util_cq *cq)
{
	/* This routine shouldn't be called */
	assert(0);
}

int vrb_cq_open(struct fid_domain *domain_fid, struct fi_cq_attr *attr,
		   struct fid_cq **cq_fid, void *context)
{
	struct vrb_cq *cq;
	struct vrb_domain *domain =
		container_of(domain_fid, struct vrb_domain,
			     util_domain.domain_fid);
	size_t size;
	int ret;
	struct fi_cq_attr tmp_attr = *attr;
	int comp_vector = 0;

	cq = calloc(1, sizeof(*cq));
	if (!cq)
		return -FI_ENOMEM;

	/* verbs uses its own implementation of wait objects for CQ */
	tmp_attr.wait_obj = FI_WAIT_NONE;
	ret = ofi_cq_init(&vrb_prov, domain_fid, &tmp_attr, &cq->util_cq,
			  vrb_util_cq_progress_noop, context);
	if (ret)
		goto err1;

	switch (attr->wait_obj) {
	case FI_WAIT_UNSPEC:
		cq->wait_obj = FI_WAIT_FD;
		break;
	case FI_WAIT_FD:
	case FI_WAIT_POLLFD:
	case FI_WAIT_NONE:
		cq->wait_obj = attr->wait_obj;
		break;
	default:
		ret = -FI_ENOSYS;
		goto err2;
	}

	if (attr->flags & FI_AFFINITY) {
		if (attr->signaling_vector < 0 ||
		    attr->signaling_vector > domain->verbs->num_comp_vectors)  {

			VRB_WARN(FI_LOG_CQ,
				   "Invalid value for the CQ attribute signaling_vector: %d\n",
				   attr->signaling_vector);
			ret = -FI_EINVAL;
			goto err2;
		}
		comp_vector = attr->signaling_vector;
	}

	if (cq->wait_obj != FI_WAIT_NONE) {
		cq->channel = ibv_create_comp_channel(domain->verbs);
		if (!cq->channel) {
			ret = -errno;
			VRB_WARN(FI_LOG_CQ,
				   "Unable to create completion channel\n");
			goto err2;
		}

		ret = fi_fd_nonblock(cq->channel->fd);
		if (ret)
			goto err3;

		if (fd_signal_init(&cq->signal)) {
			ret = -errno;
			goto err3;
		}
	}

	size = attr->size ? attr->size : VERBS_DEF_CQ_SIZE;

	/*
	 * Verbs may throw an error if CQ size exceeds ibv_device_attr->max_cqe.
	 * OFI doesn't expose CQ size to the apps because it's better to fix the
	 * issue in the provider than the app dealing with it. The fix is to
	 * open multiple verbs CQs and load balance "MSG EP to CQ binding"* among
	 * them to avoid any CQ overflow.
	 * Something like:
	 * num_qp_per_cq = ibv_device_attr->max_cqe / (qp_send_wr + qp_recv_wr)
	 */
	cq->cq = ibv_create_cq(domain->verbs, size, cq, cq->channel,
			       comp_vector);
	if (!cq->cq) {
		ret = -errno;
		VRB_WARN(FI_LOG_CQ, "Unable to create verbs CQ\n");
		goto err3;
	}

	if (cq->channel) {
		ret = ibv_req_notify_cq(cq->cq, 0);
		if (ret) {
			VRB_WARN(FI_LOG_CQ,
				   "ibv_req_notify_cq failed\n");
			goto err4;
		}
	}

	ret = ofi_bufpool_create(&cq->wce_pool, sizeof(struct vrb_wc_entry),
				16, 0, VERBS_WCE_CNT, 0);
	if (ret) {
		VRB_WARN(FI_LOG_CQ, "Failed to create wce_pool\n");
		goto err4;
	}

	cq->flags |= attr->flags;
	cq->wait_cond = attr->wait_cond;
	/* verbs uses its own ops for CQ */
	cq->util_cq.cq_fid.fid.ops = &vrb_cq_fi_ops;
	cq->util_cq.cq_fid.ops = &vrb_cq_ops;

	switch (attr->format) {
	case FI_CQ_FORMAT_UNSPEC:
	case FI_CQ_FORMAT_CONTEXT:
		cq->read_entry = vrb_cq_read_context_entry;
		cq->entry_size = sizeof(struct fi_cq_entry);
		break;
	case FI_CQ_FORMAT_MSG:
		cq->read_entry = vrb_cq_read_msg_entry;
		cq->entry_size = sizeof(struct fi_cq_msg_entry);
		break;
	case FI_CQ_FORMAT_DATA:
		cq->read_entry = vrb_cq_read_data_entry;
		cq->entry_size = sizeof(struct fi_cq_data_entry);
		break;
	case FI_CQ_FORMAT_TAGGED:
	default:
		ret = -FI_ENOSYS;
		goto err5;
	}

	slist_init(&cq->saved_wc_list);
	dlist_init(&cq->xrc.srq_list);

	ofi_atomic_initialize32(&cq->nevents, 0);

	*cq_fid = &cq->util_cq.cq_fid;
	return 0;

err5:
	ofi_bufpool_destroy(cq->wce_pool);
err4:
	ibv_destroy_cq(cq->cq);
err3:
	if (cq->channel)
		ibv_destroy_comp_channel(cq->channel);
err2:
	ofi_cq_cleanup(&cq->util_cq);
err1:
	free(cq);
	return ret;
}


int vrb_init_progress(struct vrb_progress *progress, struct ibv_context *verbs)
{
	int ret;

	ret = ofi_genlock_init(&progress->lock, OFI_LOCK_MUTEX);
	if (ret)
		return ret;

	/* Active lock will be needed when adding rdm ep support */
	progress->active_lock = &progress->lock;

	ret = ofi_bufpool_create(&progress->ctx_pool, sizeof(struct fi_context),
				 16, 0, 1024, OFI_BUFPOOL_NO_TRACK);
	if (ret)
		goto err1;

	return 0;

err1:
	ofi_genlock_destroy(&progress->lock);
	return ret;
}

void vrb_close_progress(struct vrb_progress *progress)
{
	ofi_bufpool_destroy(progress->ctx_pool);
	ofi_genlock_destroy(&progress->lock);
}
