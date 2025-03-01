/*
 * Copyright (c) 2013-2020 Intel Corporation. All rights reserved
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

#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include "ofi_iov.h"
#include "ofi_hmem.h"
#include "ofi_atom.h"
#include "ofi_mr.h"
#include "smr.h"
#include "smr_dsa.h"

static inline void
smr_try_progress_to_sar(struct smr_ep *ep, struct smr_region *smr,
                        struct smr_freestack *sar_pool, struct smr_resp *resp,
                        struct smr_cmd *cmd, struct ofi_mr **mr,
			struct iovec *iov, size_t iov_count,
                        size_t *bytes_done, int *next, void *entry_ptr)
{
	if (*bytes_done < cmd->msg.hdr.size) {
		if (smr_env.use_dsa_sar && !mr) {
			(void) smr_dsa_copy_to_sar(ep, sar_pool, resp, cmd, iov,
					    iov_count, bytes_done, entry_ptr);
			return;
		} else {
			smr_copy_to_sar(sar_pool, resp, cmd, mr, iov, iov_count,
					bytes_done, next);
		}
	}
	smr_signal(smr);
}

static inline void
smr_try_progress_from_sar(struct smr_ep *ep, struct smr_region *smr,
                          struct smr_freestack *sar_pool, struct smr_resp *resp,
                          struct smr_cmd *cmd, struct ofi_mr **mr,
			  struct iovec *iov, size_t iov_count,
                          size_t *bytes_done, int *next, void *entry_ptr)
{
	if (*bytes_done < cmd->msg.hdr.size) {
		if (smr_env.use_dsa_sar && !mr) {
			(void) smr_dsa_copy_from_sar(ep, sar_pool, resp, cmd, 
					iov, iov_count, bytes_done, entry_ptr);
			return;
		} else {
			smr_copy_from_sar(sar_pool, resp, cmd, mr,
					  iov, iov_count, bytes_done, next);
		}
	}
	smr_signal(smr);
}

static int smr_progress_resp_entry(struct smr_ep *ep, struct smr_resp *resp,
				   struct smr_tx_entry *pending, uint64_t *err)
{
	int i;
	struct smr_region *peer_smr;
	size_t inj_offset;
	struct smr_inject_buf *tx_buf = NULL;
	struct smr_sar_buf *sar_buf = NULL;
	uint8_t *src;
	ssize_t hmem_copy_ret;

	peer_smr = smr_peer_region(ep->region, pending->peer_id);

	switch (pending->cmd.msg.hdr.op_src) {
	case smr_src_iov:
		break;
	case smr_src_ipc:
		assert(pending->mr[0]);
		if (pending->mr[0]->iface == FI_HMEM_ZE)
			close(pending->fd);
		break;
	case smr_src_sar:
		sar_buf = smr_freestack_get_entry_from_index(
		    smr_sar_pool(peer_smr), pending->cmd.msg.data.sar[0]);
		if (pending->bytes_done == pending->cmd.msg.hdr.size &&
		    (resp->status == SMR_STATUS_SAR_FREE ||
		     resp->status == SMR_STATUS_SUCCESS)) {
			resp->status = SMR_STATUS_SUCCESS;
			break;
		}

		if (pending->cmd.msg.hdr.op == ofi_op_read_req)
			smr_try_progress_from_sar(ep, peer_smr,
					smr_sar_pool(peer_smr), resp,
					&pending->cmd, pending->mr, pending->iov,
				        pending->iov_count, &pending->bytes_done,
					&pending->next, pending);
		else
			smr_try_progress_to_sar(ep, peer_smr,
					smr_sar_pool(peer_smr), resp,
					&pending->cmd, pending->mr, pending->iov,
					pending->iov_count, &pending->bytes_done,
					&pending->next, pending);
		if (pending->bytes_done != pending->cmd.msg.hdr.size ||
		    resp->status != SMR_STATUS_SAR_FREE) {
			return -FI_EAGAIN;
		}

		resp->status = SMR_STATUS_SUCCESS;
		break;
	case smr_src_mmap:
		if (!pending->map_name)
			break;
		if (pending->cmd.msg.hdr.op == ofi_op_read_req) {
			if (!*err) {
				hmem_copy_ret =
					ofi_copy_to_mr_iov(pending->mr,
							   pending->iov,
							   pending->iov_count,
							   0, pending->map_ptr,
							   pending->cmd.msg.hdr.size);
				if (hmem_copy_ret < 0) {
					FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
						"Copy from mmapped file failed with code %d\n",
						(int)(-hmem_copy_ret));
					*err = hmem_copy_ret;
				} else if (hmem_copy_ret != pending->cmd.msg.hdr.size) {
					FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
						"Incomplete copy from mmapped file\n");
					*err = -FI_ETRUNC;
				} else {
					pending->bytes_done = (size_t) hmem_copy_ret;
				}
			}
			munmap(pending->map_ptr, pending->cmd.msg.hdr.size);
		}
		shm_unlink(pending->map_name->name);
		dlist_remove(&pending->map_name->entry);
		free(pending->map_name);
		pending->map_name = NULL;
		break;
	case smr_src_inject:
		inj_offset = (size_t) pending->cmd.msg.hdr.src_data;
		tx_buf = smr_get_ptr(peer_smr, inj_offset);
		if (*err || pending->bytes_done == pending->cmd.msg.hdr.size ||
		    pending->cmd.msg.hdr.op == ofi_op_atomic)
			break;

		src = pending->cmd.msg.hdr.op == ofi_op_atomic_compare ?
		      tx_buf->buf : tx_buf->data;
		hmem_copy_ret  = ofi_copy_to_mr_iov(pending->mr,
					pending->iov, pending->iov_count,
					0, src, pending->cmd.msg.hdr.size);

		if (hmem_copy_ret < 0) {
			FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
				"RMA read/fetch failed with code %d\n",
				(int)(-hmem_copy_ret));
			*err = hmem_copy_ret;
		} else if (hmem_copy_ret != pending->cmd.msg.hdr.size) {
			FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
				"Incomplete rma read/fetch buffer copied\n");
			*err = -FI_ETRUNC;
		} else {
			pending->bytes_done = (size_t) hmem_copy_ret;
		}
		break;
	default:
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"unidentified operation type\n");
	}

	if (tx_buf) {
		smr_release_txbuf(peer_smr, tx_buf);
	} else if (sar_buf) {
		pthread_spin_lock(&peer_smr->lock);
		for (i = pending->cmd.msg.data.buf_batch_size - 1; i >= 0; i--) {
			smr_freestack_push_by_index(smr_sar_pool(peer_smr),
					pending->cmd.msg.data.sar[i]);
		}
		pthread_spin_unlock(&peer_smr->lock);
		smr_peer_data(ep->region)[pending->peer_id].sar_status = 0;
	}

	return FI_SUCCESS;
}

static void smr_progress_resp(struct smr_ep *ep)
{
	struct smr_resp *resp;
	struct smr_tx_entry *pending;
	int ret;

	ofi_spin_lock(&ep->tx_lock);
	while (!ofi_cirque_isempty(smr_resp_queue(ep->region))) {
		resp = ofi_cirque_head(smr_resp_queue(ep->region));
		if (resp->status == SMR_STATUS_BUSY)
			break;

		pending = (struct smr_tx_entry *) resp->msg_id;
		if (smr_progress_resp_entry(ep, resp, pending, &resp->status))
			break;

		if (-resp->status) {
			ret = smr_write_err_comp(ep->util_ep.tx_cq, pending->context,
					 pending->op_flags, pending->cmd.msg.hdr.tag,
					 -(resp->status));
		} else {
			ret = smr_complete_tx(ep, pending->context,
					  pending->cmd.msg.hdr.op, pending->op_flags);
		}
		if (ret) {
			FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
				"unable to process tx completion\n");
			break;
		}
		ofi_freestack_push(ep->tx_fs, pending);
		ofi_cirque_discard(smr_resp_queue(ep->region));
	}
	ofi_spin_unlock(&ep->tx_lock);
}

static int smr_progress_inline(struct smr_cmd *cmd, struct ofi_mr **mr,
			       struct iovec *iov, size_t iov_count,
			       size_t *total_len)
{
	ssize_t hmem_copy_ret;

	hmem_copy_ret = ofi_copy_to_mr_iov(mr, iov, iov_count, 0,
				cmd->msg.data.msg, cmd->msg.hdr.size);
	if (hmem_copy_ret < 0) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"inline recv failed with code %d\n",
			(int)(-hmem_copy_ret));
		return hmem_copy_ret;
	} else if (hmem_copy_ret != cmd->msg.hdr.size) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"inline recv truncated\n");
		return -FI_ETRUNC;
	}

	*total_len = hmem_copy_ret;

	return FI_SUCCESS;
}

static int smr_progress_inject(struct smr_cmd *cmd, struct ofi_mr **mr,
			       struct iovec *iov, size_t iov_count,
			       size_t *total_len, struct smr_ep *ep, int err)
{
	struct smr_inject_buf *tx_buf;
	size_t inj_offset;
	ssize_t hmem_copy_ret;

	assert(cmd->msg.hdr.op != ofi_op_read_req);

	inj_offset = (size_t) cmd->msg.hdr.src_data;
	tx_buf = smr_get_ptr(ep->region, inj_offset);

	if (err) {
		smr_release_txbuf(ep->region, tx_buf);
		return err;
	}

	hmem_copy_ret = ofi_copy_to_mr_iov(mr, iov, iov_count, 0, tx_buf->data,
	                                   cmd->msg.hdr.size);
	smr_release_txbuf(ep->region, tx_buf);

	if (hmem_copy_ret < 0) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"inject recv failed with code %d\n",
			(int)(-hmem_copy_ret));
		return hmem_copy_ret;
	} else if (hmem_copy_ret != cmd->msg.hdr.size) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"inject recv truncated\n");
		return -FI_ETRUNC;
	}

	*total_len = hmem_copy_ret;

	return FI_SUCCESS;
}

static int smr_progress_iov(struct smr_cmd *cmd, struct iovec *iov,
			    size_t iov_count, size_t *total_len,
			    struct smr_ep *ep, int err)
{
	struct smr_region *peer_smr;
	struct smr_resp *resp;
	int ret;

	peer_smr = smr_peer_region(ep->region, cmd->msg.hdr.id);
	resp = smr_get_ptr(peer_smr, cmd->msg.hdr.src_data);

	if (err) {
		ret = -err;
		goto out;
	}

	ret = smr_cma_loop(peer_smr->pid, iov, iov_count, cmd->msg.data.iov,
			   cmd->msg.data.iov_count, 0, cmd->msg.hdr.size,
			   cmd->msg.hdr.op == ofi_op_read_req);
	if (!ret)
		*total_len = cmd->msg.hdr.size;

out:
	//Status must be set last (signals peer: op done, valid resp entry)
	resp->status = ret;
	smr_signal(peer_smr);

	return -ret;
}

static int smr_mmap_peer_copy(struct smr_ep *ep, struct smr_cmd *cmd,
			      struct ofi_mr **mr, struct iovec *iov,
			      size_t iov_count, size_t *total_len)
{
	char shm_name[SMR_NAME_MAX];
	void *mapped_ptr;
	int fd, num;
	int ret = 0;
	ssize_t hmem_copy_ret;

	num = smr_mmap_name(shm_name,
			ep->region->map->peers[cmd->msg.hdr.id].peer.name,
			cmd->msg.hdr.msg_id);
	if (num < 0) {
		FI_WARN(&smr_prov, FI_LOG_AV, "generating shm file name failed\n");
		return -errno;
	}

	fd = shm_open(shm_name, O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		FI_WARN(&smr_prov, FI_LOG_AV, "shm_open error\n");
		return -errno;
	}

	mapped_ptr = mmap(NULL, cmd->msg.hdr.size, PROT_READ | PROT_WRITE,
			  MAP_SHARED, fd, 0);
	if (mapped_ptr == MAP_FAILED) {
		FI_WARN(&smr_prov, FI_LOG_AV, "mmap error %s\n", strerror(errno));
		ret = -errno;
		goto unlink_close;
	}

	if (cmd->msg.hdr.op == ofi_op_read_req) {
		hmem_copy_ret = ofi_copy_from_mr_iov(mapped_ptr,
					cmd->msg.hdr.size, mr, iov,
					iov_count, 0);
	} else {
		hmem_copy_ret = ofi_copy_to_mr_iov(mr, iov, iov_count, 0,
					mapped_ptr, cmd->msg.hdr.size);
	}

	if (hmem_copy_ret < 0) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"mmap copy iov failed with code %d\n",
			(int)(-hmem_copy_ret));
		ret = hmem_copy_ret;
	} else if (hmem_copy_ret != cmd->msg.hdr.size) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"mmap copy iov truncated\n");
		ret = -FI_ETRUNC;
	}

	*total_len = hmem_copy_ret;

	munmap(mapped_ptr, cmd->msg.hdr.size);
unlink_close:
	shm_unlink(shm_name);
	close(fd);
	return ret;
}

static int smr_progress_mmap(struct smr_cmd *cmd, struct ofi_mr **mr,
			     struct iovec *iov, size_t iov_count,
			     size_t *total_len, struct smr_ep *ep)
{
	struct smr_region *peer_smr;
	struct smr_resp *resp;
	int ret;

	peer_smr = smr_peer_region(ep->region, cmd->msg.hdr.id);
	resp = smr_get_ptr(peer_smr, cmd->msg.hdr.src_data);

	ret = smr_mmap_peer_copy(ep, cmd, mr, iov, iov_count, total_len);

	//Status must be set last (signals peer: op done, valid resp entry)
	resp->status = ret;
	smr_signal(peer_smr);

	return ret;
}

static struct smr_pend_entry *smr_progress_sar(struct smr_cmd *cmd,
			struct fi_peer_rx_entry *rx_entry, struct ofi_mr **mr,
			struct iovec *iov, size_t iov_count,
			size_t *total_len, struct smr_ep *ep)
{
	struct smr_region *peer_smr;
	struct smr_pend_entry *sar_entry;
	struct smr_resp *resp;
	struct iovec sar_iov[SMR_IOV_LIMIT];
	int next = 0;

	peer_smr = smr_peer_region(ep->region, cmd->msg.hdr.id);
	resp = smr_get_ptr(peer_smr, cmd->msg.hdr.src_data);

	memcpy(sar_iov, iov, sizeof(*iov) * iov_count);
	(void) ofi_truncate_iov(sar_iov, &iov_count, cmd->msg.hdr.size);

	ofi_ep_lock_acquire(&ep->util_ep);
	sar_entry = ofi_freestack_pop(ep->pend_fs);
	sar_entry->in_use = true;
	dlist_insert_tail(&sar_entry->entry, &ep->sar_list);
	ofi_ep_lock_release(&ep->util_ep);

	if (cmd->msg.hdr.op == ofi_op_read_req)
		smr_try_progress_to_sar(ep, peer_smr, smr_sar_pool(ep->region),
				resp, cmd, mr, sar_iov, iov_count,
				total_len, &next, sar_entry);
	else
		smr_try_progress_from_sar(ep, peer_smr,
				smr_sar_pool(ep->region), resp, cmd, mr,
				sar_iov, iov_count, total_len, &next,
				sar_entry);
	ofi_ep_lock_acquire(&ep->util_ep);
	sar_entry->in_use = false;

	if (*total_len == cmd->msg.hdr.size) {
		dlist_remove(&sar_entry->entry);
		ofi_freestack_push(ep->pend_fs, sar_entry);
		ofi_ep_lock_release(&ep->util_ep);
		return NULL;
	}
	ofi_ep_lock_release(&ep->util_ep);
	sar_entry->cmd = *cmd;
	sar_entry->bytes_done = *total_len;
	sar_entry->next = next;
	memcpy(sar_entry->iov, sar_iov, sizeof(*sar_iov) * iov_count);
	sar_entry->iov_count = iov_count;
	sar_entry->rx_entry = rx_entry ? rx_entry : NULL;
	if (mr)
		memcpy(sar_entry->mr, mr, sizeof(*mr) * iov_count);
	else
		memset(sar_entry->mr, 0, sizeof(*mr) * iov_count);

	*total_len = cmd->msg.hdr.size;
	return sar_entry;
}

static int
smr_ipc_async_copy(struct smr_ep *ep, void *ptr,
		   struct fi_peer_rx_entry *rx_entry,
		   struct iovec *iov, size_t iov_count,
		   struct ofi_mr_entry *mr_entry, struct smr_cmd *cmd,
		   struct smr_pend_entry **pend)
{
	struct smr_pend_entry *ipc_entry;
	enum fi_hmem_iface iface = cmd->msg.data.ipc_info.iface;
	uint64_t device = cmd->msg.data.ipc_info.device;
	int ret;

	ofi_ep_lock_acquire(&ep->util_ep);
	ipc_entry = ofi_freestack_pop(ep->pend_fs);
	ofi_ep_lock_release(&ep->util_ep);
	if (!ipc_entry)
		return -FI_ENOMEM;

	ipc_entry->cmd = *cmd;
	ipc_entry->ipc_entry = mr_entry;
	ipc_entry->bytes_done = 0;
	memcpy(ipc_entry->iov, iov, sizeof(*iov) * iov_count);
	ipc_entry->iov_count = iov_count;
	ipc_entry->rx_entry = rx_entry;
	if (rx_entry) {
		ipc_entry->rx_entry->flags |= cmd->msg.hdr.op_flags;
		ipc_entry->rx_entry->flags &= ~SMR_MULTI_RECV;
	}

	ret = ofi_create_async_copy_event(iface, device,
					  &ipc_entry->async_event);
	if (ret < 0)
		goto fail;

	if (cmd->msg.hdr.op == ofi_op_read_req) {
		ret = ofi_async_copy_from_hmem_iov(ptr, cmd->msg.hdr.size,
						iface, device, iov, iov_count, 0,
						ipc_entry->async_event);
	} else {
		ret = ofi_async_copy_to_hmem_iov(iface, device, iov, iov_count, 0,
						ptr, cmd->msg.hdr.size,
						ipc_entry->async_event);
	}

	if (ret < 0)
		goto fail;

	ofi_ep_lock_acquire(&ep->util_ep);
	dlist_insert_tail(&ipc_entry->entry, &ep->ipc_cpy_pend_list);
	ofi_ep_lock_release(&ep->util_ep);

	*pend = ipc_entry;

	return FI_SUCCESS;

fail:
	ofi_ep_lock_acquire(&ep->util_ep);
	ofi_freestack_push(ep->pend_fs, ipc_entry);
	ofi_ep_lock_release(&ep->util_ep);
	return ret;
}

static struct smr_pend_entry *smr_progress_ipc(struct smr_cmd *cmd,
			struct fi_peer_rx_entry *rx_entry,
			struct ofi_mr **mr, struct iovec *iov,
			size_t iov_count, size_t *total_len,
			struct smr_ep *ep, int *err)
{
	struct smr_region *peer_smr;
	struct smr_resp *resp;
	void *base, *ptr;
	uint64_t ipc_device;
	int64_t id;
	int ret, fd, ipc_fd;
	ssize_t hmem_copy_ret;
	struct ofi_mr_entry *mr_entry;
	struct smr_domain *domain;
	struct smr_pend_entry *ipc_entry;

	domain = container_of(ep->util_ep.domain, struct smr_domain,
			      util_domain);

	peer_smr = smr_peer_region(ep->region, cmd->msg.hdr.id);
	resp = smr_get_ptr(peer_smr, cmd->msg.hdr.src_data);

	//TODO disable IPC if more than 1 interface is initialized
	if (cmd->msg.data.ipc_info.iface == FI_HMEM_ZE) {
		id = cmd->msg.hdr.id;
		ipc_device = cmd->msg.data.ipc_info.device;
		fd = ep->sock_info->peers[id].device_fds[ipc_device];
		ret = ze_hmem_open_shared_handle(fd,
				(void **) &cmd->msg.data.ipc_info.ipc_handle,
				&ipc_fd, ipc_device, &base);
	} else {
		ret = ofi_ipc_cache_search(domain->ipc_cache,
					   cmd->msg.hdr.id,
					   &cmd->msg.data.ipc_info,
					   &mr_entry);
	}
	if (ret)
		goto out;

	if (cmd->msg.data.ipc_info.iface == FI_HMEM_ZE)
		ptr = (char *) base + (uintptr_t) cmd->msg.data.ipc_info.offset;
	else
		ptr = (char *) (uintptr_t) mr_entry->info.ipc_mapped_addr +
		      (uintptr_t) cmd->msg.data.ipc_info.offset;

	if (cmd->msg.data.ipc_info.iface == FI_HMEM_ROCR) {
		*total_len = 0;
		ipc_entry = NULL;
		resp->status = SMR_STATUS_BUSY;

		ret = smr_ipc_async_copy(ep, (char*)ptr, rx_entry, iov,
					 iov_count, mr_entry, cmd,
					 &ipc_entry);
		if (ret)
			resp->status = ret;
		smr_signal(peer_smr);

		return ipc_entry;
	}

	if (cmd->msg.hdr.op == ofi_op_read_req) {
		hmem_copy_ret = ofi_copy_from_hmem_iov(ptr, cmd->msg.hdr.size,
					cmd->msg.data.ipc_info.iface,
					cmd->msg.data.ipc_info.device, iov,
					iov_count, 0);
	} else {
		hmem_copy_ret = ofi_copy_to_hmem_iov(cmd->msg.data.ipc_info.iface,
					cmd->msg.data.ipc_info.device, iov,
					iov_count, 0, ptr, cmd->msg.hdr.size);
	}

	if (cmd->msg.data.ipc_info.iface == FI_HMEM_ZE) {
		close(ipc_fd);
		/* Truncation error takes precedence over close_handle error */
		ret = ofi_hmem_close_handle(cmd->msg.data.ipc_info.iface, base);
	} else {
		ofi_mr_cache_delete(domain->ipc_cache, mr_entry);
	}

	if (hmem_copy_ret < 0)
		*err = hmem_copy_ret;
	else if (hmem_copy_ret != cmd->msg.hdr.size)
		*err = -FI_ETRUNC;
	else
		*err = ret;

	*total_len = hmem_copy_ret;

