/*
 * Copyright (c) Amazon.com, Inc. or its affiliates.
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

#include "efa.h"
#include "efa_av.h"
#include "efa_rdm_ep.h"

#include "rxr_tp.h"
#include "efa_cntr.h"
#include "rxr_pkt_cmd.h"
#include "rxr_pkt_pool.h"
#include "rxr_pkt_type_base.h"

/**
 * @brief post an internal receive buffer to lower endpoint
 *
 * The buffer was posted as undirected recv, (address was set to FI_ADDR_UNSPEC)
 *
 * @param[in]	ep		endpoint
 * @param[in]	flags		flags passed to lower provider, can have FI_MORE
 * @return	On success, return 0
 * 		On failure, return a negative error code.
 */
int efa_rdm_ep_post_internal_rx_pkt(struct efa_rdm_ep *ep, uint64_t flags)
{
	void *desc;
	struct rxr_pkt_entry *rx_pkt_entry = NULL;
	int ret = 0;

	rx_pkt_entry = rxr_pkt_entry_alloc(ep, ep->efa_rx_pkt_pool, RXR_PKT_FROM_EFA_RX_POOL);

	if (OFI_UNLIKELY(!rx_pkt_entry)) {
		EFA_WARN(FI_LOG_EP_CTRL,
			"Unable to allocate rx_pkt_entry\n");
		return -FI_ENOMEM;
	}

	rx_pkt_entry->ope = NULL;

#if ENABLE_DEBUG
	dlist_insert_tail(&rx_pkt_entry->dbg_entry,
				  &ep->rx_posted_buf_list);
#endif
	desc = fi_mr_desc(rx_pkt_entry->mr);
	ret = rxr_pkt_entry_recv(ep, rx_pkt_entry, &desc, flags);
	if (OFI_UNLIKELY(ret)) {
		rxr_pkt_entry_release_rx(ep, rx_pkt_entry);
		EFA_WARN(FI_LOG_EP_CTRL,
			"failed to post buf %d (%s)\n", -ret,
			fi_strerror(-ret));
		return ret;
	}
	ep->efa_rx_pkts_posted++;

	return 0;
}

/**
 * @brief bulk post internal receive buffer(s) to device
 *
 * When posting multiple buffers, this function will use
 * FI_MORE flag to achieve better performance.
 *
 * @param[in]	ep		endpint
 * @param[in]	nrecv		number of receive buffers to post
 * @return	On success, return 0
 * 		On failure, return negative libfabric error code
 */
static inline
ssize_t efa_rdm_ep_bulk_post_internal_rx_pkts(struct efa_rdm_ep *ep, int nrecv)
{
	int i;
	ssize_t err;
	uint64_t flags;

	flags = FI_MORE;
	for (i = 0; i < nrecv; ++i) {
		if (i == nrecv - 1)
			flags = 0;

		err = efa_rdm_ep_post_internal_rx_pkt(ep, flags);
		if (OFI_UNLIKELY(err))
			return err;
	}

	return 0;
}

/*
 * @brief explicitly allocate a chunk of memory for 6 pools on RX side:
 *     efa's receive packet pool (efa_rx_pkt_pool)
 *     unexpected packet pool (rx_unexp_pkt_pool),
 *     out-of-order packet pool (rx_ooo_pkt_pool), and
 *     local read-copy packet pool (rx_readcopy_pkt_pool).
 *
 * This function is called when the progress engine is called for
 * the 1st time on this endpoint.
 *
 * @param ep[in,out]	endpoint
 * @return		On success, return 0
 * 			On failure, return a negative error code.
 */
