/*
 * Copyright (c) 2019 Intel Corporation. All rights reserved.
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

#include <arpa/inet.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <inttypes.h>

#if HAVE_GETIFADDRS
#include <net/if.h>
#include <ifaddrs.h>
#endif

#include <ofi_util.h>

#include <rdma/fi_collective.h>
#include <rdma/fi_cm.h>
#include <ofi_list.h>
#include <ofi_atomic.h>
#include <ofi_coll.h>
#include <ofi_osd.h>

static uint64_t util_coll_cid[OFI_CONTEXT_ID_SIZE];
/* TODO: if collective support is requested, initialize up front
 * when opening the domain or EP
 */
static bool util_coll_cid_initialized = 0;


int ofi_av_set_union(struct fid_av_set *dst, const struct fid_av_set *src)
{
	struct util_av_set *src_av_set;
	struct util_av_set *dst_av_set;
	size_t temp_count;
	int i,j;

	src_av_set = container_of(src, struct util_av_set, av_set_fid);
	dst_av_set = container_of(dst, struct util_av_set, av_set_fid);

	assert(src_av_set->av == dst_av_set->av);
	temp_count = dst_av_set->fi_addr_count;

	for (i = 0; i < src_av_set->fi_addr_count; i++) {
		for (j = 0; j < dst_av_set->fi_addr_count; j++) {
			if (dst_av_set->fi_addr_array[j] ==
			    src_av_set->fi_addr_array[i])
				break;
		}
		if (j == dst_av_set->fi_addr_count) {
			dst_av_set->fi_addr_array[temp_count++] =
				src_av_set->fi_addr_array[i];
		}
	}

	dst_av_set->fi_addr_count = temp_count;
	return FI_SUCCESS;
}

int ofi_av_set_intersect(struct fid_av_set *dst, const struct fid_av_set *src)
{
	struct util_av_set *src_av_set;
	struct util_av_set *dst_av_set;
	int i,j, temp;

	src_av_set = container_of(src, struct util_av_set, av_set_fid);
	dst_av_set = container_of(dst, struct util_av_set, av_set_fid);

	assert(src_av_set->av == dst_av_set->av);

	temp = 0;
	for (i = 0; i < src_av_set->fi_addr_count; i++) {
		for (j = temp; j < dst_av_set->fi_addr_count; j++) {
			if (dst_av_set->fi_addr_array[j] ==
			    src_av_set->fi_addr_array[i]) {
				dst_av_set->fi_addr_array[temp++] =
					dst_av_set->fi_addr_array[j];
				break;
			}
		}
	}
	dst_av_set->fi_addr_count = temp;
	return FI_SUCCESS;
}

int ofi_av_set_diff(struct fid_av_set *dst, const struct fid_av_set *src)
{

	struct util_av_set *src_av_set;
	struct util_av_set *dst_av_set;
	int i,j, temp;

	src_av_set = container_of(src, struct util_av_set, av_set_fid);
	dst_av_set = container_of(dst, struct util_av_set, av_set_fid);

	assert(src_av_set->av == dst_av_set->av);

	temp = dst_av_set->fi_addr_count;
	for (i = 0; i < src_av_set->fi_addr_count; i++) {
		for (j = 0; j < temp; j++) {
			if (dst_av_set->fi_addr_array[j] ==
			    src_av_set->fi_addr_array[i]) {
				dst_av_set->fi_addr_array[--temp] =
					dst_av_set->fi_addr_array[j];
				break;
			}
		}
	}
	dst_av_set->fi_addr_count = temp;
	return FI_SUCCESS;
}

int ofi_av_set_insert(struct fid_av_set *set, fi_addr_t addr)
{
	struct util_av_set *av_set;
	int i;

	av_set = container_of(set, struct util_av_set, av_set_fid);

	for (i = 0; i < av_set->fi_addr_count; i++) {
		if (av_set->fi_addr_array[i] == addr)
			return -FI_EINVAL;
	}
	av_set->fi_addr_array[av_set->fi_addr_count++] = addr;
	return FI_SUCCESS;
}