out:
	//Status must be set last (signals peer: op done, valid resp entry)
	resp->status = ret;
	smr_signal(peer_smr);

	return NULL;
}

static void smr_do_atomic(void *src, void *dst, void *cmp, enum fi_datatype datatype,
			  enum fi_op op, size_t cnt, uint16_t flags)
{
	char tmp_result[SMR_INJECT_SIZE];

	if (ofi_atomic_isswap_op(op)) {
		ofi_atomic_swap_handler(op, datatype, dst, src, cmp,
					tmp_result, cnt);
	} else if (flags & SMR_RMA_REQ && ofi_atomic_isreadwrite_op(op)) {
		ofi_atomic_readwrite_handler(op, datatype, dst, src,
					     tmp_result, cnt);
	} else if (ofi_atomic_iswrite_op(op)) {
		ofi_atomic_write_handler(op, datatype, dst, src, cnt);
	} else {
		FI_WARN(&smr_prov, FI_LOG_EP_DATA,
			"invalid atomic operation\n");
	}

	if (flags & SMR_RMA_REQ)
		memcpy(src, op == FI_ATOMIC_READ ? dst : tmp_result,
		       cnt * ofi_datatype_size(datatype));
}

static int smr_progress_inline_atomic(struct smr_cmd *cmd, struct fi_ioc *ioc,
			       size_t ioc_count, size_t *len)
{
	int i;
	uint8_t *src = cmd->msg.data.msg;

	assert(cmd->msg.hdr.op == ofi_op_atomic);

	for (i = *len = 0; i < ioc_count && *len < cmd->msg.hdr.size; i++) {
		smr_do_atomic(&src[*len], ioc[i].addr, NULL,
			      cmd->msg.hdr.datatype, cmd->msg.hdr.atomic_op,
			      ioc[i].count, cmd->msg.hdr.op_flags);
		*len += ioc[i].count * ofi_datatype_size(cmd->msg.hdr.datatype);
	}

	if (*len != cmd->msg.hdr.size) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"recv truncated");
		return -FI_ETRUNC;
	}
	return FI_SUCCESS;
}

