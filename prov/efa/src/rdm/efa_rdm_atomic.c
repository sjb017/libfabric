/*
 * Copyright (c) 2019-2020 Amazon.com, Inc. or its affiliates.
 * All rights reserved.
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

#include <ofi_iov.h>
#include <ofi_atomic.h>
#include "efa.h"
#include "efa_rdm_rma.h"
#include "efa_cntr.h"
#include "efa_rdm_atomic.h"
#include "rxr_pkt_cmd.h"

static void efa_rdm_atomic_init_shm_msg(struct efa_rdm_ep *ep, struct fi_msg_atomic *shm_msg,
				    const struct fi_msg_atomic *msg,
				    struct fi_rma_ioc *rma_iov,
				    void **shm_desc)
{
	int i;

	assert(msg->rma_iov_count <= RXR_IOV_LIMIT);
	memcpy(shm_msg, msg, sizeof(*msg));
	if (!(efa_rdm_ep_domain(ep)->shm_info->domain_attr->mr_mode & FI_MR_VIRT_ADDR)) {
		memcpy(rma_iov, msg->rma_iov,
		       sizeof(*msg->rma_iov) * msg->rma_iov_count);
		for (i = 0; i < msg->rma_iov_count; i++)
			rma_iov[i].addr = 0;
		shm_msg->rma_iov = rma_iov;
	}

	if (msg->desc) {
		efa_rdm_get_desc_for_shm(msg->iov_count, msg->desc, shm_desc);
		shm_msg->desc = shm_desc;
	} else {
		shm_msg->desc = NULL;
	}
}

static
struct efa_rdm_ope *
efa_rdm_atomic_alloc_txe(struct efa_rdm_ep *efa_rdm_ep,
			  const struct fi_msg_atomic *msg_atomic,
			  const struct efa_rdm_atomic_ex *atomic_ex,
			  uint32_t op, uint64_t flags)
{
	struct efa_rdm_ope *txe;
	struct fi_msg msg;
	struct iovec iov[RXR_IOV_LIMIT];
	size_t datatype_size;

	datatype_size = ofi_datatype_size(msg_atomic->datatype);
	if (OFI_UNLIKELY(!datatype_size)) {
		return NULL;
	}

	txe = ofi_buf_alloc(efa_rdm_ep->ope_pool);
	if (OFI_UNLIKELY(!txe)) {
		EFA_DBG(FI_LOG_EP_CTRL, "TX entries exhausted.\n");
		return NULL;
	}

	dlist_insert_tail(&txe->ep_entry, &efa_rdm_ep->txe_list);

	ofi_ioc_to_iov(msg_atomic->msg_iov, iov, msg_atomic->iov_count, datatype_size);
	msg.addr = msg_atomic->addr;
	msg.msg_iov = iov;
	msg.context = msg_atomic->context;
	msg.iov_count = msg_atomic->iov_count;
	msg.data = msg_atomic->data;
	msg.desc = msg_atomic->desc;
	efa_rdm_txe_construct(txe, efa_rdm_ep, &msg, op, flags);

	assert(msg_atomic->rma_iov_count > 0);
	assert(msg_atomic->rma_iov);
	txe->rma_iov_count = msg_atomic->rma_iov_count;
	ofi_rma_ioc_to_iov(msg_atomic->rma_iov,
			   txe->rma_iov,
			   msg_atomic->rma_iov_count,
			   datatype_size);

	txe->atomic_hdr.atomic_op = msg_atomic->op;
	txe->atomic_hdr.datatype = msg_atomic->datatype;

	if (op == ofi_op_atomic_fetch || op == ofi_op_atomic_compare) {
		assert(atomic_ex);
		memcpy(&txe->atomic_ex, atomic_ex, sizeof(struct efa_rdm_atomic_ex));
	}

	return txe;
}

static
ssize_t efa_rdm_atomic_generic_efa(struct efa_rdm_ep *efa_rdm_ep,
			       const struct fi_msg_atomic *msg,
			       const struct efa_rdm_atomic_ex *atomic_ex,
			       uint32_t op, uint64_t flags)
{
	struct efa_rdm_ope *txe;
	struct efa_rdm_peer *peer;
	bool delivery_complete_requested;
	ssize_t err;
	static int req_pkt_type_list[] = {
		[ofi_op_atomic] = RXR_WRITE_RTA_PKT,
		[ofi_op_atomic_fetch] = RXR_FETCH_RTA_PKT,
		[ofi_op_atomic_compare] = RXR_COMPARE_RTA_PKT
	};

	assert(msg->iov_count <= efa_rdm_ep->tx_iov_limit);
	efa_perfset_start(efa_rdm_ep, perf_efa_tx);

	ofi_mutex_lock(&efa_rdm_ep->base_ep.util_ep.lock);

	peer = efa_rdm_ep_get_peer(efa_rdm_ep, msg->addr);
	assert(peer);

	if (peer->flags & EFA_RDM_PEER_IN_BACKOFF) {
		err = -FI_EAGAIN;
		goto out;
	}

	txe = efa_rdm_atomic_alloc_txe(efa_rdm_ep, msg, atomic_ex, op, flags);
	if (OFI_UNLIKELY(!txe)) {
		err = -FI_EAGAIN;
		efa_rdm_ep_progress_internal(efa_rdm_ep);
		goto out;
	}

	delivery_complete_requested = txe->fi_flags & FI_DELIVERY_COMPLETE;
	if (delivery_complete_requested && !(peer->is_local)) {
		/*
		 * Because delivery complete is defined as an extra
		 * feature, the receiver might not support it.
		 *
		 * The sender cannot send with FI_DELIVERY_COMPLETE
		 * if the peer is not able to handle it.
		 *
		 * If the sender does not know whether the peer
		 * can handle it, it needs to trigger
		 * a handshake packet from the peer.
		 *
		 * The handshake packet contains
		 * the information whether the peer
		 * support it or not.
		 */
		err = rxr_pkt_trigger_handshake(efa_rdm_ep, txe->addr, peer);
		if (OFI_UNLIKELY(err)) {
			efa_rdm_txe_release(txe);
			goto out;
		}

		if (!(peer->flags & EFA_RDM_PEER_HANDSHAKE_RECEIVED)) {
			efa_rdm_txe_release(txe);
			err = -FI_EAGAIN;
			goto out;
		} else if (!efa_rdm_peer_support_delivery_complete(peer)) {
			efa_rdm_txe_release(txe);
			err = -FI_EOPNOTSUPP;
			goto out;
		}
	}

	txe->msg_id = (peer->next_msg_id != ~0) ?
			    peer->next_msg_id++ : ++peer->next_msg_id;

	if (delivery_complete_requested && op == ofi_op_atomic) {
		err = rxr_pkt_post_req(efa_rdm_ep,
				       txe,
				       RXR_DC_WRITE_RTA_PKT,
				       0);
	} else {
		/*
		 * Fetch atomic and compare atomic
		 * support DELIVERY_COMPLETE
		 * by nature
		 */
		err = rxr_pkt_post_req(efa_rdm_ep,
				       txe,
				       req_pkt_type_list[op],
				       0);
	}

	if (OFI_UNLIKELY(err)) {
		efa_rdm_ep_progress_internal(efa_rdm_ep);
		efa_rdm_txe_release(txe);
		peer->next_msg_id--;
	}