int ofi_av_set_remove(struct fid_av_set *set, fi_addr_t addr)

{
	struct util_av_set *av_set;
	int i;

	av_set = container_of(set, struct util_av_set, av_set_fid);

	for (i = 0; i < av_set->fi_addr_count; i++) {
		if (av_set->fi_addr_array[i] == addr) {
			av_set->fi_addr_array[i] =
				av_set->fi_addr_array[--av_set->fi_addr_count];
			return FI_SUCCESS;
		}
	}
	return -FI_EINVAL;
}

int ofi_av_set_addr(struct fid_av_set *set, fi_addr_t *coll_addr)
{
	struct util_av_set *av_set;

	av_set = container_of(set, struct util_av_set, av_set_fid);
	*coll_addr = (uintptr_t)av_set->av->coll_mc;

	return FI_SUCCESS;
}

static inline void util_coll_init_cid(uint64_t *cid)
{
	int i;

	for (i = 0; i < OFI_CONTEXT_ID_SIZE; i++) {
		cid[i] = -1;
	}

	/* reserving the first bit in context id to whole av set */
	cid[0] &= ~0x1ULL;
}

static inline int util_coll_mc_alloc(struct util_coll_mc **coll_mc)
{

	*coll_mc = calloc(1, sizeof(**coll_mc));
	if (!*coll_mc)
		return -FI_ENOMEM;

	slist_init(&(*coll_mc)->barrier_list);
	slist_init(&(*coll_mc)->deferred_list);
	slist_init(&(*coll_mc)->pending_xfer_list);
	return FI_SUCCESS;
}

static inline uint64_t util_coll_form_tag(uint32_t coll_id, uint32_t rank)
{
	uint64_t tag;
	uint64_t src_rank = rank;

	tag = coll_id;
	tag |= (src_rank << 32);

	return OFI_COLL_TAG_FLAG | tag;
}

static inline uint32_t util_coll_get_next_id(struct util_coll_mc *coll_mc)
{
	uint32_t cid = coll_mc->cid;
	return cid << 16 | coll_mc->seq++;
}

static int util_coll_sched_send(struct util_coll_mc *coll_mc, uint32_t dest,
				void *buf, int count, enum fi_datatype datatype,
				uint32_t coll_id, int is_barrier)
{
	struct util_coll_xfer_item *xfer_item;

	xfer_item = calloc(1, sizeof(*xfer_item));
	if (!xfer_item)
		return -FI_ENOMEM;

	xfer_item->hdr.type = UTIL_COLL_SEND;
	xfer_item->hdr.is_barrier = is_barrier;
	xfer_item->hdr.tag = util_coll_form_tag(coll_id, coll_mc->my_rank);
	xfer_item->buf = buf;
	xfer_item->count = count;
	xfer_item->datatype = datatype;
	xfer_item->dest_rank = dest;

	slist_insert_tail(&xfer_item->hdr.entry, &coll_mc->deferred_list);
	return FI_SUCCESS;
}

static int util_coll_sched_recv(struct util_coll_mc *coll_mc, uint32_t src,
				void *buf, int count, enum fi_datatype datatype,
				uint32_t coll_id, int is_barrier)
{
	struct util_coll_xfer_item *xfer_item;

	xfer_item = calloc(1, sizeof(*xfer_item));
	if (!xfer_item)
		return -FI_ENOMEM;

	xfer_item->hdr.type = UTIL_COLL_RECV;
	xfer_item->hdr.is_barrier = is_barrier;

	xfer_item->hdr.tag = util_coll_form_tag(coll_id, src);
	xfer_item->buf = buf;
	xfer_item->count = count;
	xfer_item->datatype = datatype;
	xfer_item->src_rank = src;

	slist_insert_tail(&xfer_item->hdr.entry, &coll_mc->deferred_list);
	return FI_SUCCESS;
}