int efa_rdm_ep_grow_rx_pools(struct efa_rdm_ep *ep)
{
	int err;

	assert(ep->efa_rx_pkt_pool);
	err = rxr_pkt_pool_grow(ep->efa_rx_pkt_pool);
	if (err) {
		EFA_WARN(FI_LOG_CQ,
			"cannot allocate memory for EFA's RX packet pool. error: %s\n",
			strerror(-err));
		return err;
	}

	if (ep->rx_unexp_pkt_pool) {
		assert(ep->rx_unexp_pkt_pool);
		err = rxr_pkt_pool_grow(ep->rx_unexp_pkt_pool);
		if (err) {
			EFA_WARN(FI_LOG_CQ,
				"cannot allocate memory for unexpected packet pool. error: %s\n",
				strerror(-err));
			return err;
		}
	}

	if (ep->rx_ooo_pkt_pool) {
		assert(ep->rx_ooo_pkt_pool);
		err = rxr_pkt_pool_grow(ep->rx_ooo_pkt_pool);
		if (err) {
			EFA_WARN(FI_LOG_CQ,
				"cannot allocate memory for out-of-order packet pool. error: %s\n",
				strerror(-err));
			return err;
		}
	}

	if (ep->rx_readcopy_pkt_pool) {
		err = rxr_pkt_pool_grow(ep->rx_readcopy_pkt_pool);
		if (err) {
			EFA_WARN(FI_LOG_CQ,
				"cannot allocate and register memory for readcopy packet pool. error: %s\n",
				strerror(-err));
			return err;
		}
	}

	if (ep->map_entry_pool) {
		err = ofi_bufpool_grow(ep->map_entry_pool);
		if (err) {
			EFA_WARN(FI_LOG_CQ,
				 "cannot allocate memory for map entry pool. error: %s\n",
				 strerror(-err));
		}

		return err;
	}

	return 0;
}

/**
 * @brief post internal receive buffers for progress engine.
 *
 * It is more efficient to post multiple receive buffers
 * to the device at once than to post each receive buffer
 * individually.
 *
 * Therefore, after an internal receive buffer (a packet
 * entry) was processed, it is not posted to the device
 * right away.
 *
 * Instead, we increase counter
 *      ep->efa_rx_pkts_to_post
 * by one.
 *
 * Later, progress engine calls this function to
 * bulk post internal receive buffers (according to
 * the counter).
 *
 * This function also control number of internal
 * buffers posted to the device in zero copy receive
 * mode.
 *
 * param[in]	ep	endpoint
 */
static inline
void efa_rdm_ep_progress_post_internal_rx_pkts(struct efa_rdm_ep *ep)
{
	int err;

	if (ep->use_zcpy_rx) {
		/*
		 * In zero copy receive mode,
		 *
		 * If application did not post any receive buffer,
		 * we post one internal buffer so endpoint can
		 * receive RxR control packets such as handshake.
		 *
		 * If buffers have posted to the device, we do NOT
		 * repost internal buffers to maximize the chance
		 * user buffer is used to receive data.
		 */
		if (ep->efa_rx_pkts_posted == 0 && ep->efa_rx_pkts_to_post == 0) {
			ep->efa_rx_pkts_to_post = 1;
		} else if (ep->efa_rx_pkts_posted > 0 && ep->efa_rx_pkts_to_post > 0){
			ep->efa_rx_pkts_to_post = 0;
		}
	} else {
		if (ep->efa_rx_pkts_posted == 0 && ep->efa_rx_pkts_to_post == 0) {
			/* Both efa_rx_pkts_posted and efa_rx_pkts_to_post equal to 0 means
			 * this is the first call of the progress engine on this endpoint.
			 *
			 * In this case, we explictly allocate the 1st chunk of memory
			 * for unexp/ooo/readcopy RX packet pool.
			 *
			 * The reason to explicitly allocate the memory for RX packet
			 * pool is to improve efficiency.
			 *
			 * Without explicit memory allocation, a pkt pools's memory
			 * is allocated when 1st packet is allocated from it.
			 * During the computation, different processes got their 1st
			 * unexp/ooo/read-copy packet at different time. Therefore,
			 * if we do not explicitly allocate memory at the beginning,
			 * memory will be allocated at different time.
			 *
			 * When one process is allocating memory, other processes
			 * have to wait. When each process allocate memory at different
			 * time, the accumulated waiting time became significant.
			 *
			 * By explicitly allocating memory at 1st call to progress
			 * engine, the memory allocation is parallelized.
			 * (This assumes the 1st call to the progress engine on
			 * all processes happen at roughly the same time, which
			 * is a valid assumption according to our knowledge of
			 * the workflow of most application)
			 *
			 * The memory was not allocated during endpoint initialization
			 * because some applications will initialize some endpoints
			 * but never uses it, thus allocating memory initialization
			 * causes waste.
			 */
			err = efa_rdm_ep_grow_rx_pools(ep);
			if (err)
				goto err_exit;

			ep->efa_rx_pkts_to_post = rxr_get_rx_pool_chunk_cnt(ep);
		}
	}

	err = efa_rdm_ep_bulk_post_internal_rx_pkts(ep, ep->efa_rx_pkts_to_post);
	if (err)
		goto err_exit;

	ep->efa_rx_pkts_to_post = 0;

	return;

err_exit:

	efa_base_ep_write_eq_error(&ep->base_ep, err, FI_EFA_ERR_INTERNAL_RX_BUF_POST);
}