out:
	ofi_mutex_unlock(&efa_rdm_ep->base_ep.util_ep.lock);
	efa_perfset_end(efa_rdm_ep, perf_efa_tx);
	return err;
}

static ssize_t
efa_rdm_atomic_inject(struct fid_ep *ep,
		  const void *buf, size_t count,
		  fi_addr_t dest_addr, uint64_t remote_addr, uint64_t remote_key,
		  enum fi_datatype datatype, enum fi_op op)
{
	struct fi_ioc iov;
	struct fi_rma_ioc rma_iov;
	struct fi_msg_atomic msg;

	struct efa_rdm_ep *efa_rdm_ep;
	struct efa_rdm_peer *peer;
	int err;

	efa_rdm_ep = container_of(ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);
	err = efa_rdm_ep_cap_check_atomic(efa_rdm_ep);
	if (err)
		return err;
	peer = efa_rdm_ep_get_peer(efa_rdm_ep, dest_addr);
	assert(peer);
	if (peer->is_local && efa_rdm_ep->use_shm_for_tx) {
		if (!(efa_rdm_ep_domain(efa_rdm_ep)->shm_info->domain_attr->mr_mode & FI_MR_VIRT_ADDR))
			remote_addr = 0;

		return fi_inject_atomic(efa_rdm_ep->shm_ep, buf, count, peer->shm_fiaddr,
					remote_addr, remote_key, datatype, op);
	}