static int util_coll_sched_reduce(struct util_coll_mc *coll_mc, void *in_buf,
				  void *inout_buf, int count,
				  enum fi_datatype datatype, enum fi_op op,
				  int is_barrier)
{
	struct util_coll_reduce_item *reduce_item;

	reduce_item = calloc(1, sizeof(*reduce_item));
	if (!reduce_item)
		return -FI_ENOMEM;

	reduce_item->hdr.type = UTIL_COLL_REDUCE;
	reduce_item->hdr.is_barrier = is_barrier;
	reduce_item->in_buf = in_buf;
	reduce_item->inout_buf = inout_buf;
	reduce_item->count = count;
	reduce_item->datatype = datatype;
	reduce_item->op = op;

	slist_insert_tail(&reduce_item->hdr.entry, &coll_mc->deferred_list);
	return FI_SUCCESS;
}

static int util_coll_sched_copy(struct util_coll_mc *coll_mc, void *in_buf,
				void *out_buf, int count,
				enum fi_datatype datatype, int is_barrier)
{
	struct util_coll_copy_item *copy_item;

	copy_item = calloc(1, sizeof(*copy_item));
	if (!copy_item)
		return -FI_ENOMEM;

	copy_item->hdr.type = UTIL_COLL_COPY;
	copy_item->hdr.is_barrier = is_barrier;
	copy_item->in_buf = in_buf;
	copy_item->out_buf = out_buf;
	copy_item->count = count;
	copy_item->datatype = datatype;

	slist_insert_tail(&copy_item->hdr.entry, &coll_mc->deferred_list);
	return FI_SUCCESS;
}

static int util_coll_sched_comp(struct util_coll_mc *coll_mc,
				enum util_coll_op_type op_type, void *ctx,
				struct util_coll_state *state,
				util_coll_comp_fn_t comp_fn)
{
	state->hdr.type = UTIL_COLL_COMP;
	state->hdr.is_barrier = 0;

	state->type = op_type;
	state->context = ctx;
	state->comp_fn = comp_fn;

	slist_insert_tail(&state->hdr.entry, &coll_mc->deferred_list);
	return FI_SUCCESS;
}

/* TODO: when this fails, clean up the already scheduled work in this function */
static int util_coll_allreduce(struct util_coll_mc *coll_mc, void *send_buf,
			void *recv_buf, int count, enum fi_datatype datatype,
			enum fi_op op)
{
	uint32_t coll_id;
	int rem, pof2, my_new_id;
	int dest, new_dest;
	int ret;
	int mask = 1;

	coll_id = util_coll_get_next_id(coll_mc);
	pof2 = rounddown_power_of_two(coll_mc->av_set->fi_addr_count);
	rem = coll_mc->av_set->fi_addr_count - pof2;

	if (coll_mc->my_rank < 2 * rem) {
		if (coll_mc->my_rank % 2 == 0) {
			ret = util_coll_sched_send(coll_mc, coll_mc->my_rank + 1,
						   send_buf, count, datatype,
						   coll_id, BARRIER);
			if (ret)
				return ret;

			my_new_id = -1;
		} else {
			ret = util_coll_sched_recv(coll_mc, coll_mc->my_rank - 1,
						   recv_buf, count, datatype,
						   coll_id, BARRIER);
			if (ret)
				return ret;

			my_new_id = coll_mc->my_rank / 2;

			ret = util_coll_sched_reduce(coll_mc, recv_buf, send_buf,
						     count, datatype, op, BARRIER);
			if (ret)
				return ret;
		}
	} else {
		my_new_id = coll_mc->my_rank - rem;
	}

	if (my_new_id != -1) {
		while (mask < pof2) {
			new_dest = my_new_id ^ mask;
			dest = (new_dest < rem) ? new_dest * 2 + 1 :
				new_dest + rem;

			ret = util_coll_sched_recv(coll_mc, dest, recv_buf,
						   count, datatype, coll_id, NO_BARRIER);
			if (ret)
				return ret;
			ret = util_coll_sched_send(coll_mc, dest, send_buf,
						   count, datatype, coll_id, BARRIER);
			if (ret)
				return ret;

			if (dest < coll_mc->my_rank) {
				ret = util_coll_sched_reduce(coll_mc, recv_buf, send_buf,
							     count, datatype, op, BARRIER);
				if (ret)
					return ret;

			} else {
				ret = util_coll_sched_reduce(coll_mc, send_buf, recv_buf,
							     count, datatype, op, BARRIER);
				if (ret)
					return ret;

				ret = util_coll_sched_copy(coll_mc, recv_buf, send_buf,
							   count, datatype, BARRIER);
				if (ret)
					return ret;
			}
			mask <<= 1;
		}
	}