static int smr_progress_inject_atomic(struct smr_cmd *cmd, struct fi_ioc *ioc,
			       size_t ioc_count, size_t *len,
			       struct smr_ep *ep, int err)
{
	struct smr_inject_buf *tx_buf;
	size_t inj_offset;
	uint8_t *src, *comp;
	int i;

	inj_offset = (size_t) cmd->msg.hdr.src_data;
	tx_buf = smr_get_ptr(ep->region, inj_offset);
	if (err)
		goto out;

	switch (cmd->msg.hdr.op) {
	case ofi_op_atomic_compare:
		src = tx_buf->buf;
		comp = tx_buf->comp;
		break;
	default:
		src = tx_buf->data;
		comp = NULL;
		break;
	}

	for (i = *len = 0; i < ioc_count && *len < cmd->msg.hdr.size; i++) {
		smr_do_atomic(&src[*len], ioc[i].addr, comp ? &comp[*len] : NULL,
			      cmd->msg.hdr.datatype, cmd->msg.hdr.atomic_op,
			      ioc[i].count, cmd->msg.hdr.op_flags);
		*len += ioc[i].count * ofi_datatype_size(cmd->msg.hdr.datatype);
	}

	if (*len != cmd->msg.hdr.size) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"recv truncated");
		err = -FI_ETRUNC;
	}