	iov.addr = (void *)buf;
	iov.count = count;

	rma_iov.addr = remote_addr;
	rma_iov.count = count;
	rma_iov.key = remote_key;

	msg.msg_iov = &iov;
	msg.iov_count = 1;
	msg.desc = NULL;
	msg.addr = dest_addr;
	msg.rma_iov = &rma_iov;
	msg.rma_iov_count = 1;
	msg.datatype = datatype;
	msg.op = op;
	msg.context = NULL;
	msg.data = 0;

	return efa_rdm_atomic_generic_efa(efa_rdm_ep, &msg, NULL, ofi_op_atomic,
				      FI_INJECT | EFA_RDM_TXE_NO_COMPLETION);
}

static ssize_t
efa_rdm_atomic_writemsg(struct fid_ep *ep,
		    const struct fi_msg_atomic *msg,
		    uint64_t flags)
{
	struct fi_msg_atomic shm_msg;
	struct efa_rdm_ep *efa_rdm_ep;
	struct efa_rdm_peer *peer;
	struct fi_rma_ioc rma_iov[RXR_IOV_LIMIT];
	void *shm_desc[RXR_IOV_LIMIT];
	int err;

	EFA_DBG(FI_LOG_EP_DATA,
	       "%s: iov_len: %lu flags: %lx\n",
	       __func__, ofi_total_ioc_cnt(msg->msg_iov, msg->iov_count), flags);

	efa_rdm_ep = container_of(ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);
	err = efa_rdm_ep_cap_check_atomic(efa_rdm_ep);
	if (err)
		return err;
	peer = efa_rdm_ep_get_peer(efa_rdm_ep, msg->addr);
	assert(peer);
	if (peer->is_local && efa_rdm_ep->use_shm_for_tx) {
		efa_rdm_atomic_init_shm_msg(efa_rdm_ep, &shm_msg, msg, rma_iov, shm_desc);
		shm_msg.addr = peer->shm_fiaddr;
		return fi_atomicmsg(efa_rdm_ep->shm_ep, &shm_msg, flags);
	}

	return efa_rdm_atomic_generic_efa(efa_rdm_ep, msg, NULL, ofi_op_atomic, flags);
}

static ssize_t
efa_rdm_atomic_writev(struct fid_ep *ep,
		  const struct fi_ioc *iov, void **desc, size_t count,
		  fi_addr_t dest_addr, uint64_t remote_addr, uint64_t remote_key,
		  enum fi_datatype datatype, enum fi_op op, void *context)
{
	struct fi_msg_atomic msg;
	struct fi_rma_ioc rma_ioc;

	rma_ioc.addr = remote_addr;
	rma_ioc.count = ofi_total_ioc_cnt(iov, count);
	rma_ioc.key = remote_key;

	msg.msg_iov = iov;
	msg.iov_count = count;
	msg.desc = desc;
	msg.addr = dest_addr;
	msg.rma_iov = &rma_ioc;
	msg.rma_iov_count = 1;
	msg.datatype = datatype;
	msg.op = op;
	msg.context = context;
	msg.data = 0;

	EFA_DBG(FI_LOG_EP_DATA, "%s total_count=%ld atomic_op=%d\n", __func__,
	       ofi_total_ioc_cnt(iov, count), msg.op);

	return efa_rdm_atomic_writemsg(ep, &msg, 0);
}

static ssize_t
efa_rdm_atomic_write(struct fid_ep *ep,
		 const void *buf, size_t count, void *desc,
		 fi_addr_t dest_addr, uint64_t addr, uint64_t key,
		 enum fi_datatype datatype, enum fi_op op, void *context)
{
	struct fi_ioc ioc;