	if (coll_mc->my_rank < 2 * rem) {
		if (coll_mc->my_rank % 2) {
			ret = util_coll_sched_send(coll_mc, coll_mc->my_rank - 1,
						   send_buf, count, datatype,
						   coll_id, BARRIER);
			if (ret)
				return ret;
		} else {
			ret = util_coll_sched_recv(coll_mc, coll_mc->my_rank + 1,
						   send_buf, count, datatype,
						   coll_id, BARRIER);
			if (ret)
				return ret;
		}
	}
	return FI_SUCCESS;
}

static int util_coll_close(struct fid *fid)
{
	struct util_coll_mc *coll_mc;

	coll_mc = container_of(fid, struct util_coll_mc, mc_fid.fid);

	free(coll_mc);
	return FI_SUCCESS;
}

static struct fi_ops util_coll_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = util_coll_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

/* TODO: Figure out requirements for using collectives.
 * e.g. require local address to be in AV?
 * Determine best way to handle first join request
 */
static int util_coll_find_my_rank(struct fid_ep *ep,
				  struct util_coll_mc *coll_mc)
{
	size_t addrlen;
	char *addr;
	int ret, mem;

	addrlen = sizeof(mem);
	addr = (char *) &mem;

	ret = fi_getname(&ep->fid, addr, &addrlen);
	if (ret != -FI_ETOOSMALL) {
		return ret;
	}

	addr = calloc(1, addrlen);
	if (!addr)
		return -FI_ENOMEM;

	ret = fi_getname(&ep->fid, addr, &addrlen);
	if (ret) {
		free(addr);
		return ret;
	}
	coll_mc->my_rank =
		ofi_av_lookup_fi_addr(coll_mc->av_set->av, addr);

	free(addr);

	return FI_SUCCESS;
}

void util_coll_join_comp(struct util_coll_state *state)
{
	struct fi_eq_err_entry entry;
	struct util_ep *ep;
	ssize_t bytes;
	uint64_t tmp;
	int iter, lsb_set_pos = 0, pos;

	for (iter = 0; iter < OFI_CONTEXT_ID_SIZE; iter++) {
		if (state->data.cid_buf[iter]) {
			tmp = state->data.cid_buf[iter];
			pos = 0;
			while (!(tmp & 0x1)) {
				tmp >>= 1;
				pos++;
			}

			/* clear the bit from global cid space */
			util_coll_cid[iter] ^= (1 << pos);
			lsb_set_pos += pos;
		} else {
			lsb_set_pos += sizeof(state->data.cid_buf[0]) * 8;
		}
	}
	assert(lsb_set_pos < OFI_CONTEXT_ID_SIZE * 8);
	state->mc->cid = lsb_set_pos;
	state->mc->seq = 0;

	/* write to the eq  */
	memset(&entry, 0, sizeof(entry));
	entry.fid = &state->mc->mc_fid.fid;
	entry.context = state->mc->mc_fid.fid.context;
	bytes = sizeof(struct fi_eq_entry);

	ep  = container_of(state->mc->ep, struct util_ep, ep_fid);
	if (ofi_eq_write(&ep->eq->eq_fid, FI_JOIN_COMPLETE,
			 &entry, (size_t) bytes, FI_COLLECTIVE) < 0)
		FI_WARN(ep->domain->fabric->prov, FI_LOG_DOMAIN,
			"join collective - eq write failed\n");

	dlist_remove(&state->entry);
	free(state);
}