out:
	if (!(cmd->msg.hdr.op_flags & SMR_RMA_REQ))
		smr_release_txbuf(ep->region, tx_buf);

	return err;
}

static int smr_start_common(struct smr_ep *ep, struct smr_cmd *cmd,
		struct fi_peer_rx_entry *rx_entry)
{
	struct smr_pend_entry *pend = NULL;
	size_t total_len = 0;
	uint64_t comp_flags;
	void *comp_buf;
	int ret;
	uint64_t err = 0;

	switch (cmd->msg.hdr.op_src) {
	case smr_src_inline:
		err = smr_progress_inline(cmd,
				(struct ofi_mr **) rx_entry->desc,
				rx_entry->iov, rx_entry->count, &total_len);
		break;
	case smr_src_inject:
		err = smr_progress_inject(cmd,
				(struct ofi_mr **) rx_entry->desc,
				rx_entry->iov, rx_entry->count, &total_len,
				ep, 0);
		break;
	case smr_src_iov:
		err = smr_progress_iov(cmd, rx_entry->iov, rx_entry->count,
				       &total_len, ep, 0);
		break;
	case smr_src_mmap:
		err = smr_progress_mmap(cmd, (struct ofi_mr **) rx_entry->desc,
					rx_entry->iov, rx_entry->count,
					&total_len, ep);
		break;
	case smr_src_sar:
		pend = smr_progress_sar(cmd, rx_entry,
				       (struct ofi_mr **) rx_entry->desc,
				       rx_entry->iov, rx_entry->count,
				       &total_len, ep);
		break;
	case smr_src_ipc:
		pend = smr_progress_ipc(cmd, rx_entry,
				(struct ofi_mr **) rx_entry->desc,
				rx_entry->iov, rx_entry->count,
				&total_len, ep, (int*)&err);
		break;
	default:
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"unidentified operation type\n");
		err = -FI_EINVAL;
	}

	comp_buf = rx_entry->iov[0].iov_base;
	comp_flags = smr_rx_cq_flags(cmd->msg.hdr.op, rx_entry->flags,
				     cmd->msg.hdr.op_flags);
	if (!pend) {
		if (err) {
			FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
				"error processing op\n");
			ret = smr_write_err_comp(ep->util_ep.rx_cq,
						 rx_entry->context,
						 comp_flags, rx_entry->tag,
						 err);
		} else {
			ret = smr_complete_rx(ep, rx_entry->context, cmd->msg.hdr.op,
					      comp_flags, total_len, comp_buf,
					      cmd->msg.hdr.id, cmd->msg.hdr.tag,
					      cmd->msg.hdr.data);
		}
		if (ret) {
			FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
				"unable to process rx completion\n");
		}
		smr_get_peer_srx(ep)->owner_ops->free_entry(rx_entry);
	}

	return 0;
}