static inline
void efa_rdm_ep_check_peer_backoff_timer(struct efa_rdm_ep *ep)
{
	struct efa_rdm_peer *peer;
	struct dlist_entry *tmp;

	if (OFI_LIKELY(dlist_empty(&ep->peer_backoff_list)))
		return;

	dlist_foreach_container_safe(&ep->peer_backoff_list, struct efa_rdm_peer,
				     peer, rnr_backoff_entry, tmp) {
		if (ofi_gettime_us() >= peer->rnr_backoff_begin_ts +
					peer->rnr_backoff_wait_time) {
			peer->flags &= ~EFA_RDM_PEER_IN_BACKOFF;
			dlist_remove(&peer->rnr_backoff_entry);
		}
	}
}

/**
 * @brief handle rdma-core CQ completion resulted from IBV_WRITE_WITH_IMM
 *
 * This function handles hardware-assisted RDMA writes with immediate data at
 * remote endpoint.  These do not have a packet context, nor do they have a
 * connid available.
 *
 * @param[in,out]	ep		endpoint
 * @param[in]		int32_t		Data provided in the IMMEDIATE value.
 * @param[in]		flags		flags (such as FI_REMOTE_CQ_DATA)
 */
void efa_rdm_ep_proc_ibv_recv_rdma_with_imm_completion(struct efa_rdm_ep *ep,
						       int32_t imm_data,
						       uint64_t flags,
						       struct rxr_pkt_entry *pkt_entry)
{
	struct util_cq *target_cq;
	int ret;
	fi_addr_t src_addr;
	struct efa_av *efa_av;

	target_cq = ep->base_ep.util_ep.rx_cq;
	efa_av = ep->base_ep.av;

	if (ep->base_ep.util_ep.caps & FI_SOURCE) {
		src_addr = efa_av_reverse_lookup_rdm(efa_av,
						ibv_wc_read_slid(ep->ibv_cq_ex),
						ibv_wc_read_src_qp(ep->ibv_cq_ex),
						NULL);
		ret = ofi_cq_write_src(target_cq, NULL, flags, 0, NULL, imm_data, 0, src_addr);
	} else {
		ret = ofi_cq_write(target_cq, NULL, flags, 0, NULL, imm_data, 0);
	}

	if (OFI_UNLIKELY(ret)) {
		EFA_WARN(FI_LOG_CQ,
			"Unable to write a cq entry for remote for RECV_RDMA operation: %s\n",
			fi_strerror(-ret));
		efa_base_ep_write_eq_error(&ep->base_ep, FI_EIO, FI_EFA_ERR_WRITE_SHM_CQ_ENTRY);
	}

	efa_cntr_report_rx_completion(&ep->base_ep.util_ep, flags);

	/* Recv with immediate will consume a pkt_entry, but the pkt is not
	   filled, so free the pkt_entry and record we have one less posted
	   packet now. */
	ep->efa_rx_pkts_posted--;
	rxr_pkt_entry_release_rx(ep, pkt_entry);
}