void util_coll_barrier_comp(struct util_coll_state *state)
{
	struct util_ep *ep;

	ep = container_of(state->mc->ep, struct util_ep, ep_fid);

	if (ofi_cq_write(ep->tx_cq, state->context, FI_COLLECTIVE,
			 sizeof(state->data), &state->data, 0, state->hdr.tag)) {
		FI_WARN(ep->domain->fabric->prov, FI_LOG_DOMAIN,
			"barrier collective - cq write failed\n");
	}

	dlist_remove(&state->entry);
	free(state);
}

static int util_coll_proc_reduce_item(struct util_coll_mc *coll_mc,
				      struct util_coll_reduce_item *reduce_item)
{
	if (FI_MIN <= reduce_item->op && FI_BXOR >= reduce_item->op) {
		ofi_atomic_write_handlers[reduce_item->op]
					 [reduce_item->datatype](
						 reduce_item->inout_buf,
						 reduce_item->in_buf,
						 reduce_item->count);
	} else {
		return -FI_ENOSYS;
	}

	return FI_SUCCESS;
}

int util_coll_process_pending(struct util_coll_mc *coll_mc)
{
	struct slist_entry *entry;
	struct util_coll_hdr *hdr;
	struct util_coll_xfer_item *xfer_item;
	struct iovec iov;
	struct fi_msg_tagged msg;
	int err;
	int is_barrier;

	msg.msg_iov = &iov;
	msg.desc = NULL;
	msg.iov_count = 1;
	msg.ignore = 0;
	msg.context = coll_mc;
	msg.data = 0;

	while (!slist_empty(&coll_mc->pending_xfer_list)) {
		entry = slist_remove_head(&coll_mc->pending_xfer_list);
		hdr = container_of(entry, struct util_coll_hdr, entry);
		is_barrier = hdr->is_barrier;
		switch (hdr->type) {
		case UTIL_COLL_SEND:
			xfer_item = container_of(
				hdr, struct util_coll_xfer_item, hdr);
			iov.iov_base = xfer_item->buf;
			iov.iov_len = (xfer_item->count *
				       ofi_datatype_size(xfer_item->datatype));
			msg.tag = hdr->tag;
			msg.addr = coll_mc->av_set->fi_addr_array[xfer_item->dest_rank];
			err = fi_tsendmsg(coll_mc->ep, &msg, FI_COLLECTIVE);
			if (err) {
				slist_insert_head(entry, &coll_mc->pending_xfer_list);
				return err;
			}
			break;
		case UTIL_COLL_RECV:
			xfer_item = container_of(
				hdr, struct util_coll_xfer_item, hdr);
			iov.iov_base = xfer_item->buf;
			iov.iov_len = (xfer_item->count *
				       ofi_datatype_size(xfer_item->datatype));
			msg.tag = hdr->tag;
			msg.addr = coll_mc->av_set->fi_addr_array[xfer_item->src_rank];
			err = fi_trecvmsg(coll_mc->ep, &msg, FI_COLLECTIVE);
			if (err)
				return err;
			break;
		default:
			break;
		}

		if (is_barrier)
			break;
	}

	return FI_SUCCESS;
}

