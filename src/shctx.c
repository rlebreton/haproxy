/*
 * shctx.c - shared context management functions for SSL
 *
 * Copyright (C) 2011-2012 EXCELIANCE
 *
 * Author: Emeric Brun - emeric@exceliance.fr
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <sys/mman.h>
#include <arpa/inet.h>
#include <import/ebmbtree.h>
#include <haproxy/list.h>
#include <haproxy/shctx.h>

int use_shared_mem = 0;

FILE *debug_file = NULL;

static unsigned int nbav_init = 0;

__attribute__((noinline))
void shctx_rdlock(struct shared_context *shctx)
{
       if (use_shared_mem)
               HA_RWLOCK_RDLOCK(SHCTX_LOCK, &shctx->lock);
}

__attribute__((noinline))
void shctx_rdunlock(struct shared_context *shctx)
{
       if (use_shared_mem)
              HA_RWLOCK_RDUNLOCK(SHCTX_LOCK, &shctx->lock);
}

__attribute__((noinline))
void shctx_wrlock(struct shared_context *shctx)
{
       if (use_shared_mem)
               HA_RWLOCK_WRLOCK(SHCTX_LOCK, &shctx->lock);
}

__attribute__((noinline))
void shctx_wrunlock(struct shared_context *shctx)
{
       if (use_shared_mem)
              HA_RWLOCK_WRUNLOCK(SHCTX_LOCK, &shctx->lock);
}

__attribute__((noinline))
void shctx_rdlock_avail(struct shared_context *shctx)
{
       if (use_shared_mem)
               HA_RWLOCK_RDLOCK(SHCTX_LOCK, &shctx->avail_lock);
}

__attribute__((noinline))
void shctx_rdunlock_avail(struct shared_context *shctx)
{
       if (use_shared_mem)
              HA_RWLOCK_RDUNLOCK(SHCTX_LOCK, &shctx->avail_lock);
}

__attribute__((noinline))
void shctx_wrlock_avail(struct shared_context *shctx)
{
       if (use_shared_mem)
               HA_RWLOCK_WRLOCK(SHCTX_LOCK, &shctx->avail_lock);
}

__attribute__((noinline))
void shctx_wrunlock_avail(struct shared_context *shctx)
{
       if (use_shared_mem)
              HA_RWLOCK_WRUNLOCK(SHCTX_LOCK, &shctx->avail_lock);
}

/*
 * Reserve a new row if <first> is null, put it in the hotlist, set the refcount to 1
 * or append new blocks to the row with <first> as first block if non null.
 *
 * Reserve blocks in the avail list and put them in the hot list
 * Return the first block put in the hot list or NULL if not enough blocks available
 */
struct shared_block *shctx_row_reserve_hot(struct shared_context *shctx,
                                           struct shared_block *first, int data_len)
{
	struct shared_block *last = NULL, *block, *sblock;
	struct shared_block *ret = first;
	int remain = 1;

	BUG_ON(data_len < 0);

	/* Check the object size limit. */
	if (shctx->max_obj_size > 0) {
		if ((first && first->len + data_len > shctx->max_obj_size) ||
			(!first && data_len > shctx->max_obj_size))
			goto out;
	}

	if (first) {
		/* Check that there is some block to reserve.
		 * In this first block of code we compute the remaining room in the
		 * current list of block already reserved for this object.
		 * We return asap if there is enough room to copy <data_len> bytes.
		 */
		last = first->last_reserved;
		/* Remaining room. */
		remain = (shctx->block_size * first->block_count - first->len);
		if (remain) {
			if (remain > data_len) {
				return last ? last : first;
			} else {
				data_len -= remain;
				if (data_len <= 0)
					return last ? last : first;
			}
		}
	}

	shctx_wrlock_avail(shctx);

	/* not enough usable blocks */
	if (data_len > shctx->nbav * shctx->block_size) {
		shctx_wrunlock_avail(shctx);
		goto out;
	}


	if (data_len <= 0 || LIST_ISEMPTY(&shctx->avail)) {
		ret = NULL;
		shctx_wrunlock_avail(shctx);
		goto out;
	}