	ioc.addr = (void *)buf;
	ioc.count = count;
	return efa_rdm_atomic_writev(ep, &ioc, &desc, 1,
				 dest_addr, addr, key,
				 datatype, op, context);
}

static ssize_t
efa_rdm_atomic_readwritemsg(struct fid_ep *ep,
			const struct fi_msg_atomic *msg,
			struct fi_ioc *resultv, void **result_desc, size_t result_count,
			uint64_t flags)
{
	struct efa_rdm_ep *efa_rdm_ep;
	struct efa_rdm_peer *peer;
	struct fi_msg_atomic shm_msg;
	struct fi_rma_ioc shm_rma_iov[RXR_IOV_LIMIT];
	void *shm_desc[RXR_IOV_LIMIT];
	struct efa_rdm_atomic_ex atomic_ex;
	size_t datatype_size;
	int err;

	datatype_size = ofi_datatype_size(msg->datatype);
	if (OFI_UNLIKELY(!datatype_size)) {
		return -errno;
	}

	EFA_DBG(FI_LOG_EP_DATA, "%s total_len=%ld atomic_op=%d\n", __func__,
	       ofi_total_ioc_cnt(msg->msg_iov, msg->iov_count), msg->op);

	efa_rdm_ep = container_of(ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);
	err = efa_rdm_ep_cap_check_atomic(efa_rdm_ep);
	if (err)
		return err;
	peer = efa_rdm_ep_get_peer(efa_rdm_ep, msg->addr);
	assert(peer);
	if (peer->is_local & efa_rdm_ep->use_shm_for_tx) {
		efa_rdm_atomic_init_shm_msg(efa_rdm_ep, &shm_msg, msg, shm_rma_iov, shm_desc);
		shm_msg.addr = peer->shm_fiaddr;
		return fi_fetch_atomicmsg(efa_rdm_ep->shm_ep, &shm_msg,
					  resultv, result_desc, result_count,
					  flags);
	}

	ofi_ioc_to_iov(resultv, atomic_ex.resp_iov, result_count, datatype_size);
	atomic_ex.resp_iov_count = result_count;

	memcpy(atomic_ex.result_desc, result_desc, sizeof(void*) * result_count);
	atomic_ex.compare_desc = NULL;

	return efa_rdm_atomic_generic_efa(efa_rdm_ep, msg, &atomic_ex, ofi_op_atomic_fetch, flags);
}

static ssize_t
efa_rdm_atomic_readwritev(struct fid_ep *ep,
		      const struct fi_ioc *iov, void **desc, size_t count,
		      struct fi_ioc *resultv, void **result_desc, size_t result_count,
		      fi_addr_t dest_addr, uint64_t addr, uint64_t key,
		      enum fi_datatype datatype, enum fi_op op, void *context)
{
	struct fi_msg_atomic msg;
	struct fi_rma_ioc rma_ioc;

	rma_ioc.addr = addr;
	rma_ioc.count = ofi_total_ioc_cnt(iov, count);
	rma_ioc.key = key;

	msg.msg_iov = iov;
	msg.iov_count = count;
	msg.desc = desc;
	msg.addr = dest_addr;
	msg.rma_iov = &rma_ioc;
	msg.rma_iov_count = 1;
	msg.datatype = datatype;
	msg.op = op;
	msg.context = context;
	msg.data = 0;

	return efa_rdm_atomic_readwritemsg(ep, &msg, resultv, result_desc, result_count, 0);
}

static ssize_t
efa_rdm_atomic_readwrite(struct fid_ep *ep,
		     const void *buf, size_t count, void *desc,
		     void *result, void *result_desc,
		     fi_addr_t dest_addr, uint64_t addr, uint64_t key,
		     enum fi_datatype datatype, enum fi_op op, void *context)
{
	struct fi_ioc ioc, resp_ioc;

	ioc.addr = (void *)buf;
	ioc.count = count;
	resp_ioc.addr = result;
	resp_ioc.count = count;

	return efa_rdm_atomic_readwritev(ep, &ioc, &desc, 1,
				     &resp_ioc, &result_desc, 1,
				     dest_addr, addr, key,
				     datatype, op, context);
}