#if HAVE_EFADV_CQ_EX
/**
 * @brief Read peer raw address from EFA device and look up the peer address in AV.
 * This function should only be called if the peer AH is unknown.
 * @return Peer address, or FI_ADDR_NOTAVAIL if unavailable.
 */
static inline
fi_addr_t efa_rdm_ep_determine_peer_address_from_efadv(struct efa_rdm_ep *ep,
						       struct ibv_cq_ex *ibv_cqx)
{
	struct rxr_pkt_entry *pkt_entry;
	struct efa_ep_addr efa_ep_addr = {0};
	fi_addr_t addr;
	union ibv_gid gid = {0};
	uint32_t *connid = NULL;

	if (ep->ibv_cq_ex_type != EFADV_CQ) {
		/* EFA DV CQ is not supported. This could be due to old EFA kernel module versions. */
		return FI_ADDR_NOTAVAIL;
	}

	/* Attempt to read sgid from EFA firmware */
	if (efadv_wc_read_sgid(efadv_cq_from_ibv_cq_ex(ibv_cqx), &gid) < 0) {
		/* Return code is negative if the peer AH is known */
		return FI_ADDR_NOTAVAIL;
	}

	pkt_entry = (void *)(uintptr_t)ibv_cqx->wr_id;

	connid = rxr_pkt_connid_ptr(pkt_entry);
	if (!connid) {
		return FI_ADDR_NOTAVAIL;
	}

	/*
	 * Use raw:qpn:connid as the key to lookup AV for peer's fi_addr
	 */
	memcpy(efa_ep_addr.raw, gid.raw, sizeof(efa_ep_addr.raw));
	efa_ep_addr.qpn = ibv_wc_read_src_qp(ibv_cqx);
	efa_ep_addr.qkey = *connid;
	addr = ofi_av_lookup_fi_addr(&ep->base_ep.av->util_av, &efa_ep_addr);
	if (addr != FI_ADDR_NOTAVAIL) {
		char gid_str_cdesc[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, gid.raw, gid_str_cdesc, INET6_ADDRSTRLEN);
		EFA_WARN(FI_LOG_AV,
				"Recovered peer fi_addr. [Raw]:[QPN]:[QKey] = [%s]:[%" PRIu16 "]:[%" PRIu32 "]\n",
				gid_str_cdesc, efa_ep_addr.qpn, efa_ep_addr.qkey);
	}

	return addr;
}

/**
 * @brief Determine peer address from ibv_cq_ex
 * Attempt to inject or determine peer address if not available. This usually
 * happens when the endpoint receives the first packet from a new peer.
 * There is an edge case for EFA endpoint - the device might lose the address
 * handle of a known peer due to a firmware bug and return FI_ADDR_NOTAVAIL.
 * The provider needs to look up the address using Raw address:QPN:QKey.
 * Note: This function introduces addtional overhead. It should only be called if
 * efa_av_lookup_address_rdm fails to find the peer address.
 * @param ep Pointer to RDM endpoint
 * @param ibv_cqx Pointer to CQ
 * @returns Peer address, or FI_ADDR_NOTAVAIL if unsuccessful.
 */
static inline fi_addr_t efa_rdm_ep_determine_addr_from_ibv_cq(struct efa_rdm_ep *ep, struct ibv_cq_ex *ibv_cqx)
{
	struct rxr_pkt_entry *pkt_entry;
	fi_addr_t addr = FI_ADDR_NOTAVAIL;

	pkt_entry = (void *)(uintptr_t)ibv_cqx->wr_id;

	addr = rxr_pkt_determine_addr(ep, pkt_entry);

	if (addr == FI_ADDR_NOTAVAIL) {
		addr = efa_rdm_ep_determine_peer_address_from_efadv(ep, ibv_cqx);
	}

	return addr;
}
#else
/**
 * @brief Determine peer address from ibv_cq_ex
 * Attempt to inject peer address if not available. This usually
 * happens when the endpoint receives the first packet from a new peer.
 * Note: This function introduces addtional overhead. It should only be called if
 * efa_av_lookup_address_rdm fails to find the peer address.
 * @param ep Pointer to RDM endpoint
 * @param ibv_cqx Pointer to CQ
 * @returns Peer address, or FI_ADDR_NOTAVAIL if unsuccessful.
 */