static int util_coll_process_work_items(struct util_coll_mc *coll_mc)
{
	struct util_coll_hdr *hdr;
	struct util_coll_reduce_item *reduce_item;
	struct util_coll_copy_item *copy_item;
	struct util_coll_state *state;
	struct slist_entry *entry;
	int ret;
	int is_barrier;

	while (!slist_empty(&coll_mc->deferred_list)) {
		entry = slist_remove_head(&coll_mc->deferred_list);
		hdr = container_of(entry, struct util_coll_hdr, entry);
		is_barrier = hdr->is_barrier;
		switch (hdr->type) {
		case UTIL_COLL_SEND:
			slist_insert_tail(&hdr->entry, &coll_mc->pending_xfer_list);
			slist_insert_tail(&hdr->barrier_entry, &coll_mc->barrier_list);
			break;
		case UTIL_COLL_RECV:
			slist_insert_tail(&hdr->entry, &coll_mc->pending_xfer_list);
			slist_insert_tail(&hdr->barrier_entry, &coll_mc->barrier_list);
			break;
		case UTIL_COLL_REDUCE:
			reduce_item = (struct util_coll_reduce_item *) hdr;
			ret = util_coll_proc_reduce_item(coll_mc, reduce_item);
			free(reduce_item);
			if (ret)
				return ret;
			break;
		case UTIL_COLL_COPY:
			copy_item = (struct util_coll_copy_item *) hdr;
			memcpy(copy_item->out_buf, copy_item->in_buf,
			       copy_item->count *
				       ofi_datatype_size(copy_item->datatype));
			free(copy_item);
			break;
		case UTIL_COLL_COMP:
			state = container_of(hdr, struct util_coll_state, hdr);
			if (state->comp_fn)
				state->comp_fn(state);
			break;
		default:
			break;
		}

		if (is_barrier)
			break;
	}
	return FI_SUCCESS;
}

static int util_coll_schedule(struct util_coll_mc *coll_mc)
{
	int ret;

	if (slist_empty(&coll_mc->barrier_list)) {
		ret = util_coll_process_work_items(coll_mc);
		if (ret)
			return ret;
	}
	return FI_SUCCESS;
}

int ofi_coll_ep_progress(struct fid_ep *ep)
{
	int ret;
	struct util_coll_state *state;
	struct dlist_entry *cur;
	struct util_ep *util_ep;

	util_ep  = container_of(ep, struct util_ep, ep_fid);

	ofi_fastlock_acquire(&util_ep->coll_state_lock);

	dlist_foreach(&util_ep->coll_state_list, cur) {
		state = container_of(cur, struct util_coll_state, entry);
		ret = util_coll_schedule(state->mc);
		if(ret)
			goto err;
	}
	ret = FI_SUCCESS;

err:
	ofi_fastlock_release(&util_ep->coll_state_lock);

	return ret;
}

int ofi_coll_process_pending(struct fid_ep *ep)
{
	int ret;
	struct util_coll_state *state;
	struct dlist_entry *cur;
	struct util_ep *util_ep;

	util_ep  = container_of(ep, struct util_ep, ep_fid);

	ofi_fastlock_acquire(&util_ep->coll_state_lock);

	dlist_foreach(&util_ep->coll_state_list, cur) {
		state = container_of(cur, struct util_coll_state, entry);
		ret = util_coll_process_pending(state->mc);
		if(ret)
			goto err;
	}
	ret = FI_SUCCESS;

err:
	ofi_fastlock_release(&util_ep->coll_state_lock);

	return ret;
}

static inline
int util_coll_state_init(struct util_coll_mc *coll_mc,
			 struct util_coll_state **state)
{
	struct util_ep *util_ep  = container_of(coll_mc->ep, struct util_ep, ep_fid);

	*state = calloc(1, sizeof(struct util_coll_state));
	if (!(*state))
		return -FI_ENOMEM;

	(*state)->mc = coll_mc;
	(*state)->comp_fn = NULL;

	ofi_fastlock_acquire(&util_ep->coll_state_lock);
	dlist_insert_tail(&(*state)->entry, &util_ep->coll_state_list);
	ofi_fastlock_release(&util_ep->coll_state_lock);

	return FI_SUCCESS;
}