static ssize_t
efa_rdm_atomic_compwritemsg(struct fid_ep *ep,
			const struct fi_msg_atomic *msg,
			const struct fi_ioc *comparev, void **compare_desc, size_t compare_count,
			struct fi_ioc *resultv, void **result_desc, size_t result_count,
			uint64_t flags)
{
	struct efa_rdm_ep *efa_rdm_ep;
	struct efa_rdm_peer *peer;
	struct fi_msg_atomic shm_msg;
	struct fi_rma_ioc shm_rma_iov[RXR_IOV_LIMIT];
	void *shm_desc[RXR_IOV_LIMIT];
	struct efa_rdm_atomic_ex atomic_ex;
	size_t datatype_size;
	int err;

	datatype_size = ofi_datatype_size(msg->datatype);
	if (OFI_UNLIKELY(!datatype_size)) {
		return -errno;
	}

	EFA_DBG(FI_LOG_EP_DATA,
	       "%s: iov_len: %lu flags: %lx\n",
	       __func__, ofi_total_ioc_cnt(msg->msg_iov, msg->iov_count), flags);

	efa_rdm_ep = container_of(ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);
	err = efa_rdm_ep_cap_check_atomic(efa_rdm_ep);
	if (err)
		return err;
	peer = efa_rdm_ep_get_peer(efa_rdm_ep, msg->addr);
	assert(peer);
	if (peer->is_local && efa_rdm_ep->use_shm_for_tx) {
		efa_rdm_atomic_init_shm_msg(efa_rdm_ep, &shm_msg, msg, shm_rma_iov, shm_desc);
		shm_msg.addr = peer->shm_fiaddr;
		return fi_compare_atomicmsg(efa_rdm_ep->shm_ep, &shm_msg,
					    comparev, compare_desc, compare_count,
					    resultv, result_desc, result_count,
					    flags);
	}

	ofi_ioc_to_iov(resultv, atomic_ex.resp_iov, result_count, datatype_size);
	atomic_ex.resp_iov_count = result_count;

	ofi_ioc_to_iov(comparev, atomic_ex.comp_iov, compare_count, datatype_size);
	atomic_ex.comp_iov_count = compare_count;

	memcpy(atomic_ex.result_desc, result_desc, sizeof(void*) * result_count);
	atomic_ex.compare_desc = compare_desc;

	return efa_rdm_atomic_generic_efa(efa_rdm_ep, msg, &atomic_ex, ofi_op_atomic_compare, flags);
}

static ssize_t
efa_rdm_atomic_compwritev(struct fid_ep *ep,
		      const struct fi_ioc *iov, void **desc, size_t count,
		      const struct fi_ioc *comparev, void **compare_desc, size_t compare_count,
		      struct fi_ioc *resultv, void **result_desc, size_t result_count,
		      fi_addr_t dest_addr, uint64_t rma_addr, uint64_t rma_key,
		      enum fi_datatype datatype, enum fi_op op, void *context)
{
	struct fi_msg_atomic msg;
	struct fi_rma_ioc rma_ioc;

	rma_ioc.addr = rma_addr;
	rma_ioc.count = ofi_total_ioc_cnt(iov, count);
	rma_ioc.key = rma_key;

	msg.msg_iov = iov;
	msg.iov_count = count;
	msg.desc = desc;
	msg.addr = dest_addr;
	msg.rma_iov = &rma_ioc;
	msg.rma_iov_count = 1;
	msg.datatype = datatype;
	msg.op = op;
	msg.context = context;
	msg.data = 0;

	return efa_rdm_atomic_compwritemsg(ep, &msg,
				       comparev, compare_desc, compare_count,
				       resultv, result_desc, result_count,
				       0);
}

static ssize_t
efa_rdm_atomic_compwrite(struct fid_ep *ep,
		     const void *buf, size_t count, void *desc,
		     const void *compare, void *compare_desc,
		     void *result, void *result_desc,
		     fi_addr_t dest_addr, uint64_t addr, uint64_t key,
		     enum fi_datatype datatype, enum fi_op op, void *context)
{
	struct fi_ioc ioc, resp_ioc, comp_ioc;

	ioc.addr = (void *)buf;
	ioc.count = count;
	resp_ioc.addr = result;
	resp_ioc.count = count;
	comp_ioc.addr = (void *)compare;
	comp_ioc.count = count;