int smr_unexp_start(struct fi_peer_rx_entry *rx_entry)
{
	struct smr_cmd_ctx *cmd_ctx = rx_entry->peer_context;
	int ret;

	ret = smr_start_common(cmd_ctx->ep, &cmd_ctx->cmd, rx_entry);
	ofi_buf_free(cmd_ctx);

	return ret;
}

static void smr_progress_connreq(struct smr_ep *ep, struct smr_cmd *cmd)
{
	struct smr_region *peer_smr;
	struct smr_inject_buf *tx_buf;
	size_t inj_offset;
	int64_t idx = -1;
	int ret = 0;

	inj_offset = (size_t) cmd->msg.hdr.src_data;
	tx_buf = smr_get_ptr(ep->region, inj_offset);

	ret = smr_map_add(&smr_prov, ep->region->map,
			  (char *) tx_buf->data, &idx);
	if (ret)
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"Error processing mapping request\n");

	peer_smr = smr_peer_region(ep->region, idx);

	if (peer_smr->pid != (int) cmd->msg.hdr.data) {
		//TODO track and update/complete in error any transfers
		//to or from old mapping
		munmap(peer_smr, peer_smr->total_size);
		smr_map_to_region(&smr_prov, ep->region->map, idx);
		peer_smr = smr_peer_region(ep->region, idx);
	}
	smr_peer_data(peer_smr)[cmd->msg.hdr.id].addr.id = idx;
	smr_peer_data(ep->region)[idx].addr.id = cmd->msg.hdr.id;

	smr_release_txbuf(ep->region, tx_buf);
	assert(ep->region->map->num_peers > 0);
	ep->region->max_sar_buf_per_peer = SMR_MAX_PEERS /
		ep->region->map->num_peers;
}

