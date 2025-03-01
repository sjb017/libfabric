/*
 * Copyright (c) 2019-2023 Amazon.com, Inc. or its affiliates.
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

#ifndef _EFA_RDM_OPE_H
#define _EFA_RDM_OPE_H

#include "rxr_pkt_entry.h"

#define RXR_IOV_LIMIT		(4)

/**
 * @brief EFA RDM operation entry (ope) type
 */
enum efa_rdm_ope_type {
	EFA_RDM_TXE = 1, /**< this ope is for an TX operation */
	EFA_RDM_RXE,     /**< this ope is for an RX operation */
};

/**
 * @brief EFA RDM operation entry (ope)'s state
 */
enum efa_rdm_ope_state {
	EFA_RDM_OPE_FREE = 0,	/**< txe/rxe free state */
	EFA_RDM_TXE_REQ,	/**< txe sending REQ packet */
	EFA_RDM_TXE_SEND,	/**< txe sending data in progress */
	EFA_RDM_RXE_INIT,	/**< rxe ready to recv RTM */
	EFA_RDM_RXE_UNEXP,	/**< rxe unexp msg waiting for post recv */
	EFA_RDM_RXE_MATCHED,	/**< rxe matched with RTM */
	EFA_RDM_RXE_RECV,	/**< rxe large msg recv data pkts */
};

/**
 * @brief basic information of an atomic operation
 * used by all 3 types of atomic operations: fetch, compare and write  
 */
struct efa_rdm_atomic_hdr {
	/* atomic_op is different from tx_op */
	uint32_t atomic_op;
	uint32_t datatype;
};

/**
 * @brief extra information that is not included in fi_msg_atomic
 * used by fetch atomic and compare atomic.
 *     resp stands for response
 *     comp stands for compare
 */
struct efa_rdm_atomic_ex {
	struct iovec resp_iov[RXR_IOV_LIMIT];
	int resp_iov_count;
	struct iovec comp_iov[RXR_IOV_LIMIT];
	int comp_iov_count;
	void *result_desc[RXR_IOV_LIMIT];
	/* compare_desc does not require persistence b/c it is only used to send the RTA */
	void **compare_desc;
};

/**
 * @brief how to copy data from bounce buffer to CUDA receive buffer
 */
enum efa_rdm_cuda_copy_method {
	EFA_RDM_CUDA_COPY_UNSPEC = 0,
	EFA_RDM_CUDA_COPY_BLOCKING,   /** gdrcopy or cudaMemcpy */
	EFA_RDM_CUDA_COPY_LOCALREAD   /** device driven copy by using local RDMA read */
};

/**
 * @brief EFA RDM operation entry (ope)
 * 
 */
struct efa_rdm_ope {
	enum efa_rdm_ope_type type;

	struct efa_rdm_ep *ep;
	fi_addr_t addr;
	struct efa_rdm_peer *peer;

	uint32_t tx_id;
	uint32_t rx_id;
	uint32_t op;

	struct efa_rdm_atomic_hdr atomic_hdr;
	struct efa_rdm_atomic_ex atomic_ex;

	uint32_t msg_id;

	uint64_t tag;
	uint64_t ignore;

	int64_t window;

	uint64_t total_len;

	enum efa_rdm_ope_state state;
	int queued_ctrl_type;

	uint64_t fi_flags;
	uint16_t rxr_flags;

	size_t iov_count;
	struct iovec iov[RXR_IOV_LIMIT];
	void *desc[RXR_IOV_LIMIT];
	void *shm_desc[RXR_IOV_LIMIT];
	struct fid_mr *mr[RXR_IOV_LIMIT];

	size_t rma_iov_count;
	struct fi_rma_iov rma_iov[RXR_IOV_LIMIT];

	struct fi_cq_tagged_entry cq_entry;

	/* For txe, entry is linked with tx_pending_list in efa_rdm_ep.
	 * For rxe, entry is linked with one of the receive lists: rx_list, rx_tagged_list,
	 * rx_unexp_list and rxr_unexp_tagged_list in efa_rdm_ep.
	 */
	struct dlist_entry entry;

	/* ep_entry is linked to tx/rxe_list in efa_rdm_ep */
	struct dlist_entry ep_entry;