	return efa_rdm_atomic_compwritev(ep, &ioc, &desc, 1,
				     &comp_ioc, &compare_desc, 1,
				     &resp_ioc, &result_desc, 1,
				     dest_addr, addr, key,
				     datatype, op, context);
}

int efa_rdm_atomic_query(struct fid_domain *domain,
		         enum fi_datatype datatype, enum fi_op op,
			 struct fi_atomic_attr *attr, uint64_t flags)
{
	struct efa_domain *efa_domain;
	int ret;
	size_t max_atomic_size;

	if (flags & FI_TAGGED) {
		EFA_WARN(FI_LOG_EP_CTRL,
			"tagged atomic op not supported\n");
		return -FI_EINVAL;
	}

	if ((datatype == FI_INT128) || (datatype == FI_UINT128)) {
		EFA_WARN(FI_LOG_EP_CTRL,
			"128-bit atomic integers not supported\n");
		return -FI_EOPNOTSUPP;
	}

	ret = ofi_atomic_valid(&efa_prov, datatype, op, flags);
	if (ret || !attr)
		return ret;

	efa_domain = container_of(domain, struct efa_domain,
				  util_domain.domain_fid);

	max_atomic_size = efa_domain->mtu_size - sizeof(struct rxr_rta_hdr)
			  - efa_domain->addrlen
			  - RXR_IOV_LIMIT * sizeof(struct fi_rma_iov);

	if (flags & FI_COMPARE_ATOMIC)
		max_atomic_size /= 2;

	attr->size = ofi_datatype_size(datatype);
	if (OFI_UNLIKELY(!attr->size)) {
		return -errno;
	}
	attr->count = max_atomic_size / attr->size;
	return 0;
}

static int efa_rdm_atomic_valid(struct fid_ep *ep_fid, enum fi_datatype datatype,
			    enum fi_op op, uint64_t flags, size_t *count)
{
	struct util_ep *ep;
	struct efa_rdm_ep *efa_rdm_ep;
	struct fi_atomic_attr attr;
	int ret;

	ep = container_of(ep_fid, struct util_ep, ep_fid);
	efa_rdm_ep = container_of(ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);

	ret = efa_rdm_ep_cap_check_atomic(efa_rdm_ep);
	if (ret)
		return ret;

	ret = efa_rdm_atomic_query(&ep->domain->domain_fid,
				   datatype, op, &attr, flags);
	if (!ret)
		*count = attr.count;

	return ret;
}

static int efa_rdm_atomic_write_valid(struct fid_ep *ep, enum fi_datatype datatype,
				  enum fi_op op, size_t *count)
{
	return efa_rdm_atomic_valid(ep, datatype, op, 0, count);
}

static int efa_rdm_atomic_readwrite_valid(struct fid_ep *ep,
				      enum fi_datatype datatype, enum fi_op op,
				      size_t *count)
{
	return efa_rdm_atomic_valid(ep, datatype, op, FI_FETCH_ATOMIC, count);
}

static int efa_rdm_atomic_compwrite_valid(struct fid_ep *ep,
				      enum fi_datatype datatype, enum fi_op op,
				      size_t *count)
{
	return efa_rdm_atomic_valid(ep, datatype, op, FI_COMPARE_ATOMIC, count);
}

struct fi_ops_atomic efa_rdm_atomic_ops = {
	.size = sizeof(struct fi_ops_atomic),
	.write = efa_rdm_atomic_write,
	.writev = efa_rdm_atomic_writev,
	.writemsg = efa_rdm_atomic_writemsg,
	.inject = efa_rdm_atomic_inject,
	.readwrite = efa_rdm_atomic_readwrite,
	.readwritev = efa_rdm_atomic_readwritev,
	.readwritemsg = efa_rdm_atomic_readwritemsg,
	.compwrite = efa_rdm_atomic_compwrite,
	.compwritev = efa_rdm_atomic_compwritev,
	.compwritemsg = efa_rdm_atomic_compwritemsg,
	.writevalid = efa_rdm_atomic_write_valid,
	.readwritevalid = efa_rdm_atomic_readwrite_valid,
	.compwritevalid = efa_rdm_atomic_compwrite_valid,
};