static int smr_alloc_cmd_ctx(struct smr_ep *ep,
		struct fi_peer_rx_entry *rx_entry, struct smr_cmd *cmd)
{
	struct smr_cmd_ctx *cmd_ctx;

	cmd_ctx = ofi_buf_alloc(ep->cmd_ctx_pool);
	if (!cmd_ctx) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"Error allocating cmd ctx\n");
		return -FI_ENOMEM;
	}
	memcpy(&cmd_ctx->cmd, cmd, sizeof(*cmd));
	cmd_ctx->ep = ep;

	rx_entry->peer_context = cmd_ctx;

	return FI_SUCCESS;
}

static int smr_progress_cmd_msg(struct smr_ep *ep, struct smr_cmd *cmd)
{
	struct fid_peer_srx *peer_srx = smr_get_peer_srx(ep);
	struct fi_peer_rx_entry *rx_entry;
	fi_addr_t addr;
	int ret;

	addr = ep->region->map->peers[cmd->msg.hdr.id].fiaddr;
	if (cmd->msg.hdr.op == ofi_op_tagged) {
		ret = peer_srx->owner_ops->get_tag(peer_srx, addr,
				cmd->msg.hdr.size, cmd->msg.hdr.tag, &rx_entry);
		ofi_ep_lock_release(&ep->util_ep);
		if (ret == -FI_ENOENT) {
			ret = smr_alloc_cmd_ctx(ep, rx_entry, cmd);
			if (ret)
				return ret;

			ret = peer_srx->owner_ops->queue_tag(rx_entry);
			goto out;
		}
	} else {
		ret = peer_srx->owner_ops->get_msg(peer_srx, addr,
				cmd->msg.hdr.size, &rx_entry);
		ofi_ep_lock_release(&ep->util_ep);
		if (ret == -FI_ENOENT) {
			ret = smr_alloc_cmd_ctx(ep, rx_entry, cmd);
			if (ret)
				return ret;

			ret = peer_srx->owner_ops->queue_msg(rx_entry);
			goto out;
		}
	}
	if (ret) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL, "Error getting rx_entry\n");
		return ret;
	}
	ret = smr_start_common(ep, cmd, rx_entry);

out:
	return ret < 0 ? ret : 0;
}

static int smr_progress_cmd_rma(struct smr_ep *ep, struct smr_cmd *cmd,
				struct smr_cmd *rma_cmd)
{
	struct smr_region *peer_smr;
	struct smr_domain *domain;
	struct smr_resp *resp;
	struct iovec iov[SMR_IOV_LIMIT];
	size_t iov_count;
	size_t total_len = 0;
	int err = 0, ret = 0;
	struct ofi_mr *mr[SMR_IOV_LIMIT];

	domain = container_of(ep->util_ep.domain, struct smr_domain,
			      util_domain);

	ofi_genlock_lock(&domain->util_domain.lock);
	for (iov_count = 0; iov_count < rma_cmd->rma.rma_count; iov_count++) {
		ret = ofi_mr_map_verify(&domain->util_domain.mr_map,
				(uintptr_t *) &(rma_cmd->rma.rma_iov[iov_count].addr),
				rma_cmd->rma.rma_iov[iov_count].len,
				rma_cmd->rma.rma_iov[iov_count].key,
				ofi_rx_mr_reg_flags(cmd->msg.hdr.op, 0),
				(void **) &mr[iov_count]);
		if (ret)
			break;

		iov[iov_count].iov_base = (void *) rma_cmd->rma.rma_iov[iov_count].addr;
		iov[iov_count].iov_len = rma_cmd->rma.rma_iov[iov_count].len;
	}
	ofi_genlock_unlock(&domain->util_domain.lock);

	if (ret)
		goto out;

	switch (cmd->msg.hdr.op_src) {
	case smr_src_inline:
		err = smr_progress_inline(cmd, mr, iov, iov_count, &total_len);
		break;
	case smr_src_inject:
		err = smr_progress_inject(cmd, mr, iov, iov_count, &total_len,
					  ep, ret);
		if (cmd->msg.hdr.op == ofi_op_read_req && cmd->msg.hdr.data) {
			peer_smr = smr_peer_region(ep->region, cmd->msg.hdr.id);
			resp = smr_get_ptr(peer_smr, cmd->msg.hdr.data);
			resp->status = -err;
			smr_signal(peer_smr);
		}
		break;
	case smr_src_iov:
		err = smr_progress_iov(cmd, iov, iov_count, &total_len, ep, ret);
		break;
	case smr_src_mmap:
		err = smr_progress_mmap(cmd, mr, iov, iov_count, &total_len,
					ep);
		break;
	case smr_src_sar:
		if (smr_progress_sar(cmd, NULL, mr, iov, iov_count, &total_len,
				     ep))
			return ret;
		break;
	case smr_src_ipc:
		if (smr_progress_ipc(cmd, NULL, mr, iov, iov_count, &total_len,
				     ep, &ret))
			return ret;
		break;
	default:
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"unidentified operation type\n");
		err = -FI_EINVAL;
	}

	if (err) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"error processing rma op\n");
		ret = smr_write_err_comp(ep->util_ep.rx_cq, NULL,
					 smr_rx_cq_flags(cmd->msg.hdr.op, 0,
					 cmd->msg.hdr.op_flags), 0, err);
	} else {
		ret = smr_complete_rx(ep, (void *) cmd->msg.hdr.msg_id,
			      cmd->msg.hdr.op, smr_rx_cq_flags(cmd->msg.hdr.op,
			      0, cmd->msg.hdr.op_flags), total_len,
			      iov_count ? iov[0].iov_base : NULL,
			      cmd->msg.hdr.id, 0, cmd->msg.hdr.data);
	}
	if (ret) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
		"unable to process rx completion\n");
	}