static inline
fi_addr_t efa_rdm_ep_determine_addr_from_ibv_cq(struct efa_rdm_ep *ep, struct ibv_cq_ex *ibv_cqx)
{
	struct rxr_pkt_entry *pkt_entry;

	pkt_entry = (void *)(uintptr_t)ibv_cqx->wr_id;

	return rxr_pkt_determine_addr(ep, pkt_entry);
}
#endif

/**
 * @brief poll rdma-core cq and process the cq entry
 *
 * @param[in]	ep	RDM endpoint
 * @param[in]	cqe_to_process	Max number of cq entry to poll and process. Must be positive.
 */
static inline void efa_rdm_ep_poll_ibv_cq(struct efa_rdm_ep *ep, size_t cqe_to_process)
{
	bool should_end_poll = false;
	/* Initialize an empty ibv_poll_cq_attr struct for ibv_start_poll.
	 * EFA expects .comp_mask = 0, or otherwise returns EINVAL.
	 */
	struct ibv_poll_cq_attr poll_cq_attr = {.comp_mask = 0};
	struct efa_av *efa_av;
	struct rxr_pkt_entry *pkt_entry;
	ssize_t err;
	size_t i = 0;
	int prov_errno;

	assert(cqe_to_process > 0);

	efa_av = ep->base_ep.av;

	/* Call ibv_start_poll only once */
	err = ibv_start_poll(ep->ibv_cq_ex, &poll_cq_attr);
	should_end_poll = !err;

	while (!err) {
		pkt_entry = (void *)(uintptr_t)ep->ibv_cq_ex->wr_id;
		rxr_tracepoint(poll_cq, (size_t) ep->ibv_cq_ex->wr_id);
		if (ep->ibv_cq_ex->status) {
			prov_errno = ibv_wc_read_vendor_err(ep->ibv_cq_ex);
			if (ibv_wc_read_opcode(ep->ibv_cq_ex) == IBV_WC_SEND) {
#if ENABLE_DEBUG
				ep->failed_send_comps++;
#endif
				rxr_pkt_handle_send_error(ep, pkt_entry, FI_EIO, prov_errno);
			} else {
				assert(ibv_wc_read_opcode(ep->ibv_cq_ex) == IBV_WC_RECV);
				rxr_pkt_handle_recv_error(ep, pkt_entry, FI_EIO, prov_errno);
			}
			break;
		}

		switch (ibv_wc_read_opcode(ep->ibv_cq_ex)) {
		case IBV_WC_SEND:
#if ENABLE_DEBUG
			ep->send_comps++;
#endif
			rxr_pkt_handle_send_completion(ep, pkt_entry);
			break;
		case IBV_WC_RECV:
			pkt_entry->addr = efa_av_reverse_lookup_rdm(efa_av, ibv_wc_read_slid(ep->ibv_cq_ex),
								ibv_wc_read_src_qp(ep->ibv_cq_ex), pkt_entry);

			if (pkt_entry->addr == FI_ADDR_NOTAVAIL) {
				pkt_entry->addr = efa_rdm_ep_determine_addr_from_ibv_cq(ep, ep->ibv_cq_ex);
			}

			pkt_entry->pkt_size = ibv_wc_read_byte_len(ep->ibv_cq_ex);
			assert(pkt_entry->pkt_size > 0);
			rxr_pkt_handle_recv_completion(ep, pkt_entry);
#if ENABLE_DEBUG
			ep->recv_comps++;
#endif
			break;
		case IBV_WC_RDMA_READ:
		case IBV_WC_RDMA_WRITE:
			rxr_pkt_handle_rma_completion(ep, pkt_entry);
			break;
		case IBV_WC_RECV_RDMA_WITH_IMM:
			efa_rdm_ep_proc_ibv_recv_rdma_with_imm_completion(ep,
				ibv_wc_read_imm_data(ep->ibv_cq_ex),
				FI_REMOTE_CQ_DATA | FI_RMA | FI_REMOTE_WRITE,
				pkt_entry );
			break;
		default:
			EFA_WARN(FI_LOG_EP_CTRL,
				"Unhandled cq type\n");
			assert(0 && "Unhandled cq type");
		}

		i++;
		if (i == cqe_to_process) {
			break;
		}

		/*
		 * ibv_next_poll MUST be call after the current WC is fully processed,
		 * which prevents later calls on ibv_cq_ex from reading the wrong WC.
		 */
		err = ibv_next_poll(ep->ibv_cq_ex);
	}

	if (err && err != ENOENT) {
		err = err > 0 ? err : -err;
		prov_errno = ibv_wc_read_vendor_err(ep->ibv_cq_ex);
		efa_base_ep_write_eq_error(&ep->base_ep, err, prov_errno);
	}

	if (should_end_poll)
		ibv_end_poll(ep->ibv_cq_ex);
}