int ofi_join_collective(struct fid_ep *ep, fi_addr_t coll_addr,
		       const struct fid_av_set *set,
		       uint64_t flags, struct fid_mc **mc, void *context)
{
	struct util_coll_mc *new_coll_mc;
	struct util_av_set *av_set;
	struct util_coll_mc *coll_mc;
	struct util_coll_state *join_state;

	int ret;

	av_set = container_of(set, struct util_av_set, av_set_fid);

	if (coll_addr == FI_ADDR_NOTAVAIL) {
		assert(av_set->av->coll_mc != NULL);
		coll_mc = av_set->av->coll_mc;
	} else {
		coll_mc = (struct util_coll_mc*) ((uintptr_t) coll_addr);
	}

	ret = util_coll_mc_alloc(&new_coll_mc);
	if (ret)
		return ret;

	if (util_coll_cid_initialized == 0) {
		util_coll_init_cid(util_coll_cid);
		util_coll_cid_initialized = 1;
	}

	// set up the new mc for future collectives
	new_coll_mc->mc_fid.fid.fclass = FI_CLASS_MC;
	new_coll_mc->mc_fid.fid.context = context;
	new_coll_mc->mc_fid.fid.ops = &util_coll_fi_ops;
	new_coll_mc->mc_fid.fi_addr = (uintptr_t) new_coll_mc;
	new_coll_mc->av_set = av_set;
	new_coll_mc->ep = ep;

	coll_mc->ep = ep;

	ret = util_coll_state_init(coll_mc, &join_state);
	if (ret)
		goto err1;

	/* get the rank */
	util_coll_find_my_rank(ep, new_coll_mc);
	util_coll_find_my_rank(ep, coll_mc);

	if (new_coll_mc->my_rank != FI_ADDR_NOTAVAIL) {
		ret = util_coll_sched_copy(coll_mc, util_coll_cid, &join_state->data.cid_buf,
					   OFI_CONTEXT_ID_SIZE * sizeof(uint64_t),
					   FI_UINT8, NO_BARRIER);
		if (ret) {
			goto err2;
		}
	} else {
		util_coll_init_cid(join_state->data.cid_buf);
	}

	ret = util_coll_allreduce(coll_mc, &join_state->data.cid_buf, &join_state->data.tmp_cid_buf,
				  OFI_CONTEXT_ID_SIZE, FI_INT64, FI_BAND);
	if (ret)
		goto err2;

	ret = util_coll_sched_comp(coll_mc, UTIL_COLL_JOIN_OP, context,
				   join_state, util_coll_join_comp);
	if (ret)
		goto err2;

	*mc = &new_coll_mc->mc_fid;
	util_coll_schedule(coll_mc);
	return FI_SUCCESS;

err2:
	free(join_state);
err1:
	free(new_coll_mc);
	return ret;
}

static struct fi_ops_av_set util_av_set_ops= {
	.set_union	=	ofi_av_set_union,
	.intersect	=	ofi_av_set_intersect,
	.diff		=	ofi_av_set_diff,
	.insert		=	ofi_av_set_insert,
	.remove		=	ofi_av_set_remove,
	.addr		=	ofi_av_set_addr
};

static int util_coll_copy_from_av(struct util_av *av, void *addr,
			      fi_addr_t fi_addr, void *arg)
{
	struct util_av_set *av_set = (struct util_av_set *) arg;
	av_set->fi_addr_array[av_set->fi_addr_count++] = fi_addr;
	return FI_SUCCESS;
}

static int util_coll_av_init(struct util_av *av)
{

	struct util_coll_mc *coll_mc;
	int ret;

	assert(!av->coll_mc);

	if (util_coll_cid_initialized == 0) {
		util_coll_init_cid(util_coll_cid);
		util_coll_cid_initialized = 1;
	}

	ret = util_coll_mc_alloc(&coll_mc);
	if (ret)
		return ret;

	coll_mc->av_set = calloc(1, sizeof(*coll_mc->av_set));
	if (!coll_mc->av_set) {
		ret = -FI_ENOMEM;
		goto err1;
	}

	coll_mc->av_set->fi_addr_array =
		calloc(av->count, sizeof(*coll_mc->av_set->fi_addr_array));
	if (!coll_mc->av_set->fi_addr_array) {
		ret = -FI_ENOMEM;
		goto err2;
	}

	ret = fastlock_init(&coll_mc->av_set->lock);
	if (ret)
		goto err3;

	coll_mc->av_set->av = av;
	ret = ofi_av_elements_iter(av, util_coll_copy_from_av,
				   (void *)coll_mc->av_set);
	if (ret)
		goto err4;

	coll_mc->av_set->av_set_fid.fid.fclass = FI_CLASS_AV_SET;
	coll_mc->av_set->av_set_fid.ops = &util_av_set_ops;

	coll_mc->mc_fid.fi_addr = (uintptr_t) coll_mc;
	coll_mc->mc_fid.fid.fclass = FI_CLASS_MC;
	coll_mc->mc_fid.fid.context = NULL;
	coll_mc->mc_fid.fid.ops = &util_coll_fi_ops;
	av->coll_mc = coll_mc;
	return FI_SUCCESS;

err4:
	fastlock_destroy(&coll_mc->av_set->lock);
err3:
	free(coll_mc->av_set->fi_addr_array);
err2:
	free(coll_mc->av_set);
err1:
	free(coll_mc);
	return ret;
}