	/* queued_ctrl_entry is linked with tx/rx_queued_ctrl_list in efa_rdm_ep */
	struct dlist_entry queued_ctrl_entry;

	/* queued_read_entry is linked with ope_queued_read_list in efa_rdm_ep */
	struct dlist_entry queued_read_entry;

	/* queued_rnr_entry is linked with tx/rx_queued_rnr_list in efa_rdm_ep */
	struct dlist_entry queued_rnr_entry;

	/* Queued packets due to TX queue full or RNR backoff */
	struct dlist_entry queued_pkts;


	/* linked with tx/rxe_list in rdm_peer */
	struct dlist_entry peer_entry;

	uint64_t bytes_runt;

	/* the following variables are for RX operation only */
	uint64_t bytes_received;
	uint64_t bytes_received_via_mulreq;
	uint64_t bytes_copied;
	uint64_t bytes_queued_blocking_copy;

	/* linked to peer->rx_unexp_list or peer->rx_unexp_tagged_list */
	struct dlist_entry peer_unexp_entry;
#if ENABLE_DEBUG
	/* linked with ope_recv_list in efa_rdm_ep */
	struct dlist_entry pending_recv_entry;
#endif

	size_t efa_outstanding_tx_ops;

	/*
	 * A list of rx_entries tracking FI_MULTI_RECV buffers. An rxe of
	 * type EFA_RDM_RXE_MULTI_RECV_POSTED that was created when the multi-recv
	 * buffer was posted is the list head, and the rx_entries of type
	 * EFA_RDM_RXE_MULTI_RECV_CONSUMER get added to the list as they consume the
	 * buffer.
	 */
	struct dlist_entry multi_recv_consumers;
	struct dlist_entry multi_recv_entry;
	struct efa_rdm_ope *master_entry;
	struct fi_msg *posted_recv;
	struct rxr_pkt_entry *unexp_pkt;
	char *atomrsp_data;
	enum efa_rdm_cuda_copy_method cuda_copy_method;
	/* end of RX related variables */
	/* the following variables are for TX operation only */
	uint64_t bytes_acked;
	uint64_t bytes_sent;
	uint64_t max_req_data_size;
	/* end of TX only variables */

	uint64_t bytes_read_completed;
	uint64_t bytes_read_submitted;
	uint64_t bytes_read_total_len;
	uint64_t bytes_read_offset;

	/* counters for rma writes */
	uint64_t bytes_write_completed;
	uint64_t bytes_write_submitted;
	uint64_t bytes_write_total_len;

	/* used by peer SRX ops */
	struct fi_peer_rx_entry peer_rxe;

	/** the source packet entry of a local read operation */
	struct rxr_pkt_entry *local_read_pkt_entry;
};

void efa_rdm_txe_construct(struct efa_rdm_ope *txe,
			    struct efa_rdm_ep *ep,
			    const struct fi_msg *msg,
			    uint32_t op, uint64_t flags);

void efa_rdm_txe_release(struct efa_rdm_ope *txe);

void efa_rdm_rxe_release(struct efa_rdm_ope *rxe);

/* The follow flags are applied to the rxr_flags field
 * of an efa_rdm_ope*/

/**
 * @brief indicate an ope's receive has been cancel
 * 
 * @todo: In future we will send RECV_CANCEL signal to sender,
 * to stop transmitting large message, this flag is also
 * used for fi_discard which has similar behavior.
 */
#define EFA_RDM_RXE_RECV_CANCEL		BIT_ULL(3)

/**
 * @brief Flags to tell if the rxe is tracking FI_MULTI_RECV buffers
 */
#define EFA_RDM_RXE_MULTI_RECV_POSTED		BIT_ULL(4)
#define EFA_RDM_RXE_MULTI_RECV_CONSUMER	BIT_ULL(5)

/**
 * @brief Flag to tell if the transmission is using FI_DELIVERY_COMPLETE
 * protocols
 */
#define EFA_RDM_TXE_DELIVERY_COMPLETE_REQUESTED	BIT_ULL(6)

/**
 * @brief flag to tell if an ope encouter RNR when sending packets
 * 
 * If an ope has this flag, it is on the ope_queued_rnr_list
 * of the endpoint.
 */