out:
	return ret;
}

static int smr_progress_cmd_atomic(struct smr_ep *ep, struct smr_cmd *cmd,
				   struct smr_cmd *rma_cmd)
{
	struct smr_region *peer_smr;
	struct smr_domain *domain;
	struct smr_resp *resp;
	struct fi_ioc ioc[SMR_IOV_LIMIT];
	size_t ioc_count;
	size_t total_len = 0;
	int err = 0, ret = 0;

	domain = container_of(ep->util_ep.domain, struct smr_domain,
			      util_domain);

	for (ioc_count = 0; ioc_count < rma_cmd->rma.rma_count; ioc_count++) {
		ret = ofi_mr_verify(&domain->util_domain.mr_map,
				rma_cmd->rma.rma_ioc[ioc_count].count *
				ofi_datatype_size(cmd->msg.hdr.datatype),
				(uintptr_t *) &(rma_cmd->rma.rma_ioc[ioc_count].addr),
				rma_cmd->rma.rma_ioc[ioc_count].key,
				ofi_rx_mr_reg_flags(cmd->msg.hdr.op,
				cmd->msg.hdr.atomic_op));
		if (ret)
			break;

		ioc[ioc_count].addr = (void *) rma_cmd->rma.rma_ioc[ioc_count].addr;
		ioc[ioc_count].count = rma_cmd->rma.rma_ioc[ioc_count].count;
	}
	if (ret)
		goto out;

	switch (cmd->msg.hdr.op_src) {
	case smr_src_inline:
		err = smr_progress_inline_atomic(cmd, ioc, ioc_count, &total_len);
		break;
	case smr_src_inject:
		err = smr_progress_inject_atomic(cmd, ioc, ioc_count, &total_len, ep, ret);
		break;
	default:
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"unidentified operation type\n");
		err = -FI_EINVAL;
	}
	if (cmd->msg.hdr.data) {
		peer_smr = smr_peer_region(ep->region, cmd->msg.hdr.id);
		resp = smr_get_ptr(peer_smr, cmd->msg.hdr.data);
		resp->status = -err;
		smr_signal(peer_smr);
	}

	if (err) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"error processing atomic op\n");
		ret = smr_write_err_comp(ep->util_ep.rx_cq, NULL,
					 smr_rx_cq_flags(cmd->msg.hdr.op, 0,
					 cmd->msg.hdr.op_flags), 0, err);
	} else {
		ret = smr_complete_rx(ep, NULL, cmd->msg.hdr.op,
				      smr_rx_cq_flags(cmd->msg.hdr.op, 0,
				      cmd->msg.hdr.op_flags), total_len,
				      ioc_count ? ioc[0].addr : NULL,
				      cmd->msg.hdr.id, 0, cmd->msg.hdr.data);
	}
	if (ret) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"unable to process rx completion\n");
		err = ret;
	}

out:
	return err;
}

static void smr_progress_cmd(struct smr_ep *ep)
{
	struct smr_cmd_entry *ce;
	int ret = 0;
	int64_t pos;

	/* ep->util_ep.lock is used to serialize the message/tag matching.
	 * We keep the lock until the matching is complete. This will
	 * ensure that commands are matched in the order they are
	 * received, if there are multiple progress threads.
	 *
	 * This lock should be low cost because it's only used by this
	 * single process. It is also optimized to be a noop if
	 * multi-threading is disabled.
	 *
	 * Other processes are free to post on the queue without the need
	 * for locking the queue.
	 */
	while (1) {
		ofi_ep_lock_acquire(&ep->util_ep);
		ret = smr_cmd_queue_head(smr_cmd_queue(ep->region), &ce, &pos);
		if (ret == -FI_ENOENT) {
			ofi_ep_lock_release(&ep->util_ep);
			break;
		}
		switch (ce->cmd.msg.hdr.op) {
		case ofi_op_msg:
		case ofi_op_tagged:
			ret = smr_progress_cmd_msg(ep, &ce->cmd);
			break;
		case ofi_op_write:
		case ofi_op_read_req:
			ofi_ep_lock_release(&ep->util_ep);
			ret = smr_progress_cmd_rma(ep, &ce->cmd,
				&ce->rma_cmd);
			break;
		case ofi_op_write_async:
		case ofi_op_read_async:
			ofi_ep_lock_release(&ep->util_ep);
			ofi_ep_rx_cntr_inc_func(&ep->util_ep,
						ce->cmd.msg.hdr.op);
			break;
		case ofi_op_atomic:
		case ofi_op_atomic_fetch:
		case ofi_op_atomic_compare:
			ofi_ep_lock_release(&ep->util_ep);
			ret = smr_progress_cmd_atomic(ep, &ce->cmd,
				&ce->rma_cmd);
			break;
		case SMR_OP_MAX + ofi_ctrl_connreq:
			ofi_ep_lock_release(&ep->util_ep);
			smr_progress_connreq(ep, &ce->cmd);
			break;
		default:
			ofi_ep_lock_release(&ep->util_ep);
			FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
				"unidentified operation type\n");
			ret = -FI_EINVAL;
		}
		smr_cmd_queue_release(smr_cmd_queue(ep->region), ce, pos);
		if (ret) {
			smr_signal(ep->region);
			if (ret != -FI_EAGAIN) {
				FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
					"error processing command\n");
			}
			break;
		}
	}
}

