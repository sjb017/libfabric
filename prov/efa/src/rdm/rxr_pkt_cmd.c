/*
 * Copyright (c) 2019-2022 Amazon.com, Inc. or its affiliates.
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

#include "efa_rdm_msg.h"
#include "efa_cntr.h"
#include "rxr_pkt_cmd.h"
#include "rxr_pkt_type_base.h"

/* Handshake wait timeout in microseconds */
#define RXR_HANDSHAKE_WAIT_TIMEOUT 1000000

/* This file implements 4 actions that can be applied to a packet:
 *          posting,
 *          handling send completion and,
 *          handing recv completion.
 *          dump (for debug only)
 */

/*
 *   rxr_pkt_init_ctrl() uses init functions declared in rxr_pkt_type.h
 */
static
int rxr_pkt_init_ctrl(struct efa_rdm_ep *efa_rdm_ep, int entry_type, struct efa_rdm_ope *x_entry,
		      int ctrl_type, struct rxr_pkt_entry *pkt_entry)
{
	int ret = 0;

	switch (ctrl_type) {
	case RXR_READRSP_PKT:
		ret = rxr_pkt_init_readrsp(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_CTS_PKT:
		ret = rxr_pkt_init_cts(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_EOR_PKT:
		ret = rxr_pkt_init_eor(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_ATOMRSP_PKT:
		ret = rxr_pkt_init_atomrsp(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_RECEIPT_PKT:
		ret = rxr_pkt_init_receipt(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_EAGER_MSGRTM_PKT:
		ret = rxr_pkt_init_eager_msgrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_EAGER_TAGRTM_PKT:
		ret = rxr_pkt_init_eager_tagrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_MEDIUM_MSGRTM_PKT:
		ret = rxr_pkt_init_medium_msgrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_MEDIUM_TAGRTM_PKT:
		ret = rxr_pkt_init_medium_tagrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_LONGCTS_MSGRTM_PKT:
		ret = rxr_pkt_init_longcts_msgrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_LONGCTS_TAGRTM_PKT:
		ret = rxr_pkt_init_longcts_tagrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_LONGREAD_MSGRTM_PKT:
		ret = rxr_pkt_init_longread_msgrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_LONGREAD_TAGRTM_PKT:
		ret = rxr_pkt_init_longread_tagrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_RUNTREAD_MSGRTM_PKT:
		ret = rxr_pkt_init_runtread_msgrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_RUNTREAD_TAGRTM_PKT:
		ret = rxr_pkt_init_runtread_tagrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_EAGER_RTW_PKT:
		ret = rxr_pkt_init_eager_rtw(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_LONGCTS_RTW_PKT:
		ret = rxr_pkt_init_longcts_rtw(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_LONGREAD_RTW_PKT:
		ret = rxr_pkt_init_longread_rtw(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_SHORT_RTR_PKT:
		ret = rxr_pkt_init_short_rtr(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_LONGCTS_RTR_PKT:
		ret = rxr_pkt_init_longcts_rtr(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_WRITE_RTA_PKT:
		ret = rxr_pkt_init_write_rta(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_FETCH_RTA_PKT:
		ret = rxr_pkt_init_fetch_rta(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_COMPARE_RTA_PKT:
		ret = rxr_pkt_init_compare_rta(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_DC_EAGER_MSGRTM_PKT:
		ret = rxr_pkt_init_dc_eager_msgrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_DC_EAGER_TAGRTM_PKT:
		ret = rxr_pkt_init_dc_eager_tagrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_DC_MEDIUM_MSGRTM_PKT:
		ret = rxr_pkt_init_dc_medium_msgrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_DC_MEDIUM_TAGRTM_PKT:
		ret = rxr_pkt_init_dc_medium_tagrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_DC_LONGCTS_MSGRTM_PKT:
		ret = rxr_pkt_init_dc_longcts_msgrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_DC_LONGCTS_TAGRTM_PKT:
		ret = rxr_pkt_init_dc_longcts_tagrtm(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_DC_EAGER_RTW_PKT:
		ret = rxr_pkt_init_dc_eager_rtw(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_DC_LONGCTS_RTW_PKT:
		ret = rxr_pkt_init_dc_longcts_rtw(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_DC_WRITE_RTA_PKT:
		ret = rxr_pkt_init_dc_write_rta(efa_rdm_ep, x_entry, pkt_entry);
		break;
	case RXR_DATA_PKT:
		ret = rxr_pkt_init_data(efa_rdm_ep, x_entry, pkt_entry);
		break;
	default:
		assert(0 && "unknown pkt type to init");
		ret = -FI_EINVAL;
		break;
	}

	return ret;
}

/*
 *   rxr_pkt_handle_ctrl_sent() uses handle_sent() functions declared in rxr_pkt_type.h
 */
static
void rxr_pkt_handle_ctrl_sent(struct efa_rdm_ep *efa_rdm_ep, struct rxr_pkt_entry *pkt_entry)
{
	int ctrl_type = rxr_get_base_hdr(pkt_entry->wiredata)->type;

	switch (ctrl_type) {
	case RXR_READRSP_PKT:
		rxr_pkt_handle_readrsp_sent(efa_rdm_ep, pkt_entry);
		break;
	case RXR_CTS_PKT:
		rxr_pkt_handle_cts_sent(efa_rdm_ep, pkt_entry);
		break;
	case RXR_EOR_PKT:
		rxr_pkt_handle_eor_sent(efa_rdm_ep, pkt_entry);
		break;
	case RXR_ATOMRSP_PKT:
		rxr_pkt_handle_atomrsp_sent(efa_rdm_ep, pkt_entry);
		break;
	case RXR_RECEIPT_PKT:
		rxr_pkt_handle_receipt_sent(efa_rdm_ep, pkt_entry);
		break;
	case RXR_EAGER_MSGRTM_PKT:
	case RXR_EAGER_TAGRTM_PKT:
		rxr_pkt_handle_eager_rtm_sent(efa_rdm_ep, pkt_entry);
		break;
	case RXR_MEDIUM_MSGRTM_PKT:
	case RXR_MEDIUM_TAGRTM_PKT:
	case RXR_DC_MEDIUM_MSGRTM_PKT:
	case RXR_DC_MEDIUM_TAGRTM_PKT:
		rxr_pkt_handle_medium_rtm_sent(efa_rdm_ep, pkt_entry);
		break;
	case RXR_LONGCTS_MSGRTM_PKT:
	case RXR_DC_LONGCTS_MSGRTM_PKT:
	case RXR_LONGCTS_TAGRTM_PKT:
	case RXR_DC_LONGCTS_TAGRTM_PKT:
		rxr_pkt_handle_longcts_rtm_sent(efa_rdm_ep, pkt_entry);
		break;
	case RXR_LONGREAD_MSGRTM_PKT:
	case RXR_LONGREAD_TAGRTM_PKT:
		rxr_pkt_handle_longread_rtm_sent(efa_rdm_ep, pkt_entry);
		break;
	case RXR_RUNTREAD_MSGRTM_PKT:
	case RXR_RUNTREAD_TAGRTM_PKT:
		rxr_pkt_handle_runtread_rtm_sent(efa_rdm_ep, pkt_entry);
		break;
	case RXR_EAGER_RTW_PKT:
		rxr_pkt_handle_eager_rtw_sent(efa_rdm_ep, pkt_entry);
		break;
	case RXR_LONGCTS_RTW_PKT:
	case RXR_DC_LONGCTS_RTW_PKT:
		rxr_pkt_handle_longcts_rtw_sent(efa_rdm_ep, pkt_entry);
		break;
	case RXR_LONGREAD_RTW_PKT:
		rxr_pkt_handle_longread_rtw_sent(efa_rdm_ep, pkt_entry);
		break;
	case RXR_SHORT_RTR_PKT:
	case RXR_LONGCTS_RTR_PKT:
		/* nothing can be done when RTR packets are sent */
		break;
	case RXR_WRITE_RTA_PKT:
	case RXR_DC_WRITE_RTA_PKT:
	case RXR_FETCH_RTA_PKT:
	case RXR_COMPARE_RTA_PKT:
		rxr_pkt_handle_rta_sent(efa_rdm_ep, pkt_entry);
		break;
	case RXR_DC_EAGER_MSGRTM_PKT:
	case RXR_DC_EAGER_TAGRTM_PKT:
	case RXR_DC_EAGER_RTW_PKT:
		break;
	case RXR_DATA_PKT:
		rxr_pkt_handle_data_sent(efa_rdm_ep, pkt_entry);
		break;
	default:
		assert(0 && "Unknown packet type to handle sent");
		break;
	}
}

/**
 * @brief post one packet.
 *
 * The function does the following:
 *  1. construct a packet.
 *  2. send it
 *  3. Upon success, call the packet's sent handler.
 *  4. If inject, call the packet's send completion handler.
 *
 * @param[in]   efa_rdm_ep          endpoint
 * @param[in]   x_entry         pointer to efa_rdm_ope. (either a txe or an rxe)
 * @param[in]   pkt_type        packet type.
 * @param[in]   flags           additional flags to apply for fi_sendmsg.
 *                              currently only accepted flags is FI_MORE.
 * @return      On success return 0, otherwise return a negative libfabric error code.
 *              Possible error code include (but not limited to):
 * 		-FI_EAGAIN	temporary out of resource
 */
static inline
ssize_t rxr_pkt_post_one(struct efa_rdm_ep *efa_rdm_ep, struct efa_rdm_ope *ope,
			 int pkt_type, uint64_t flags)
{
	struct rxr_pkt_entry *pkt_entry;
	struct efa_rdm_peer *peer;
	ssize_t err;
	fi_addr_t addr;

	addr = ope->addr;
	peer = efa_rdm_ep_get_peer(efa_rdm_ep, addr);
	assert(peer);
	pkt_entry = rxr_pkt_entry_alloc(efa_rdm_ep, efa_rdm_ep->efa_tx_pkt_pool, RXR_PKT_FROM_EFA_TX_POOL);

	if (!pkt_entry)
		return -FI_EAGAIN;

	/*
	 * rxr_pkt_init_ctrl will set pkt_entry->send if it want to use multi iov
	 */
	err = rxr_pkt_init_ctrl(efa_rdm_ep, ope->type, ope, pkt_type, pkt_entry);
	if (OFI_UNLIKELY(err)) {
		rxr_pkt_entry_release_tx(efa_rdm_ep, pkt_entry);
		return err;
	}

	/* If the send succeeded, the function rxr_pkt_entry_send will increase the
	 * counter in efa_rdm_ep that tracks number of outstanding TX ops.
	 */
	err = rxr_pkt_entry_send(efa_rdm_ep, pkt_entry, flags);

	if (OFI_UNLIKELY(err)) {
		rxr_pkt_entry_release_tx(efa_rdm_ep, pkt_entry);
		return err;
	}

	peer->flags |= EFA_RDM_PEER_REQ_SENT;
	rxr_pkt_handle_ctrl_sent(efa_rdm_ep, pkt_entry);

	return 0;
}

/**
 * @brief post packet(s) according to packet type.
 *
 * Depend on packet type, this function may post one packet or multiple packets.
 * This is because some REQ packet types such as MEDIUM RTM must be sent as a series of packets.
 *
 * @param[in]   efa_rdm_ep          endpoint
 * @param[in]   ope        pointer to efa_rdm_ope. (either a txe or an rxe)
 * @param[in]   pkt_type        packet type.
 * @return      On success return 0, otherwise return a negative libfabric error code. Possible error codes include:
 * 		-FI_EAGAIN	temporarily  out of resource
 */
ssize_t rxr_pkt_post(struct efa_rdm_ep *ep, struct efa_rdm_ope *ope, int pkt_type, uint64_t flags)
{
	ssize_t err;
	size_t num_req, i;
	uint64_t extra_flags;

	if (rxr_pkt_type_is_mulreq(pkt_type)) {
		if(rxr_pkt_type_is_runt(pkt_type))
			efa_rdm_txe_set_runt_size(ep, ope);

		efa_rdm_txe_set_max_req_data_size(ep, ope, pkt_type);

		num_req = efa_rdm_txe_num_req(ope, pkt_type);

		if (num_req > (ep->efa_max_outstanding_tx_ops - ep->efa_outstanding_tx_ops))
			return -FI_EAGAIN;

		for (i = 0; i < num_req; ++i) {
			extra_flags = (i == num_req - 1) ? 0 : FI_MORE;

			err = rxr_pkt_post_one(ep, ope, pkt_type, flags | extra_flags);
			if (OFI_UNLIKELY(err))
				return err;
		}

		assert(ope->bytes_sent == efa_rdm_ope_mulreq_total_data_size(ope, pkt_type));
		return 0;
	}

	return rxr_pkt_post_one(ep, ope, pkt_type, flags);
}

/**
 * @brief post packet(s) according to packet type. Queue the post if -FI_EAGAIN is encountered.
 *
 * This function will cal rxr_pkt_post() to post packet(s) according to packet type.
 * If rxr_pkt_post() returned -FI_EAGAIN, this function will put the txe in efa_rdm_ep's
 * queued_ctrl_list. The progress engine will try to post the packet later.
 *
 * This function is called by rxr_pkt_post_req() to post MEDIUM RTM packets, and is
 * called by packet handler to post responsive ctrl packet (such as EOR and CTS).
 *
 * @param[in]   efa_rdm_ep          endpoint
 * @param[in]   x_entry         pointer to efa_rdm_ope. (either a txe or an rxe)
 * @param[in]   pkt_type        packet type.
 * @return      On success return 0, otherwise return a negative libfabric error code.
 */
ssize_t rxr_pkt_post_or_queue(struct efa_rdm_ep *ep, struct efa_rdm_ope *ope, int pkt_type)
{
	ssize_t err;

	err = rxr_pkt_post(ep, ope, pkt_type, 0);
	if (err == -FI_EAGAIN) {
		assert(!(ope->rxr_flags & EFA_RDM_OPE_QUEUED_RNR));
		ope->rxr_flags |= EFA_RDM_OPE_QUEUED_CTRL;
		ope->queued_ctrl_type = pkt_type;
		dlist_insert_tail(&ope->queued_ctrl_entry,
				  &ep->ope_queued_ctrl_list);
		err = 0;
	}

	return err;
}

/**
 * @brief post req packet(s). Queue the post for multi-req packet types
 *
 * We must use rxr_pkt_post_or_queue() for multi-req packet types.
 *
 * This is because for multi-req packets, rxr_pkt_post() will
 * send multiple packets.
 *
 * It can happen that 1st packet was sent successfully, and the next
 * one encountered -FI_EAGAIN, which will cause rxr_pkt_post()
 * to return -FI_EAGAIN.
 *
 * If rxr_pkt_post() was used by this function, the -FI_EAGAIN will
 * be returned to user application. User application will then send
 * the entire message again. This would cause the receiver to receive duplicated
 * packets (because 1st packet was sent successfully).
 *
 * @param[in]   efa_rdm_ep          endpoint
 * @param[in]   ope        pointer to efa_rdm_ope. (either a txe or an rxe)
 * @param[in]   pkt_type        packet type.
 * @return      On success return 0, otherwise return a negative libfabric error code.
 */
ssize_t rxr_pkt_post_req(struct efa_rdm_ep *ep, struct efa_rdm_ope *ope, int req_type, uint64_t flags)
{
	assert(ope->type == EFA_RDM_TXE);
	assert(req_type >= RXR_REQ_PKT_BEGIN);

	if (rxr_pkt_type_is_mulreq(req_type)) {
		return rxr_pkt_post_or_queue(ep, ope, req_type);
	}

	return rxr_pkt_post(ep, ope, req_type, flags);
}

/*
 * This function is used for any extra feature that does not have an
 * alternative.
 *
 * This function will send a eager rtw packet to trigger handshake.
 *
 * We do not send eager rtm packets here because the receiver might require
 * ordering and an extra eager rtm will interrupt the reorder
 * process.
 *
 * ep: The endpoint on which the packet for triggering handshake will be sent.
 * peer: The peer from which the sender receives handshake.
 * addr: The address of the peer.
 *
 * This function will return 0 if the eager rtw packet is successfully sent.
 */
ssize_t rxr_pkt_trigger_handshake(struct efa_rdm_ep *ep,
				  fi_addr_t addr, struct efa_rdm_peer *peer)
{
	struct efa_rdm_ope *txe;
	ssize_t err;

	if ((peer->flags & EFA_RDM_PEER_HANDSHAKE_RECEIVED) ||
	    (peer->flags & EFA_RDM_PEER_REQ_SENT))
		return 0;

	/* TODO: use efa_rdm_ep_alloc_txe to allocate txe */
	txe = ofi_buf_alloc(ep->ope_pool);
	if (OFI_UNLIKELY(!txe)) {
		EFA_WARN(FI_LOG_EP_CTRL, "TX entries exhausted.\n");
		return -FI_EAGAIN;
	}

	txe->ep = ep;
	txe->total_len = 0;
	txe->addr = addr;
	txe->peer = efa_rdm_ep_get_peer(ep, txe->addr);
	assert(txe->peer);
	dlist_insert_tail(&txe->peer_entry, &txe->peer->txe_list);
	txe->msg_id = -1;
	txe->cq_entry.flags = FI_RMA | FI_WRITE;
	txe->cq_entry.buf = NULL;
	dlist_init(&txe->queued_pkts);

	txe->type = EFA_RDM_TXE;
	txe->op = ofi_op_write;
	txe->state = EFA_RDM_TXE_REQ;

	txe->bytes_acked = 0;
	txe->bytes_sent = 0;
	txe->window = 0;
	txe->rma_iov_count = 0;
	txe->iov_count = 0;
	txe->fi_flags = EFA_RDM_TXE_NO_COMPLETION | EFA_RDM_TXE_NO_COUNTER;
	txe->rxr_flags = 0;

	dlist_insert_tail(&txe->ep_entry, &ep->txe_list);

	err = rxr_pkt_post(ep, txe, RXR_EAGER_RTW_PKT, 0);

	if (OFI_UNLIKELY(err))
		return err;

	return 0;
}

void rxr_pkt_handle_data_copied(struct efa_rdm_ep *ep,
				struct rxr_pkt_entry *pkt_entry,
				size_t data_size)
{
	struct efa_rdm_ope *ope;

	ope = pkt_entry->ope;
	assert(ope);
	ope->bytes_copied += data_size;

	rxr_pkt_entry_release_rx(ep, pkt_entry);

	if (ope->total_len == ope->bytes_copied) {
		if (ope->cuda_copy_method == EFA_RDM_CUDA_COPY_BLOCKING) {
			assert(ep->blocking_copy_rxe_num > 0);
			ope->cuda_copy_method = EFA_RDM_CUDA_COPY_UNSPEC;
			ep->blocking_copy_rxe_num -= 1;
		}

		efa_rdm_ope_handle_recv_completed(ope);
	}
}

/**
 * @brief handle the a packet that encountered error completion while sending
 *
 * Depend on the packet type and error type, the error are handled differently.
 *
 * If the packet is associated with an user initialized TX operation:
 * (TX means send,read or write; such packets include all REQ packets and DATA):
 *
 *    If the error is Receiver Not Ready (RNR). there are two cases:
 *
 *         If user wants to manager RNR by itself (FI_RM_DISABLED),
 *         an error CQ entry will be written.
 *
 *         Otherwise, the packet will be queued and resent by progress engine.
 *
 *    For other type of error, an error CQ entry is written.
 *
 * If the packet is associated with an user initialized recv operiaton,
 * (such packets include EOR, CTS):
 *
 *      If the error is RNR, the packet is queued and resent by progress
 *      engine. No CQ entry is written.
 *
 *      For other types of error, an error CQ entry is written.
 *
 * If the packet is not associated with a user operation (such packet include
 * HANDSHAKE):
 *
 *      If the error is RNR, the packet is queued and resent by progress engine.
 *
 *      For other types of error, an error EQ entry is written.
 *
 * @param[in]	ep		endpoint
 * @param[in]	pkt_entry	pkt entry
 * @param[in]	err		libfabric error code
 * @param[in]	prov_errno	provider specific error code
 */
void rxr_pkt_handle_send_error(struct efa_rdm_ep *ep, struct rxr_pkt_entry *pkt_entry, int err, int prov_errno)
{
	struct efa_rdm_peer *peer;
	struct efa_rdm_ope *txe;
	struct efa_rdm_ope *rxe;

	assert(pkt_entry->alloc_type == RXR_PKT_FROM_EFA_TX_POOL);

	EFA_DBG(FI_LOG_CQ, "Packet send error: %s (%d)\n",
	        efa_strerror(prov_errno, NULL), prov_errno);

	efa_rdm_ep_record_tx_op_completed(ep, pkt_entry);

	peer = efa_rdm_ep_get_peer(ep, pkt_entry->addr);
	if (!peer) {
		/*
		 * If peer is NULL, it means the peer has been removed from AV.
		 * In this case, ignore this error completion.
		 */
		EFA_WARN(FI_LOG_CQ, "ignoring send error completion of a packet to a removed peer.\n");
		rxr_pkt_entry_release_tx(ep, pkt_entry);
		return;
	}

	if (!pkt_entry->ope) {
		/* only handshake packet is not associated with any TX/RX operation */
		assert(rxr_get_base_hdr(pkt_entry->wiredata)->type == RXR_HANDSHAKE_PKT);
		rxr_pkt_entry_release_tx(ep, pkt_entry);
		if (prov_errno == FI_EFA_REMOTE_ERROR_RNR) {
			/*
			 * handshake should always be queued for RNR
			 */
			assert(!(peer->flags & EFA_RDM_PEER_HANDSHAKE_QUEUED));
			peer->flags |= EFA_RDM_PEER_HANDSHAKE_QUEUED;
			dlist_insert_tail(&peer->handshake_queued_entry,
					  &ep->handshake_queued_peer_list);
		} else if (prov_errno != FI_EFA_REMOTE_ERROR_BAD_DEST_QPN) {
			/* If prov_errno is FI_EFA_REMOTE_ERROR_BAD_DEST_QPN  the peer has
			 * been destroyed. Which is normal, as peer does not always need a
			 * handshake packet to perform its duty. (For example, if a peer
			 * just want to sent 1 message to the ep, it does not need
			 * handshake.) In this case, it is safe to ignore this error
			 * completion. In all other cases, we write an eq entry because
			 * there is no application operation associated with handshake.
			 */
			char ep_addr_str[OFI_ADDRSTRLEN], peer_addr_str[OFI_ADDRSTRLEN];
			size_t buflen=0;

			memset(&ep_addr_str, 0, sizeof(ep_addr_str));
			memset(&peer_addr_str, 0, sizeof(peer_addr_str));
			buflen = sizeof(ep_addr_str);
			efa_rdm_ep_raw_addr_str(ep, ep_addr_str, &buflen);
			buflen = sizeof(peer_addr_str);
			efa_rdm_ep_get_peer_raw_addr_str(ep, pkt_entry->addr, peer_addr_str, &buflen);
			EFA_WARN(FI_LOG_CQ,
				"While sending a handshake packet, an error occurred."
				"  Our address: %s, peer address: %s\n",
				ep_addr_str, peer_addr_str);
			efa_base_ep_write_eq_error(&ep->base_ep, err, prov_errno);
		}
		return;
	}

	switch (pkt_entry->ope->type) {
	case EFA_RDM_TXE:
		txe = pkt_entry->ope;
		if (prov_errno == FI_EFA_REMOTE_ERROR_RNR) {
			if (ep->handle_resource_management == FI_RM_DISABLED) {
				/*
				 * Write an error to the application for RNR when resource
				 * management is disabled.
				 *
				 * Note that a txe might send multiple packets, therefore
				 * might encounter RNR from device multiple times, but it
				 * should only write cq err entry once
				 */
				if (!(txe->rxr_flags & EFA_RDM_TXE_WRITTEN_RNR_CQ_ERR_ENTRY)) {
					txe->rxr_flags |= EFA_RDM_TXE_WRITTEN_RNR_CQ_ERR_ENTRY;
					efa_rdm_txe_handle_error(pkt_entry->ope, FI_ENORX, FI_EFA_REMOTE_ERROR_RNR);
				}

				rxr_pkt_entry_release_tx(ep, pkt_entry);
				if (!txe->efa_outstanding_tx_ops)
					efa_rdm_txe_release(txe);
			} else {
				/*
				 * This packet is associated with a send operation, (such
				 * packets include all REQ, DATA) thus shoud be queued for RNR
				 * only if application wants EFA to manager resource.
				 */
				efa_rdm_ep_queue_rnr_pkt(ep, &txe->queued_pkts, pkt_entry);
				if (!(txe->rxr_flags & EFA_RDM_OPE_QUEUED_RNR)) {
					txe->rxr_flags |= EFA_RDM_OPE_QUEUED_RNR;
					dlist_insert_tail(&txe->queued_rnr_entry,
							  &ep->ope_queued_rnr_list);
				}
			}
		} else {
			efa_rdm_txe_handle_error(pkt_entry->ope, err, prov_errno);
			rxr_pkt_entry_release_tx(ep, pkt_entry);
		}
		break;
	case EFA_RDM_RXE:
		rxe = pkt_entry->ope;
		if (prov_errno == FI_EFA_REMOTE_ERROR_RNR) {
			/*
			 * This packet is associated with a recv operation, (such packets
			 * include CTS and EOR) thus should always be queued for RNR. This
			 * is regardless value of ep->handle_resource_management, because
			 * resource management is only applied to send operation.
			 */
			efa_rdm_ep_queue_rnr_pkt(ep, &rxe->queued_pkts, pkt_entry);
			if (!(rxe->rxr_flags & EFA_RDM_OPE_QUEUED_RNR)) {
				rxe->rxr_flags |= EFA_RDM_OPE_QUEUED_RNR;
				dlist_insert_tail(&rxe->queued_rnr_entry,
						  &ep->ope_queued_rnr_list);
			}
		} else {
			efa_rdm_rxe_handle_error(pkt_entry->ope, err, prov_errno);
			rxr_pkt_entry_release_tx(ep, pkt_entry);
		}
		break;
	default:
		EFA_WARN(FI_LOG_CQ,
				"%s unknown x_entry type %d\n",
				__func__, pkt_entry->ope->type);
		assert(0 && "unknown x_entry state");
		efa_base_ep_write_eq_error(&ep->base_ep, err, prov_errno);
		rxr_pkt_entry_release_tx(ep, pkt_entry);
		break;
	}
}

void rxr_pkt_handle_send_completion(struct efa_rdm_ep *ep, struct rxr_pkt_entry *pkt_entry)
{
	/*
	 * For a send completion, pkt_entry->addr can be FI_ADDR_NOTAVAIL in 3 situations:
	 * 1. the pkt_entry is used for a local read operation
	 * 2. a new peer with same gid+qpn was inserted to av, thus the peer was removed from AV.
	 * 3. application removed the peer's address from av.
	 * In 1, we should proceed. For 2 and 3, the send completion should be ignored.
	 */
	if (pkt_entry->addr == FI_ADDR_NOTAVAIL &&
	    !(pkt_entry->flags & RXR_PKT_ENTRY_LOCAL_READ)) {
		EFA_WARN(FI_LOG_CQ, "ignoring send completion of a packet to a removed peer.\n");
		efa_rdm_ep_record_tx_op_completed(ep, pkt_entry);
		rxr_pkt_entry_release_tx(ep, pkt_entry);
		return;
	}

	switch (rxr_get_base_hdr(pkt_entry->wiredata)->type) {
	case RXR_HANDSHAKE_PKT:
		break;
	case RXR_CTS_PKT:
		break;
	case RXR_DATA_PKT:
		rxr_pkt_handle_data_send_completion(ep, pkt_entry);
		break;
	case RXR_READRSP_PKT:
		rxr_pkt_handle_readrsp_send_completion(ep, pkt_entry);
		break;
	case RXR_EOR_PKT:
		rxr_pkt_handle_eor_send_completion(ep, pkt_entry);
		break;
	case RXR_RMA_CONTEXT_PKT:
		rxr_pkt_handle_rma_completion(ep, pkt_entry);
		return;
	case RXR_ATOMRSP_PKT:
		rxr_pkt_handle_atomrsp_send_completion(ep, pkt_entry);
		break;
	case RXR_RECEIPT_PKT:
		rxr_pkt_handle_receipt_send_completion(ep, pkt_entry);
		break;
	case RXR_EAGER_MSGRTM_PKT:
	case RXR_EAGER_TAGRTM_PKT:
		rxr_pkt_handle_eager_rtm_send_completion(ep, pkt_entry);
		break;
	case RXR_MEDIUM_MSGRTM_PKT:
	case RXR_MEDIUM_TAGRTM_PKT:
		rxr_pkt_handle_medium_rtm_send_completion(ep, pkt_entry);
		break;
	case RXR_LONGCTS_MSGRTM_PKT:
	case RXR_LONGCTS_TAGRTM_PKT:
		rxr_pkt_handle_longcts_rtm_send_completion(ep, pkt_entry);
		break;
	case RXR_LONGREAD_MSGRTM_PKT:
	case RXR_LONGREAD_TAGRTM_PKT:
		rxr_pkt_handle_longread_rtm_send_completion(ep, pkt_entry);
		break;
	case RXR_RUNTREAD_MSGRTM_PKT:
	case RXR_RUNTREAD_TAGRTM_PKT:
		rxr_pkt_handle_runtread_rtm_send_completion(ep, pkt_entry);
		break;
	case RXR_EAGER_RTW_PKT:
		rxr_pkt_handle_eager_rtw_send_completion(ep, pkt_entry);
		break;
	case RXR_LONGCTS_RTW_PKT:
		rxr_pkt_handle_longcts_rtw_send_completion(ep, pkt_entry);
		break;
	case RXR_LONGREAD_RTW_PKT:
		rxr_pkt_handle_longread_rtw_send_completion(ep, pkt_entry);
		break;
	case RXR_SHORT_RTR_PKT:
	case RXR_LONGCTS_RTR_PKT:
		/* Unlike other protocol, for emulated read, txe
	 	 * is released in rxr_cq_complete_recv().
	         * Therefore there is nothing to be done here. */
		break;
	case RXR_WRITE_RTA_PKT:
		rxr_pkt_handle_write_rta_send_completion(ep, pkt_entry);
		break;
	case RXR_FETCH_RTA_PKT:
		/* no action to be taken here */
		break;
	case RXR_COMPARE_RTA_PKT:
		/* no action to be taken here */
		break;
	case RXR_DC_EAGER_MSGRTM_PKT:
	case RXR_DC_EAGER_TAGRTM_PKT:
	case RXR_DC_MEDIUM_MSGRTM_PKT:
	case RXR_DC_MEDIUM_TAGRTM_PKT:
	case RXR_DC_EAGER_RTW_PKT:
	case RXR_DC_WRITE_RTA_PKT:
	case RXR_DC_LONGCTS_MSGRTM_PKT:
	case RXR_DC_LONGCTS_TAGRTM_PKT:
	case RXR_DC_LONGCTS_RTW_PKT:
		/* no action to be taken here
		 * For non-dc version of these packet types,
		 * this is the place to increase bytes_acked or
		 * write tx completion.
		 * For dc, tx completion will always be
		 * written upon receving the receipt packet
		 * Moreoever, because receipt can arrive
		 * before send completion, we cannot take
		 * any action on txe here.
		 */
		break;
	default:
		EFA_WARN(FI_LOG_CQ,
			"invalid control pkt type %d\n",
			rxr_get_base_hdr(pkt_entry->wiredata)->type);
		assert(0 && "invalid control pkt type");
		efa_base_ep_write_eq_error(&ep->base_ep, FI_EIO, FI_EFA_ERR_INVALID_PKT_TYPE);
		return;
	}

	efa_rdm_ep_record_tx_op_completed(ep, pkt_entry);
	rxr_pkt_entry_release_tx(ep, pkt_entry);
}

/**
 * @brief handle the a packet that encountered error completion while receiving
 *
 * This function will write error cq or eq entry, then release the packet entry.
 *
 * @param[in]	ep		endpoint
 * @param[in]	pkt_entry	pkt entry
 * @param[in]	err		libfabric error code
 * @param[in]	prov_errno	provider specific error code
 */
void rxr_pkt_handle_recv_error(struct efa_rdm_ep *ep, struct rxr_pkt_entry *pkt_entry, int err, int prov_errno)
{
	EFA_DBG(FI_LOG_CQ, "Packet receive error: %s (%d)\n",
	        efa_strerror(prov_errno, NULL), prov_errno);

	if (!pkt_entry->ope) {
		char ep_addr_str[OFI_ADDRSTRLEN];
		size_t buflen=0;

		memset(&ep_addr_str, 0, sizeof(ep_addr_str));
		buflen = sizeof(ep_addr_str);
		efa_rdm_ep_raw_addr_str(ep, ep_addr_str, &buflen);
		EFA_WARN(FI_LOG_CQ,
			"Packet receive error from non TX/RX packet.  Our address: %s\n",
			ep_addr_str);

		efa_base_ep_write_eq_error(&ep->base_ep, err, prov_errno);
		rxr_pkt_entry_release_rx(ep, pkt_entry);
		return;
	}

	if (pkt_entry->ope->type == EFA_RDM_TXE) {
		efa_rdm_txe_handle_error(pkt_entry->ope, err, prov_errno);
	} else if (pkt_entry->ope->type == EFA_RDM_RXE) {
		efa_rdm_rxe_handle_error(pkt_entry->ope, err, prov_errno);
	} else {
		EFA_WARN(FI_LOG_CQ,
		"%s unknown x_entry type %d\n",
			__func__, pkt_entry->ope->type);
		assert(0 && "unknown x_entry state");
		efa_base_ep_write_eq_error(&ep->base_ep, err, prov_errno);
	}

	rxr_pkt_entry_release_rx(ep, pkt_entry);
}

static
fi_addr_t rxr_pkt_insert_addr(struct efa_rdm_ep *ep, struct rxr_pkt_entry *pkt_entry, void *raw_addr)
{
	int ret;
	fi_addr_t rdm_addr;
	struct rxr_base_hdr *base_hdr;

	base_hdr = rxr_get_base_hdr(pkt_entry->wiredata);
	if (base_hdr->version < RXR_PROTOCOL_VERSION) {
		char self_raw_addr_str[OFI_ADDRSTRLEN];
		size_t buflen = OFI_ADDRSTRLEN;

		efa_rdm_ep_raw_addr_str(ep, self_raw_addr_str, &buflen);
		EFA_WARN(FI_LOG_CQ,
			"Host %s received a packet with invalid protocol version %d.\n"
			"This host can only support protocol version %d and above.\n",
			self_raw_addr_str, base_hdr->version, RXR_PROTOCOL_VERSION);
		efa_base_ep_write_eq_error(&ep->base_ep, FI_EIO, FI_EFA_ERR_INVALID_PKT_TYPE);
		fprintf(stderr, "Host %s received a packet with invalid protocol version %d.\n"
			"This host can only support protocol version %d and above. %s:%d\n",
			self_raw_addr_str, base_hdr->version, RXR_PROTOCOL_VERSION, __FILE__, __LINE__);
		abort();
	}

	assert(base_hdr->type >= RXR_REQ_PKT_BEGIN);

	ret = efa_av_insert_one(ep->base_ep.av, (struct efa_ep_addr *)raw_addr,
	                        &rdm_addr, 0, NULL);
	if (OFI_UNLIKELY(ret != 0)) {
		efa_base_ep_write_eq_error(&ep->base_ep, FI_EINVAL, FI_EFA_ERR_AV_INSERT);
		return -1;
	}

	return rdm_addr;
}

/**
 * @brief process a received packet
 *
 * @param[in]	ep		endpoint
 * @param[in]	pkt_entry	received packet entry
 */
void rxr_pkt_proc_received(struct efa_rdm_ep *ep, struct rxr_pkt_entry *pkt_entry)
{
	struct rxr_base_hdr *base_hdr;

	base_hdr = rxr_get_base_hdr(pkt_entry->wiredata);
	switch (base_hdr->type) {
	case RXR_RETIRED_RTS_PKT:
		EFA_WARN(FI_LOG_CQ,
			"Received a RTS packet, which has been retired since protocol version 4\n");
		assert(0 && "deprecated RTS pakcet received");
		efa_base_ep_write_eq_error(&ep->base_ep, FI_EIO, FI_EFA_ERR_DEPRECATED_PKT_TYPE);
		rxr_pkt_entry_release_rx(ep, pkt_entry);
		return;
	case RXR_RETIRED_CONNACK_PKT:
		EFA_WARN(FI_LOG_CQ,
			"Received a CONNACK packet, which has been retired since protocol version 4\n");
		assert(0 && "deprecated CONNACK pakcet received");
		efa_base_ep_write_eq_error(&ep->base_ep, FI_EIO, FI_EFA_ERR_DEPRECATED_PKT_TYPE);
		rxr_pkt_entry_release_rx(ep, pkt_entry);
		return;
	case RXR_EOR_PKT:
		rxr_pkt_handle_eor_recv(ep, pkt_entry);
		return;
	case RXR_HANDSHAKE_PKT:
		rxr_pkt_handle_handshake_recv(ep, pkt_entry);
		return;
	case RXR_CTS_PKT:
		rxr_pkt_handle_cts_recv(ep, pkt_entry);
		return;
	case RXR_DATA_PKT:
		rxr_pkt_handle_data_recv(ep, pkt_entry);
		return;
	case RXR_READRSP_PKT:
		rxr_pkt_handle_readrsp_recv(ep, pkt_entry);
		return;
	case RXR_ATOMRSP_PKT:
		rxr_pkt_handle_atomrsp_recv(ep, pkt_entry);
		return;
	case RXR_RECEIPT_PKT:
		rxr_pkt_handle_receipt_recv(ep, pkt_entry);
		return;
	case RXR_EAGER_MSGRTM_PKT:
	case RXR_EAGER_TAGRTM_PKT:
	case RXR_DC_EAGER_MSGRTM_PKT:
	case RXR_DC_EAGER_TAGRTM_PKT:
	case RXR_MEDIUM_MSGRTM_PKT:
	case RXR_MEDIUM_TAGRTM_PKT:
	case RXR_DC_MEDIUM_MSGRTM_PKT:
	case RXR_DC_MEDIUM_TAGRTM_PKT:
	case RXR_LONGCTS_MSGRTM_PKT:
	case RXR_LONGCTS_TAGRTM_PKT:
	case RXR_DC_LONGCTS_MSGRTM_PKT:
	case RXR_DC_LONGCTS_TAGRTM_PKT:
	case RXR_LONGREAD_MSGRTM_PKT:
	case RXR_LONGREAD_TAGRTM_PKT:
	case RXR_RUNTREAD_MSGRTM_PKT:
	case RXR_RUNTREAD_TAGRTM_PKT:
	case RXR_WRITE_RTA_PKT:
	case RXR_DC_WRITE_RTA_PKT:
	case RXR_FETCH_RTA_PKT:
	case RXR_COMPARE_RTA_PKT:
		rxr_pkt_handle_rtm_rta_recv(ep, pkt_entry);
		return;
	case RXR_EAGER_RTW_PKT:
		rxr_pkt_handle_eager_rtw_recv(ep, pkt_entry);
		return;
	case RXR_LONGCTS_RTW_PKT:
	case RXR_DC_LONGCTS_RTW_PKT:
		rxr_pkt_handle_longcts_rtw_recv(ep, pkt_entry);
		return;
	case RXR_LONGREAD_RTW_PKT:
		rxr_pkt_handle_longread_rtw_recv(ep, pkt_entry);
		return;
	case RXR_SHORT_RTR_PKT:
	case RXR_LONGCTS_RTR_PKT:
		rxr_pkt_handle_rtr_recv(ep, pkt_entry);
		return;
	case RXR_DC_EAGER_RTW_PKT:
		rxr_pkt_handle_dc_eager_rtw_recv(ep, pkt_entry);
		return;
	default:
		EFA_WARN(FI_LOG_CQ,
			"invalid control pkt type %d\n",
			rxr_get_base_hdr(pkt_entry->wiredata)->type);
		assert(0 && "invalid control pkt type");
		efa_base_ep_write_eq_error(&ep->base_ep, FI_EIO, FI_EFA_ERR_INVALID_PKT_TYPE);
		rxr_pkt_entry_release_rx(ep, pkt_entry);
		return;
	}
}

/**
 * @brief Read peer raw address from packet header and insert the peer in AV.
 * @param ep Pointer to RDM endpoint
 * @param pkt_entry Pointer to packet entry
 * @returns Peer address, or FI_ADDR_NOTAVIL if the packet header does not include raw address
 */
fi_addr_t rxr_pkt_determine_addr(struct efa_rdm_ep *ep, struct rxr_pkt_entry *pkt_entry)
{
	struct rxr_base_hdr *base_hdr;

	base_hdr = rxr_get_base_hdr(pkt_entry->wiredata);
	if (base_hdr->type >= RXR_REQ_PKT_BEGIN && rxr_pkt_req_raw_addr(pkt_entry)) {
		void *raw_addr;
		raw_addr = rxr_pkt_req_raw_addr(pkt_entry);
		assert(raw_addr);
		return rxr_pkt_insert_addr(ep, pkt_entry, raw_addr);
	}

	return FI_ADDR_NOTAVAIL;
}

/**
 * @brief handle a received packet
 *
 * @param	ep[in,out]		endpoint
 * @param	pkt_entry[in,out]	received packet, will be released by this function
 */
void rxr_pkt_handle_recv_completion(struct efa_rdm_ep *ep,
				    struct rxr_pkt_entry *pkt_entry)
{
	int pkt_type;
	struct efa_rdm_peer *peer;
	struct rxr_base_hdr *base_hdr;
	struct efa_rdm_ope *zcpy_rxe = NULL;

	base_hdr = rxr_get_base_hdr(pkt_entry->wiredata);
	pkt_type = base_hdr->type;
	if (pkt_type >= RXR_EXTRA_REQ_PKT_END) {
		EFA_WARN(FI_LOG_CQ,
			"Peer %d is requesting feature %d, which this EP does not support.\n",
			(int)pkt_entry->addr, base_hdr->type);

		assert(0 && "invalid REQ packe type");
		efa_base_ep_write_eq_error(&ep->base_ep, FI_EIO, FI_EFA_ERR_INVALID_PKT_TYPE);
		rxr_pkt_entry_release_rx(ep, pkt_entry);
		return;
	}

	/*
	 * Ignore packet if peer address cannot be determined. This ususally happens if
	 * we had prior communication with the peer, but
	 * application called fi_av_remove() to remove the address
	 * from address vector.
	 */
	if (pkt_entry->addr == FI_ADDR_NOTAVAIL) {
		EFA_WARN(FI_LOG_CQ,
			"Warning: ignoring a received packet from a removed address. packet type: %" PRIu8
			", packet flags: %x\n",
			rxr_get_base_hdr(pkt_entry->wiredata)->type,
			rxr_get_base_hdr(pkt_entry->wiredata)->flags);
		rxr_pkt_entry_release_rx(ep, pkt_entry);
		return;
	}

#if ENABLE_DEBUG
	if (!ep->use_zcpy_rx) {
		dlist_remove(&pkt_entry->dbg_entry);
		dlist_insert_tail(&pkt_entry->dbg_entry, &ep->rx_pkt_list);
	}
#ifdef ENABLE_RXR_PKT_DUMP
	rxr_pkt_print("Received", ep, (struct rxr_base_hdr *)pkt_entry->wiredata);
#endif
#endif
	peer = efa_rdm_ep_get_peer(ep, pkt_entry->addr);
	assert(peer);
	if (peer->is_local) {
		/*
		 * This happens when the peer is on same instance, but chose to
		 * use EFA device to communicate with me. In this case, we respect
		 * that and will not use shm with the peer.
		 * TODO: decide whether to use shm through handshake packet.
		 */
		peer->is_local = 0;
	}

	rxr_pkt_post_handshake_or_queue(ep, peer);

	ep->efa_rx_pkts_posted--;

	if (pkt_entry->alloc_type == RXR_PKT_FROM_USER_BUFFER) {
		assert(pkt_entry->ope);
		zcpy_rxe = pkt_entry->ope;
	}

	rxr_pkt_proc_received(ep, pkt_entry);

	if (zcpy_rxe && pkt_type != RXR_EAGER_MSGRTM_PKT) {
		/* user buffer was not matched with a message,
		 * therefore reposting the buffer */
		efa_rdm_ep_post_user_recv_buf(ep, zcpy_rxe, 0);
	}
}

#if ENABLE_DEBUG

/*
 *  Functions used to dump packets
 */

#define RXR_PKT_DUMP_DATA_LEN 64

static
void rxr_pkt_print_handshake(char *prefix,
			     struct rxr_handshake_hdr *handshake_hdr)
{
	EFA_DBG(FI_LOG_EP_DATA,
	       "%s RxR HANDSHAKE packet - version: %" PRIu8
	       " flags: %x\n", prefix, handshake_hdr->version,
	       handshake_hdr->flags);

	EFA_DBG(FI_LOG_EP_DATA,
	       "%s RxR HANDSHAKE packet, nextra_p3: %d\n",
	       prefix, handshake_hdr->nextra_p3);
}

static
void rxr_pkt_print_cts(char *prefix, struct rxr_cts_hdr *cts_hdr)
{
	EFA_DBG(FI_LOG_EP_DATA,
	       "%s RxR CTS packet - version: %"	PRIu8
	       " flags: %x tx_id: %" PRIu32
	       " rx_id: %"	   PRIu32
	       " window: %"	   PRIu64
	       "\n", prefix, cts_hdr->version, cts_hdr->flags,
	       cts_hdr->send_id, cts_hdr->recv_id, cts_hdr->recv_length);
}

static
void rxr_pkt_print_data(char *prefix, struct rxr_pkt_entry *pkt_entry)
{
	struct rxr_data_hdr *data_hdr;
	char str[RXR_PKT_DUMP_DATA_LEN * 4];
	size_t str_len = RXR_PKT_DUMP_DATA_LEN * 4, l, hdr_size;
	uint8_t *data;
	int i;

	str[str_len - 1] = '\0';

	data_hdr = rxr_get_data_hdr(pkt_entry->wiredata);

	EFA_DBG(FI_LOG_EP_DATA,
	       "%s RxR DATA packet -  version: %" PRIu8
	       " flags: %x rx_id: %" PRIu32
	       " seg_size: %"	     PRIu64
	       " seg_offset: %"	     PRIu64
	       "\n", prefix, data_hdr->version, data_hdr->flags,
	       data_hdr->recv_id, data_hdr->seg_length,
	       data_hdr->seg_offset);

	hdr_size = sizeof(struct rxr_data_hdr);
	if (data_hdr->flags & RXR_PKT_CONNID_HDR) {
		hdr_size += sizeof(struct rxr_data_opt_connid_hdr);
		EFA_DBG(FI_LOG_EP_DATA,
		       "sender_connid: %d\n",
		       data_hdr->connid_hdr->connid);
	}

	data = (uint8_t *)pkt_entry->wiredata + hdr_size;

	l = snprintf(str, str_len, ("\tdata:    "));
	for (i = 0; i < MIN(data_hdr->seg_length, RXR_PKT_DUMP_DATA_LEN);
	     i++)
		l += snprintf(str + l, str_len - l, "%02x ",
			      data[i]);
	EFA_DBG(FI_LOG_EP_DATA, "%s\n", str);
}

void rxr_pkt_print(char *prefix, struct efa_rdm_ep *ep, struct rxr_pkt_entry *pkt_entry)
{
	struct rxr_base_hdr *hdr;

	hdr = rxr_get_base_hdr(pkt_entry->wiredata);

	switch (hdr->type) {
	case RXR_HANDSHAKE_PKT:
		rxr_pkt_print_handshake(prefix, rxr_get_handshake_hdr(pkt_entry->wiredata));
		break;
	case RXR_CTS_PKT:
		rxr_pkt_print_cts(prefix, rxr_get_cts_hdr(pkt_entry->wiredata));
		break;
	case RXR_DATA_PKT:
		rxr_pkt_print_data(prefix, pkt_entry);
		break;
	default:
		EFA_WARN(FI_LOG_CQ, "invalid ctl pkt type %d\n",
			rxr_get_base_hdr(hdr)->type);
		assert(0);
		return;
	}
}
#endif