/**
 * @brief send a linked list of packets
 * 
 * @param[in]	ep	RDM endpoint
 * @param[in]	pkts	Linked list of packets to send
 * @return		0 on success, negative error code on failure
 */
static inline
ssize_t efa_rdm_ep_send_queued_pkts(struct efa_rdm_ep *ep,
				    struct dlist_entry *pkts)
{
	struct dlist_entry *tmp;
	struct rxr_pkt_entry *pkt_entry;
	ssize_t ret;

	dlist_foreach_container_safe(pkts, struct rxr_pkt_entry,
				     pkt_entry, entry, tmp) {

		/* If send succeeded, pkt_entry->entry will be added
		 * to peer->outstanding_tx_pkts. Therefore, it must
		 * be removed from the list before send.
		 */
		dlist_remove(&pkt_entry->entry);

		ret = rxr_pkt_entry_send(ep, pkt_entry, 0);
		if (ret) {
			if (ret == -FI_EAGAIN) {
				/* add the pkt back to pkts, so it can be resent again */
				dlist_insert_tail(&pkt_entry->entry, pkts);
			}

			return ret;
		}
	}
	return 0;
}

/**
 * @brief thread unsafe progress engine of EFA RDM endpoint
 * this function should be called only when the caller holds the
 * endpoint lock.
 * @param[in,out]	ep		EFA RDM endpoint
 */