int ofi_av_set(struct fid_av *av, struct fi_av_set_attr *attr,
	       struct fid_av_set **av_set_fid, void * context)
{
	struct util_av *util_av = container_of(av, struct util_av, av_fid);
	struct util_av_set *av_set;
	int ret, iter;

	if (!util_av->coll_mc) {
		ret = util_coll_av_init(util_av);
		if (ret)
			return ret;
	}

	av_set = calloc(1,sizeof(*av_set));
	if (!av_set)
		return -FI_ENOMEM;

	ret = fastlock_init(&av_set->lock);
	if (ret)
		goto err1;

	av_set->fi_addr_array = calloc(util_av->count, sizeof(*av_set->fi_addr_array));
	if (!av_set->fi_addr_array)
		goto err2;

	for (iter = 0; iter < attr->count; iter++) {
		av_set->fi_addr_array[iter] =
			util_av->coll_mc->av_set->fi_addr_array[iter * attr->stride];
		av_set->fi_addr_count++;
	}

	av_set->av = util_av;
	av_set->av_set_fid.ops = &util_av_set_ops;
	av_set->av_set_fid.fid.fclass = FI_CLASS_AV_SET;
	av_set->av_set_fid.fid.context = context;
	(*av_set_fid) = &av_set->av_set_fid;
	return FI_SUCCESS;
err2:
	fastlock_destroy(&av_set->lock);
err1:
	free(av_set);
	return ret;
}

ssize_t ofi_ep_barrier(struct fid_ep *ep, fi_addr_t coll_addr, void *context)
{
	struct util_coll_mc *coll_mc;
	struct util_coll_state *barrier_state;
	int ret;

	coll_mc = (struct util_coll_mc*) ((uintptr_t) coll_addr);

	util_coll_state_init(coll_mc, &barrier_state);

	ret = util_coll_allreduce(coll_mc, &barrier_state->data.cid_buf,
				  &barrier_state->data.cid_buf, 1,
				  FI_UINT64, FI_BAND);
	if (ret)
		goto err;

	util_coll_sched_comp(coll_mc, UTIL_COLL_BARRIER_OP, context,
			     barrier_state, util_coll_barrier_comp);

	util_coll_schedule(coll_mc);
	return FI_SUCCESS;
err:
	free(barrier_state);
	return ret;
}

static int util_coll_match_barrier_tag(struct slist_entry *entry, const void *arg)
{
	struct util_coll_xfer_item *item;
	uint64_t *tag_ptr = (uint64_t *) arg;

	item = container_of(entry, struct util_coll_xfer_item, hdr.barrier_entry);
	if (item->hdr.tag == *tag_ptr)
		return 1;

	return 0;
}

void ofi_coll_handle_comp(uint64_t tag, void *ctx)
{
	struct util_coll_mc *coll_mc = (struct util_coll_mc *) ctx;
	struct util_coll_xfer_item *item;
	struct slist_entry *entry;
	uint64_t *tag_ptr = &tag;

	entry = slist_remove_first_match(&coll_mc->barrier_list,
					 util_coll_match_barrier_tag,
					 (void *) tag_ptr);
	if (entry) {
		item = container_of(entry, struct util_coll_xfer_item,
				    hdr.barrier_entry);
		free(item);
	}

	util_coll_schedule(coll_mc);
}