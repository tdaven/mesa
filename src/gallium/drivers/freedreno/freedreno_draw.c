/* -*- mode: C; c-file-style: "k&r"; tab-width 4; indent-tabs-mode: t; -*- */

/*
 * Copyright (C) 2012 Rob Clark <robclark@freedesktop.org>
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

#include "pipe/p_state.h"
#include "util/u_string.h"
#include "util/u_memory.h"
#include "util/u_prim.h"
#include "util/u_format.h"

#include "freedreno_draw.h"
#include "freedreno_context.h"
#include "freedreno_state.h"
#include "freedreno_resource.h"
#include "freedreno_query_hw.h"
#include "freedreno_util.h"

static void
resource_read(struct fd_batch *batch, struct pipe_resource *prsc)
{
	if (!prsc)
		return;
	fd_batch_resource_used(batch, fd_resource(prsc), false);
}

static void
resource_written(struct fd_batch *batch, struct pipe_resource *prsc)
{
	if (!prsc)
		return;
	fd_batch_resource_used(batch, fd_resource(prsc), true);
}

static void
fd_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info)
{
	struct fd_context *ctx = fd_context(pctx);
	struct fd_batch *batch = ctx->batch;
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;
	struct pipe_scissor_state *scissor = fd_context_get_scissor(ctx);
	unsigned i, prims, buffers = 0;

	/* if we supported transform feedback, we'd have to disable this: */
	if (((scissor->maxx - scissor->minx) *
			(scissor->maxy - scissor->miny)) == 0) {
		return;
	}

	/* TODO: push down the region versions into the tiles */
	if (!fd_render_condition_check(pctx))
		return;

	/* emulate unsupported primitives: */
	if (!fd_supported_prim(ctx, info->mode)) {
		if (ctx->streamout.num_targets > 0)
			debug_error("stream-out with emulated prims");
		util_primconvert_save_index_buffer(ctx->primconvert, &ctx->indexbuf);
		util_primconvert_save_rasterizer_state(ctx->primconvert, ctx->rasterizer);
		util_primconvert_draw_vbo(ctx->primconvert, info);
		return;
	}

	if (ctx->in_blit) {
		fd_batch_reset(batch);
		ctx->dirty = ~0;
	}

	batch->blit = ctx->in_blit;
	batch->back_blit = ctx->in_shadow;

	/* NOTE: needs to be before resource_written(batch->query_buf), otherwise
	 * query_buf may not be created yet.
	 */
	fd_hw_query_set_stage(batch, batch->draw, FD_STAGE_DRAW);

	/*
	 * Figure out the buffers/features we need:
	 */

	pipe_mutex_lock(ctx->screen->lock);

	if (fd_depth_enabled(ctx)) {
		buffers |= FD_BUFFER_DEPTH;
		resource_written(batch, pfb->zsbuf->texture);
		batch->gmem_reason |= FD_GMEM_DEPTH_ENABLED;
	}

	if (fd_stencil_enabled(ctx)) {
		buffers |= FD_BUFFER_STENCIL;
		resource_written(batch, pfb->zsbuf->texture);
		batch->gmem_reason |= FD_GMEM_STENCIL_ENABLED;
	}

	if (fd_logicop_enabled(ctx))
		batch->gmem_reason |= FD_GMEM_LOGICOP_ENABLED;

	for (i = 0; i < pfb->nr_cbufs; i++) {
		struct pipe_resource *surf;

		if (!pfb->cbufs[i])
			continue;

		surf = pfb->cbufs[i]->texture;

		resource_written(batch, surf);
		buffers |= PIPE_CLEAR_COLOR0 << i;

		if (surf->nr_samples > 1)
			batch->gmem_reason |= FD_GMEM_MSAA_ENABLED;

		if (fd_blend_enabled(ctx, i))
			batch->gmem_reason |= FD_GMEM_BLEND_ENABLED;
	}

	foreach_bit(i, ctx->constbuf[PIPE_SHADER_VERTEX].enabled_mask)
		resource_read(batch, ctx->constbuf[PIPE_SHADER_VERTEX].cb[i].buffer);
	foreach_bit(i, ctx->constbuf[PIPE_SHADER_FRAGMENT].enabled_mask)
		resource_read(batch, ctx->constbuf[PIPE_SHADER_FRAGMENT].cb[i].buffer);

	/* Mark VBOs as being read */
	foreach_bit(i, ctx->vtx.vertexbuf.enabled_mask) {
		assert(!ctx->vtx.vertexbuf.vb[i].user_buffer);
		resource_read(batch, ctx->vtx.vertexbuf.vb[i].buffer);
	}

	/* Mark index buffer as being read */
	resource_read(batch, ctx->indexbuf.buffer);

	/* Mark textures as being read */
	foreach_bit(i, ctx->verttex.valid_textures)
		resource_read(batch, ctx->verttex.textures[i]->texture);
	foreach_bit(i, ctx->fragtex.valid_textures)
		resource_read(batch, ctx->fragtex.textures[i]->texture);

	/* Mark streamout buffers as being written.. */
	for (i = 0; i < ctx->streamout.num_targets; i++)
		if (ctx->streamout.targets[i])
			resource_written(batch, ctx->streamout.targets[i]->buffer);

	resource_written(batch, batch->query_buf);

	pipe_mutex_unlock(ctx->screen->lock);

	batch->num_draws++;

	prims = u_reduced_prims_for_vertices(info->mode, info->count);

	ctx->stats.draw_calls++;

	/* TODO prims_emitted should be clipped when the stream-out buffer is
	 * not large enough.  See max_tf_vtx().. probably need to move that
	 * into common code.  Although a bit more annoying since a2xx doesn't
	 * use ir3 so no common way to get at the pipe_stream_output_info
	 * which is needed for this calculation.
	 */
	if (ctx->streamout.num_targets > 0)
		ctx->stats.prims_emitted += prims;
	ctx->stats.prims_generated += prims;

	/* any buffers that haven't been cleared yet, we need to restore: */
	batch->restore |= buffers & (FD_BUFFER_ALL & ~batch->cleared);
	/* and any buffers used, need to be resolved: */
	batch->resolve |= buffers;

	DBG("%p: %x %ux%u num_draws=%u (%s/%s)", batch, buffers,
		pfb->width, pfb->height, batch->num_draws,
		util_format_short_name(pipe_surface_format(pfb->cbufs[0])),
		util_format_short_name(pipe_surface_format(pfb->zsbuf)));

	if (ctx->draw_vbo(ctx, info))
		batch->needs_flush = true;

	for (i = 0; i < ctx->streamout.num_targets; i++)
		ctx->streamout.offsets[i] += info->count;

	if (fd_mesa_debug & FD_DBG_DDRAW)
		ctx->dirty = 0xffffffff;

	fd_batch_check_size(batch);
}

