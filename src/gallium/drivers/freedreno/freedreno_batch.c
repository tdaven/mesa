/*
 * Copyright (C) 2016 Rob Clark <robclark@freedesktop.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "util/u_string.h"

#include "freedreno_batch.h"
#include "freedreno_context.h"

struct fd_batch *
fd_batch_create(struct fd_context *ctx)
{
	struct fd_batch *batch = CALLOC_STRUCT(fd_batch);
	static unsigned seqno = 0;

	if (!batch)
		return NULL;

	pipe_reference_init(&batch->reference, 1);
	batch->seqno = ++seqno;
	batch->ctx = ctx;

	/* TODO how to pick a good size?  Or maybe we should introduce
	 * fd_ringlist?  Also, make sure size is aligned with bo-cache
	 * bucket size, since otherwise that will round up size..
	 */
	batch->draw    = fd_ringbuffer_new(ctx->screen->pipe, 0x10000);
	batch->binning = fd_ringbuffer_new(ctx->screen->pipe, 0x10000);
	batch->gmem    = fd_ringbuffer_new(ctx->screen->pipe, 0x10000);

	fd_ringbuffer_set_parent(batch->gmem, NULL);
	fd_ringbuffer_set_parent(batch->draw, batch->gmem);
	fd_ringbuffer_set_parent(batch->binning, batch->gmem);

	return batch;
}

void
__fd_batch_destroy(struct fd_batch *batch)
{
	fd_ringbuffer_del(batch->draw);
	fd_ringbuffer_del(batch->binning);
	fd_ringbuffer_del(batch->gmem);

	free(batch);
}

void
__fd_batch_describe(char* buf, const struct fd_batch *batch)
{
	util_sprintf(buf, "fd_batch<%u>", batch->seqno);
}

void
fd_batch_flush(struct fd_batch *batch)
{
	fd_gmem_render_tiles(batch->ctx);
}

void
fd_batch_check_size(struct fd_batch *batch)
{
	/* TODO eventually support having a list of draw/binning rb's
	 * and if we are too close to the end, add another to the
	 * list.  For now we just flush.
	 */
	struct fd_ringbuffer *ring = batch->draw;
	if (((ring->cur - ring->start) > (ring->size/4 - 0x1000)) ||
			(fd_mesa_debug & FD_DBG_FLUSH))
		fd_context_render(&batch->ctx->base);
}