static void smr_progress_ipc_list(struct smr_ep *ep)
{
	struct smr_pend_entry *ipc_entry;
	struct smr_region *peer_smr;
	struct smr_domain *domain;
	enum fi_hmem_iface iface;
	struct dlist_entry *tmp;
	struct smr_resp *resp;
	uint64_t device;
	uint64_t flags;
	void *context;
	int ret;

	domain = container_of(ep->util_ep.domain, struct smr_domain,
			      util_domain);

	ofi_ep_lock_acquire(&ep->util_ep);

	/* after the synchronize all operations should be complete */
	dlist_foreach_container_safe(&ep->ipc_cpy_pend_list,
				     struct smr_pend_entry,
				     ipc_entry, entry, tmp) {
		iface = ipc_entry->cmd.msg.data.ipc_info.iface;
		device = ipc_entry->cmd.msg.data.ipc_info.device;
		peer_smr = smr_peer_region(ep->region, ipc_entry->cmd.msg.hdr.id);
		resp = smr_get_ptr(peer_smr, ipc_entry->cmd.msg.hdr.src_data);

		if (ofi_async_copy_query(iface, ipc_entry->async_event))
			continue;

		ofi_ep_lock_release(&ep->util_ep);

		if (ipc_entry->rx_entry) {
			context = ipc_entry->rx_entry->context;
			flags = smr_rx_cq_flags(ipc_entry->cmd.msg.hdr.op,
					ipc_entry->rx_entry->flags,
					ipc_entry->cmd.msg.hdr.op_flags);
		} else {
			context = NULL;
			flags = smr_rx_cq_flags(ipc_entry->cmd.msg.hdr.op,
					0, ipc_entry->cmd.msg.hdr.op_flags);
		}

		ret = smr_complete_rx(ep, context, ipc_entry->cmd.msg.hdr.op,
				flags, ipc_entry->bytes_done,
				ipc_entry->iov[0].iov_base,
				ipc_entry->cmd.msg.hdr.id,
				ipc_entry->cmd.msg.hdr.tag,
				ipc_entry->cmd.msg.hdr.data);
		if (ret) {
			FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
				"unable to process rx completion\n");
		}

		/* indicate that the operation is completed only after we
		 * have confirmed that the write has finished. This is to
		 * ensure that the tx_complete occurs after the sending
		 * buffer is now free to be reused
		 */
		resp->status = SMR_STATUS_SUCCESS;
		smr_signal(peer_smr);

		ofi_mr_cache_delete(domain->ipc_cache, ipc_entry->ipc_entry);
		ofi_free_async_copy_event(iface, device,
					  ipc_entry->async_event);
		ofi_ep_lock_acquire(&ep->util_ep);
		dlist_remove(&ipc_entry->entry);
		if (ipc_entry->rx_entry)
			smr_get_peer_srx(ep)->owner_ops->free_entry(ipc_entry->rx_entry);
		ofi_freestack_push(ep->pend_fs, ipc_entry);
	}

	ofi_ep_lock_release(&ep->util_ep);
}

static void smr_progress_sar_list(struct smr_ep *ep)
{
	struct smr_region *peer_smr;
	struct smr_pend_entry *sar_entry;
	struct smr_resp *resp;
	struct dlist_entry *tmp;
	void *comp_ctx;
	uint64_t comp_flags;
	int ret;

	ofi_ep_lock_acquire(&ep->util_ep);
	dlist_foreach_container_safe(&ep->sar_list, struct smr_pend_entry,
				     sar_entry, entry, tmp) {
		if (sar_entry->in_use)
			continue;
		sar_entry->in_use = true;
		ofi_ep_lock_release(&ep->util_ep);
		peer_smr = smr_peer_region(ep->region, sar_entry->cmd.msg.hdr.id);
		resp = smr_get_ptr(peer_smr, sar_entry->cmd.msg.hdr.src_data);
		if (sar_entry->cmd.msg.hdr.op == ofi_op_read_req)
			smr_try_progress_to_sar(ep, peer_smr, smr_sar_pool(ep->region),
					resp, &sar_entry->cmd, sar_entry->mr,
					sar_entry->iov, sar_entry->iov_count,
					&sar_entry->bytes_done,
					&sar_entry->next, sar_entry);
		else
			smr_try_progress_from_sar(ep, peer_smr, smr_sar_pool(ep->region),
					resp, &sar_entry->cmd, sar_entry->mr,
					sar_entry->iov, sar_entry->iov_count,
					&sar_entry->bytes_done,
					&sar_entry->next, sar_entry);

		if (sar_entry->bytes_done == sar_entry->cmd.msg.hdr.size) {
			if (sar_entry->rx_entry) {
				comp_ctx = sar_entry->rx_entry->context;
				comp_flags = smr_rx_cq_flags(sar_entry->cmd.msg.hdr.op,
						sar_entry->rx_entry->flags,
						sar_entry->cmd.msg.hdr.op_flags);
			} else {
				comp_ctx = NULL;
				comp_flags = smr_rx_cq_flags(sar_entry->cmd.msg.hdr.op,
						0, sar_entry->cmd.msg.hdr.op_flags);
			}
			ret = smr_complete_rx(ep, comp_ctx,
					sar_entry->cmd.msg.hdr.op, comp_flags,
					sar_entry->bytes_done,
					sar_entry->iov[0].iov_base,
					sar_entry->cmd.msg.hdr.id,
					sar_entry->cmd.msg.hdr.tag,
					sar_entry->cmd.msg.hdr.data);
			if (ret) {
				FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
					"unable to process rx completion\n");
			}
			if (sar_entry->rx_entry)
				smr_get_peer_srx(ep)->owner_ops->free_entry(sar_entry->rx_entry);

			ofi_ep_lock_acquire(&ep->util_ep);
			dlist_remove(&sar_entry->entry);
			ofi_freestack_push(ep->pend_fs, sar_entry);
		} else {
			ofi_ep_lock_acquire(&ep->util_ep);
			sar_entry->in_use = false;
		}
	}
	ofi_ep_lock_release(&ep->util_ep);
}

void smr_ep_progress(struct util_ep *util_ep)
{
	struct smr_ep *ep;

	ep = container_of(util_ep, struct smr_ep, util_ep);

	if (ofi_atomic_cas_bool32(&ep->region->signal, 1, 0)) {
		if (smr_env.use_dsa_sar)
			smr_dsa_progress(ep);
		smr_progress_resp(ep);
		smr_progress_cmd(ep);
		smr_progress_sar_list(ep);
	}
	/* always drive forward the ipc list since the completion is
	 * independent of any action by the provider */
	smr_progress_ipc_list(ep);
}