	list_for_each_entry_safe(block, sblock, &shctx->avail, list) {

		/* release callback */
		if (block != ret) {
			if (block->len && shctx->free_block)
				shctx->free_block(shctx, block, block);
			block->len = 0;
			block->block_count = 1;
		}

		if (ret) {
			shctx_block_append_hot(shctx, ret, block);
			if (!remain) {
				first->last_append = block;
				remain = 1;
			}
		} else {
			ret = shctx_block_detach(shctx, block);
			ret->len = 0;
			ret->block_count = 0;
			ret->last_append = NULL;
			ret->refcount = 1;
		}

		++ret->block_count;

		data_len -= shctx->block_size;

		if (data_len <= 0) {
			ret->last_reserved = block;
			break;
		}
	}

	shctx_wrunlock_avail(shctx);

	if (shctx->reserve_finish)
		shctx->reserve_finish(shctx);

out:
	return ret;
}

/*
 * if the refcount is 0 move the row to the hot list. Increment the refcount
 */
void shctx_row_detach(struct shared_context *shctx, struct shared_block *first)
{

	fprintf(debug_file, "shctx_row_detach (tid %d) len %d block_count %d\n", tid, first->len, first->block_count);
	if (first->refcount <= 0) {

		BUG_ON(!first->last_reserved);

		/* Detach row from avail list, link first item's prev to last
		 * item's next. */
		first->list.p->n = first->last_reserved->list.n;
		first->last_reserved->list.n->p = first->list.p;

		first->list.p = &first->last_reserved->list;
		first->last_reserved->list.n = &first->list;

		shctx->nbav -= first->block_count;
	}

	first->refcount++;
}

/*
 * decrement the refcount and move the row at the end of the avail list if it reaches 0.
 */
void shctx_row_reattach(struct shared_context *shctx, struct shared_block *first)
{
	first->refcount--;

	fprintf(debug_file, "shctx_row_reattach (tid %d) len %d block_count %d data %p\n", tid, first->len, first->block_count, first->data);

	if (first->refcount <= 0) {

		BUG_ON(!first->last_reserved);

		/* Reattach to avail list */
// 		BUG_ON(first->list.p != &first->last_reserved->list);
		first->list.p = &first->last_reserved->list;
		LIST_SPLICE_END_DETACHED(&shctx->avail, &first->list);

		shctx->nbav += first->block_count;
	}

// 	BUG_ON(shctx->nbav > nbav_init);
}


/*
 * Append data in the row if there is enough space.
 * The row should be in the hot list
 *
 * Return the amount of appended data if ret >= 0
 * or how much more space it needs to contains the data if < 0.
 */
int shctx_row_data_append(struct shared_context *shctx, struct shared_block *first,
                          unsigned char *data, int len)
{
	int remain, start;
	struct shared_block *block;

	/* return -<len> needed to work */
	if (len > first->block_count * shctx->block_size - first->len)
		return (first->block_count * shctx->block_size - first->len) - len;

	block = first->last_append ? first->last_append : first;
	do {
		/* end of copy */
		if (len <= 0)
			break;

		/* remaining written bytes in the current block. */
		remain = (shctx->block_size * first->block_count - first->len) % shctx->block_size;
		BUG_ON(remain < 0);

		/* if remain == 0, previous buffers are full, or first->len == 0 */
		if (!remain) {
			remain = shctx->block_size;
			start = 0;
		}
		else {
			/* start must be calculated before remain is modified */
			start = shctx->block_size - remain;
			BUG_ON(start < 0);
		}

		/* must not try to copy more than len */
		remain = MIN(remain, len);

		memcpy(block->data + start, data, remain);

		data += remain;
		len -= remain;
		first->len += remain; /* update len in the head of the row */
		first->last_append = block;

		block = LIST_ELEM(block->list.n, struct shared_block*, list);
	} while (block != first);

	return len;
}

/*
 * Copy <len> data from a row of blocks, return the remaining data to copy
 * If 0 is returned, the full data has successfully been copied
 *
 * The row should be in the hot list
 */
