/*
 * include/haproxy/shctx.h - shared context management functions for SSL
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

#ifndef __HAPROXY_SHCTX_H
#define __HAPROXY_SHCTX_H

#include <haproxy/api.h>
#include <haproxy/list.h>
#include <haproxy/shctx-t.h>
#include <haproxy/thread.h>

int shctx_init(struct shared_contexts **shared_contexts, int maxblocks, int blocksize,
               unsigned int maxobjsz, int extra, int shared, int numctx,
               shctx_free_block_cb free_block);
struct shared_block *shctx_row_reserve_hot(struct shared_context *shctx,
                                           struct shared_block *last, int data_len);
void shctx_row_inc_hot(struct shared_context *shctx, struct shared_block *first);
void shctx_row_dec_hot(struct shared_context *shctx, struct shared_block *first);
int shctx_row_data_append(struct shared_context *shctx,
                          struct shared_block *first,
                          unsigned char *data, int len);
int shctx_row_data_get(struct shared_context *shctx, struct shared_block *first,
                       unsigned char *dst, int offset, int len);


/* Lock functions */

extern int use_shared_mem;

static inline void shctx_rdlock(struct shared_context *shctx)
{
       if (use_shared_mem)
               HA_RWLOCK_RDLOCK(SHCTX_LOCK, &shctx->lock);
}
static inline void shctx_rdunlock(struct shared_context *shctx)
{
       if (use_shared_mem)
              HA_RWLOCK_RDUNLOCK(SHCTX_LOCK, &shctx->lock);
}
static inline void shctx_wrlock(struct shared_context *shctx)
{
       if (use_shared_mem)
               HA_RWLOCK_WRLOCK(SHCTX_LOCK, &shctx->lock);
}
static inline void shctx_wrunlock(struct shared_context *shctx)
{
       if (use_shared_mem)
              HA_RWLOCK_WRUNLOCK(SHCTX_LOCK, &shctx->lock);
}


/* List Macros */

/*
 * Insert <s> block after <head> which is not necessarily the head of a list,
 * so between <head> and the next element after <head>.
 */
static inline void shctx_block_append_hot(struct shared_context *shctx,
                                          struct list *head,
                                          struct shared_block *s)
{
	shctx->nbav--;
	LIST_DELETE(&s->list);
	LIST_INSERT(head, &s->list);
}

static inline void shctx_block_set_hot(struct shared_context *shctx,
				    struct shared_block *s)
{
	shctx->nbav--;
	LIST_DELETE(&s->list);
	LIST_APPEND(&shctx->hot, &s->list);
}

static inline void shctx_block_set_avail(struct shared_context *shctx,
				      struct shared_block *s)
{
	shctx->nbav++;
	LIST_DELETE(&s->list);
	LIST_APPEND(&shctx->avail, &s->list);
}

#endif /* __HAPROXY_SHCTX_H */