void efa_rdm_ep_progress_internal(struct efa_rdm_ep *ep)
{
	struct ibv_send_wr *bad_wr;
	struct efa_rdm_ope *ope;
	struct efa_rdm_peer *peer;
	struct dlist_entry *tmp;
	ssize_t ret;
	uint64_t flags;

	/* Poll the EFA completion queue. Restrict poll size
	 * to avoid CQE flooding and thereby blocking user thread. */
	efa_rdm_ep_poll_ibv_cq(ep, efa_env.efa_cq_read_size);

	efa_rdm_ep_progress_post_internal_rx_pkts(ep);

	efa_rdm_ep_check_peer_backoff_timer(ep);
	/*
	 * Resend handshake packet for any peers where the first
	 * handshake send failed.
	 */
	dlist_foreach_container_safe(&ep->handshake_queued_peer_list,
				     struct efa_rdm_peer, peer,
				     handshake_queued_entry, tmp) {
		if (peer->flags & EFA_RDM_PEER_IN_BACKOFF)
			continue;

		ret = rxr_pkt_post_handshake(ep, peer);
		if (ret == -FI_EAGAIN)
			break;

		if (OFI_UNLIKELY(ret)) {
			EFA_WARN(FI_LOG_EP_CTRL,
				"Failed to post HANDSHAKE to peer %ld: %s\n",
				peer->efa_fiaddr, fi_strerror(-ret));
			efa_base_ep_write_eq_error(&ep->base_ep, FI_EIO, FI_EFA_ERR_PEER_HANDSHAKE);
			return;
		}

		dlist_remove(&peer->handshake_queued_entry);
		peer->flags &= ~EFA_RDM_PEER_HANDSHAKE_QUEUED;
		peer->flags |= EFA_RDM_PEER_HANDSHAKE_SENT;
	}

	/*
	 * Send any queued ctrl packets.
	 */
	dlist_foreach_container_safe(&ep->ope_queued_rnr_list,
				     struct efa_rdm_ope,
				     ope, queued_rnr_entry, tmp) {
		peer = efa_rdm_ep_get_peer(ep, ope->addr);
		assert(peer);

		if (peer->flags & EFA_RDM_PEER_IN_BACKOFF)
			continue;

		assert(ope->rxr_flags & EFA_RDM_OPE_QUEUED_RNR);
		assert(!dlist_empty(&ope->queued_pkts));
		ret = efa_rdm_ep_send_queued_pkts(ep, &ope->queued_pkts);

		if (ret == -FI_EAGAIN)
			break;

		if (OFI_UNLIKELY(ret)) {
			assert(ope->type == EFA_RDM_RXE || ope->type == EFA_RDM_TXE);
			if (ope->type == EFA_RDM_RXE)
				efa_rdm_rxe_handle_error(ope, -ret, FI_EFA_ERR_PKT_SEND);
			else
				efa_rdm_txe_handle_error(ope, -ret, FI_EFA_ERR_PKT_SEND);
			return;
		}

		dlist_remove(&ope->queued_rnr_entry);
		ope->rxr_flags &= ~EFA_RDM_OPE_QUEUED_RNR;
	}

	dlist_foreach_container_safe(&ep->ope_queued_ctrl_list,
				     struct efa_rdm_ope,
				     ope, queued_ctrl_entry, tmp) {
		peer = efa_rdm_ep_get_peer(ep, ope->addr);
		assert(peer);

		if (peer->flags & EFA_RDM_PEER_IN_BACKOFF)
			continue;

		assert(ope->rxr_flags & EFA_RDM_OPE_QUEUED_CTRL);
		ret = rxr_pkt_post(ep, ope, ope->queued_ctrl_type, 0);
		if (ret == -FI_EAGAIN)
			break;

		if (OFI_UNLIKELY(ret)) {
			efa_rdm_rxe_handle_error(ope, -ret, FI_EFA_ERR_PKT_POST);
			return;
		}

		/* it can happen that rxr_pkt_post() released ope
		 * (if the ope is rxe and packet type is EOR and inject is used). In
		 * that case rxe's state has been set to EFA_RDM_OPE_FREE and
		 * it has been removed from ep->op_queued_entry_list, so nothing
		 * is left to do.
		 */
		if (ope->state == EFA_RDM_OPE_FREE)
			continue;

		ope->rxr_flags &= ~EFA_RDM_OPE_QUEUED_CTRL;
		dlist_remove(&ope->queued_ctrl_entry);
	}

	/*
	 * Send data packets until window or data queue is exhausted.
	 */
	dlist_foreach_container(&ep->ope_longcts_send_list, struct efa_rdm_ope,
				ope, entry) {
		peer = efa_rdm_ep_get_peer(ep, ope->addr);
		assert(peer);
		if (peer->flags & EFA_RDM_PEER_IN_BACKOFF)
			continue;

		/*
		 * Do not send DATA packet until we received HANDSHAKE packet from the peer,
		 * this is because endpoint does not know whether peer need connid in header
		 * until it get the HANDSHAKE packet.
		 *
		 * We only do this for DATA packet because other types of packets always
		 * has connid in there packet header. If peer does not make use of the connid,
		 * the connid can be safely ignored.
		 *
		 * DATA packet is different because for DATA packet connid is an optional
		 * header inserted between the mandatory header and the application data.
		 * Therefore if peer does not use/understand connid, it will take connid
		 * as application data thus cause data corruption.
		 *
		 * This will not cause deadlock because peer will send a HANDSHAKE packet
		 * back upon receiving 1st packet from the endpoint, and in all 3 sub0protocols
		 * (long-CTS message, emulated long-CTS write and emulated long-CTS read)
		 * where DATA packet is used, endpoint will send other types of packet to
		 * peer before sending DATA packets. The workflow of the 3 sub-protocol
		 * can be found in protocol v4 document chapter 3.
		 */
		if (!(peer->flags & EFA_RDM_PEER_HANDSHAKE_RECEIVED))
			continue;
		while (ope->window > 0) {
			flags = FI_MORE;
			if (ep->efa_max_outstanding_tx_ops - ep->efa_outstanding_tx_ops <= 1 ||
			    ope->window <= ep->max_data_payload_size)
				flags = 0;
			/*
			 * The core's TX queue is full so we can't do any
			 * additional work.
			 */
			if (ep->efa_outstanding_tx_ops == ep->efa_max_outstanding_tx_ops)
				goto out;

			if (peer->flags & EFA_RDM_PEER_IN_BACKOFF)
				break;
			ret = rxr_pkt_post(ep, ope, RXR_DATA_PKT, flags);
			if (OFI_UNLIKELY(ret)) {
				if (ret == -FI_EAGAIN)
					goto out;

				efa_rdm_txe_handle_error(ope, -ret, FI_EFA_ERR_PKT_POST);
				return;
			}
		}
	}

	/*
	 * Send remote read requests until finish or error encoutered
	 */
	dlist_foreach_container_safe(&ep->ope_queued_read_list, struct efa_rdm_ope,
				     ope, queued_read_entry, tmp) {
		peer = efa_rdm_ep_get_peer(ep, ope->addr);
		/*
		 * Here peer can be NULL, when the read request is a
		 * local read request. Local read request is used to copy
		 * data from host memory to device memory on same process.
		 */
		if (peer && (peer->flags & EFA_RDM_PEER_IN_BACKOFF))
			continue;

		/*
		 * The core's TX queue is full so we can't do any
		 * additional work.
		 */
		if (ep->efa_outstanding_tx_ops == ep->efa_max_outstanding_tx_ops)
			goto out;

		ret = efa_rdm_ope_post_read(ope);
		if (ret == -FI_EAGAIN)
			break;

		if (OFI_UNLIKELY(ret)) {
			assert(ope->type == EFA_RDM_TXE || ope->type == EFA_RDM_RXE);
			if (ope->type == EFA_RDM_TXE)
				efa_rdm_txe_handle_error(ope, -ret, FI_EFA_ERR_READ_POST);
			else
				efa_rdm_rxe_handle_error(ope, -ret, FI_EFA_ERR_READ_POST);

			return;
		}

		ope->rxr_flags &= ~EFA_RDM_OPE_QUEUED_READ;
		dlist_remove(&ope->queued_read_entry);
	}

out:
	if (ep->base_ep.xmit_more_wr_tail != &ep->base_ep.xmit_more_wr_head) {
		ret = efa_rdm_ep_post_flush(ep, &bad_wr);
		if (OFI_UNLIKELY(ret))
			efa_base_ep_write_eq_error(&ep->base_ep, -ret, FI_EFA_ERR_WR_POST_SEND);
	}

	return;
}

/**
 * @brief progress engine for the EFA RDM endpoint
 * 
 * This function is thread safe.
 * 
 * @param[in] util_ep The endpoint FID to progress
 */
void efa_rdm_ep_progress(struct util_ep *util_ep)
{
	struct efa_rdm_ep *ep;

	ep = container_of(util_ep, struct efa_rdm_ep, base_ep.util_ep);

	ofi_mutex_lock(&ep->base_ep.util_ep.lock);
	efa_rdm_ep_progress_internal(ep);
	ofi_mutex_unlock(&ep->base_ep.util_ep.lock);
}