#define EFA_RDM_OPE_QUEUED_RNR BIT_ULL(9)

/**
 * @brief Flag to indicate an rxe has an EOR in flight
 * 
 * In flag means the EOR has been sent or queued, and has not got send completion.
 * hence the rxe cannot be released
 */
#define EFA_RDM_RXE_EOR_IN_FLIGHT BIT_ULL(10)

/**
 * @brief flag to indicate a txe has already written an cq error entry for RNR
 * 
 * This flag is used to prevent writing multiple cq error entries
 * for the same txe
 */
#define EFA_RDM_TXE_WRITTEN_RNR_CQ_ERR_ENTRY BIT_ULL(10)

/**
 * @brief flag to indicate an ope has queued ctrl packet,
 *
 * If this flag is on, the op_entyr is on the ope_queued_ctrl_list
 * of the endpoint
 */
#define EFA_RDM_OPE_QUEUED_CTRL BIT_ULL(11)

/**
 * @brief flag to indicate an ope does not need to report completion to user
 * 
 * This flag is used to by emulated injection and #rxr_pkt_trigger_handshake
 */
#define EFA_RDM_TXE_NO_COMPLETION	BIT_ULL(60)
/**
 * @brief flag to indicate an ope does not need to increase counter
 * 
 * This flag is used to implement #rxr_pkt_trigger_handshake
 * 
 */
#define EFA_RDM_TXE_NO_COUNTER		BIT_ULL(61)

/**
 * @brief flag to indicate an ope has queued read requests
 *
 * When this flag is on, the ope is on ope_queued_read_list
 * of the endpoint
 */
#define EFA_RDM_OPE_QUEUED_READ 	BIT_ULL(12)

/**
 * @brief flag to indicate an rxe is shared to peer provider's receive context.
 *
 */
#define EFA_RDM_RXE_FOR_PEER_SRX 	BIT_ULL(13)

void efa_rdm_ope_try_fill_desc(struct efa_rdm_ope *ope, int mr_iov_start, uint64_t access);

int efa_rdm_txe_prepare_to_be_read(struct efa_rdm_ope *txe,
				    struct fi_rma_iov *read_iov);

struct efa_rdm_ep;

void efa_rdm_txe_set_runt_size(struct efa_rdm_ep *ep, struct efa_rdm_ope *txe);

size_t efa_rdm_ope_mulreq_total_data_size(struct efa_rdm_ope *ope, int pkt_type);

size_t efa_rdm_txe_max_req_data_capacity(struct efa_rdm_ep *ep, struct efa_rdm_ope *txe, int pkt_type);

void efa_rdm_txe_set_max_req_data_size(struct efa_rdm_ep *ep, struct efa_rdm_ope *txe, int pkt_type);

size_t efa_rdm_txe_num_req(struct efa_rdm_ope *txe, int pkt_type);

void efa_rdm_txe_handle_error(struct efa_rdm_ope *txe, int err, int prov_errno);

void efa_rdm_rxe_handle_error(struct efa_rdm_ope *rxe, int err, int prov_errno);

void efa_rdm_txe_report_completion(struct efa_rdm_ope *txe);

void efa_rdm_rxe_report_completion(struct efa_rdm_ope *rxe);

void efa_rdm_ope_handle_recv_completed(struct efa_rdm_ope *ope);

void efa_rdm_ope_handle_send_completed(struct efa_rdm_ope *ope);

int efa_rdm_ope_prepare_to_post_read(struct efa_rdm_ope *ope);

void efa_rdm_ope_prepare_to_post_write(struct efa_rdm_ope *ope);

int efa_rdm_ope_post_read(struct efa_rdm_ope *ope);

int efa_rdm_ope_post_remote_write(struct efa_rdm_ope *ope);

int efa_rdm_ope_post_remote_read_or_queue(struct efa_rdm_ope *ope);

int efa_rdm_rxe_post_local_read_or_queue(struct efa_rdm_ope *rxe,
					  size_t rx_data_offset,
					  struct rxr_pkt_entry *pkt_entry,
					  char *pkt_data, size_t data_size);

#endif