/* TODO figure out how to make better use of existing state mechanism
 * for clear (and possibly gmem->mem / mem->gmem) so we can (a) keep
 * track of what state really actually changes, and (b) reduce the code
 * in the a2xx/a3xx parts.
 */

static void
fd_clear(struct pipe_context *pctx, unsigned buffers,
		const union pipe_color_union *color, double depth, unsigned stencil)
{
	struct fd_context *ctx = fd_context(pctx);
	struct fd_batch *batch = ctx->batch;
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;
	struct pipe_scissor_state *scissor = fd_context_get_scissor(ctx);
	unsigned cleared_buffers;
	int i;

	/* TODO: push down the region versions into the tiles */
	if (!fd_render_condition_check(pctx))
		return;

	if (ctx->in_blit) {
		fd_batch_reset(batch);
		ctx->dirty = ~0;
	}

	/* for bookkeeping about which buffers have been cleared (and thus
	 * can fully or partially skip mem2gmem) we need to ignore buffers
	 * that have already had a draw, in case apps do silly things like
	 * clear after draw (ie. if you only clear the color buffer, but
	 * something like alpha-test causes side effects from the draw in
	 * the depth buffer, etc)
	 */
	cleared_buffers = buffers & (FD_BUFFER_ALL & ~batch->restore);

	/* do we have full-screen scissor? */
	if (!memcmp(scissor, &ctx->disabled_scissor, sizeof(*scissor))) {
		batch->cleared |= cleared_buffers;
	} else {
		batch->partial_cleared |= cleared_buffers;
		if (cleared_buffers & PIPE_CLEAR_COLOR)
			batch->cleared_scissor.color = *scissor;
		if (cleared_buffers & PIPE_CLEAR_DEPTH)
			batch->cleared_scissor.depth = *scissor;
		if (cleared_buffers & PIPE_CLEAR_STENCIL)
			batch->cleared_scissor.stencil = *scissor;
	}
	batch->resolve |= buffers;
	batch->needs_flush = true;

	pipe_mutex_lock(ctx->screen->lock);

	if (buffers & PIPE_CLEAR_COLOR)
		for (i = 0; i < pfb->nr_cbufs; i++)
			if (buffers & (PIPE_CLEAR_COLOR0 << i))
				resource_written(batch, pfb->cbufs[i]->texture);

	if (buffers & (PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL)) {
		resource_written(batch, pfb->zsbuf->texture);
		batch->gmem_reason |= FD_GMEM_CLEARS_DEPTH_STENCIL;
	}

	resource_written(batch, batch->query_buf);

	pipe_mutex_unlock(ctx->screen->lock);

	DBG("%p: %x %ux%u depth=%f, stencil=%u (%s/%s)", batch, buffers,
		pfb->width, pfb->height, depth, stencil,
		util_format_short_name(pipe_surface_format(pfb->cbufs[0])),
		util_format_short_name(pipe_surface_format(pfb->zsbuf)));

	fd_hw_query_set_stage(batch, batch->draw, FD_STAGE_CLEAR);

	ctx->clear(ctx, buffers, color, depth, stencil);

	ctx->dirty |= FD_DIRTY_ZSA |
			FD_DIRTY_VIEWPORT |
			FD_DIRTY_RASTERIZER |
			FD_DIRTY_SAMPLE_MASK |
			FD_DIRTY_PROG |
			FD_DIRTY_CONSTBUF |
			FD_DIRTY_BLEND |
			FD_DIRTY_FRAMEBUFFER;

	if (fd_mesa_debug & FD_DBG_DCLEAR)
		ctx->dirty = 0xffffffff;
}

static void
fd_clear_render_target(struct pipe_context *pctx, struct pipe_surface *ps,
		const union pipe_color_union *color,
		unsigned x, unsigned y, unsigned w, unsigned h,
		bool render_condition_enabled)
{
	DBG("TODO: x=%u, y=%u, w=%u, h=%u", x, y, w, h);
}

static void
fd_clear_depth_stencil(struct pipe_context *pctx, struct pipe_surface *ps,
		unsigned buffers, double depth, unsigned stencil,
		unsigned x, unsigned y, unsigned w, unsigned h,
		bool render_condition_enabled)
{
	DBG("TODO: buffers=%u, depth=%f, stencil=%u, x=%u, y=%u, w=%u, h=%u",
			buffers, depth, stencil, x, y, w, h);
}

void
fd_draw_init(struct pipe_context *pctx)
{
	pctx->draw_vbo = fd_draw_vbo;
	pctx->clear = fd_clear;
	pctx->clear_render_target = fd_clear_render_target;
	pctx->clear_depth_stencil = fd_clear_depth_stencil;
}