int shctx_row_data_get(struct shared_context *shctx, struct shared_block *first,
                       unsigned char *dst, int offset, int len)
{
	int count = 0, size = 0, start = -1;
	struct shared_block *block;

	/* can't copy more */
	if (len > first->len)
		len = first->len;

	block = first;
	count = 0;
	/* Pass through the blocks to copy them */
	do {
		if (count >= first->block_count  || len <= 0)
			break;

		count++;
		/* continue until we are in right block
		   corresponding to the offset */
		if (count < offset / shctx->block_size + 1)
			continue;

		/* on the first block, data won't possibly began at offset 0 */
		if (start == -1)
			start = offset - (count - 1) * shctx->block_size;

		BUG_ON(start < 0);

		/* size can be lower than a block when copying the last block */
		size = MIN(shctx->block_size - start, len);
		BUG_ON(size < 0);

		memcpy(dst, block->data + start, size);
		dst += size;
		len -= size;
		start = 0;

		block = LIST_ELEM(block->list.n, struct shared_block*, list);
	} while (block != first);
	return len;
}

/* Allocate shared memory context.
 * <maxblocks> is maximum blocks.
 * If <maxblocks> is set to less or equal to 0, ssl cache is disabled.
 * Returns: -1 on alloc failure, <maxblocks> if it performs context alloc,
 * and 0 if cache is already allocated.
 */
int shctx_init(struct shared_contexts **shared_contexts, int maxblocks, int blocksize,
               unsigned int maxobjsz, int extra, int shared, int numctx,
               shctx_free_block_cb free_block, shctx_reserve_finish_cb reserve_finish)
{
	int i;
	struct shared_contexts *shctxs;
	struct shared_context *shctx;
	int ret;
	void *cur;
	int maptype = MAP_PRIVATE;
	size_t mapsize = 0;

	if (!debug_file)
		debug_file = fopen("/tmp/toto", "w");


	if (maxblocks <= 0)
		return 0;

	/* make sure to align the records on a pointer size */
	blocksize = (blocksize + sizeof(void *) - 1) & -sizeof(void *);
	extra     = (extra     + sizeof(void *) - 1) & -sizeof(void *);

	if (shared) {
		maptype = MAP_SHARED;
		use_shared_mem = 1;
	}

	mapsize = sizeof(struct shared_contexts) + numctx * (sizeof(struct shared_context) + extra) + (maxblocks * (sizeof(struct shared_block) + blocksize));

	shctxs = (struct shared_contexts *)mmap(NULL, mapsize, PROT_READ | PROT_WRITE,
						maptype | MAP_ANON, -1, 0);
	if (!shctxs || shctxs == MAP_FAILED) {
		shctxs = NULL;
		ret = SHCTX_E_ALLOC_CACHE;
		goto err;
	}

	shctxs->context_num = numctx;
	shctxs->context_size = sizeof(struct shared_context) + extra;

	for (i = 0; i < numctx; ++i) {
		shctx = (struct shared_context *)(shctxs->contexts + i * shctxs->context_size);
		HA_RWLOCK_INIT(&shctx->lock);
		HA_RWLOCK_INIT(&shctx->avail_lock);
		shctx->nbav = 0;

		LIST_INIT(&shctx->avail);
		shctx->block_size = blocksize;
		shctx->max_obj_size = maxobjsz == (unsigned int)-1 ? 0 : maxobjsz;

		shctx->free_block = free_block;
		shctx->reserve_finish = reserve_finish;
	}

	/* init the free blocks after the shared context struct */
	cur = shctxs->contexts + shctxs->context_num * shctxs->context_size;
	for (i = 0; i < maxblocks; i++) {
		struct shared_block *cur_block = (struct shared_block *)cur;
		shctx = (struct shared_context *)(shctxs->contexts + (i*numctx/maxblocks) * shctxs->context_size);
		cur_block->len = 0;
		cur_block->refcount = 0;
		cur_block->block_count = 1;
		LIST_APPEND(&shctx->avail, &cur_block->list);
		shctx->nbav++;
		cur += sizeof(struct shared_block) + blocksize;
	}
	ret = maxblocks;

	if (extra == 96) {
		nbav_init = shctx->nbav;
	}

err:
	*shared_contexts = shctxs;
	return ret;
}

