/*
 * Copyright © 2016 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "radv_meta.h"
#include "vk_format.h"
static VkExtent3D
meta_image_block_size(const struct radv_image *image)
{
	const struct vk_format_description *desc = vk_format_description(image->vk_format);
	return (VkExtent3D) { desc->block.width, desc->block.height, 1 };
}

/* Returns the user-provided VkBufferImageCopy::imageExtent in units of
 * elements rather than texels. One element equals one texel or one block
 * if Image is uncompressed or compressed, respectively.
 */
static struct VkExtent3D
meta_region_extent_el(const struct radv_image *image,
                      const struct VkExtent3D *extent)
{
	const VkExtent3D block = meta_image_block_size(image);
	return radv_sanitize_image_extent(image->type, (VkExtent3D) {
			.width  = DIV_ROUND_UP(extent->width , block.width),
				.height = DIV_ROUND_UP(extent->height, block.height),
				.depth  = DIV_ROUND_UP(extent->depth , block.depth),
				});
}

/* Returns the user-provided VkBufferImageCopy::imageOffset in units of
 * elements rather than texels. One element equals one texel or one block
 * if Image is uncompressed or compressed, respectively.
 */
static struct VkOffset3D
meta_region_offset_el(const struct radv_image *image,
                      const struct VkOffset3D *offset)
{
	const VkExtent3D block = meta_image_block_size(image);
	return radv_sanitize_image_offset(image->type, (VkOffset3D) {
			.x = offset->x / block.width,
				.y = offset->y / block.height,
				.z = offset->z / block.depth,
				});
}

static struct radv_meta_blit2d_surf
blit_surf_for_image(const struct radv_image* image,
                    const struct radeon_surf *surf)
{
	int tiling = RADEON_SURF_GET(surf->flags, MODE) == RADEON_SURF_MODE_LINEAR_ALIGNED ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
	return (struct radv_meta_blit2d_surf) {
		.bo = image->bo,
			.base_offset = image->offset,
			.bs = vk_format_get_blocksize(image->vk_format),
			.pitch = surf->level[0].pitch_bytes,
			.tiling = tiling,
			};
}

static struct radv_meta_blit2d_surf
blit_surf_for_image_level(const struct radv_image* image,
			  const struct radeon_surf *surf, int level)
{
	int tiling = RADEON_SURF_GET(surf->flags, MODE) == RADEON_SURF_MODE_LINEAR_ALIGNED ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
	return (struct radv_meta_blit2d_surf) {
		.bo = image->bo,
			.base_offset = image->offset + surf->level[level].offset,
			.bs = vk_format_get_blocksize(image->vk_format),
			.pitch = surf->level[level].pitch_bytes,
			.tiling = tiling,
			};
}

static void
do_buffer_copy(struct radv_cmd_buffer *cmd_buffer,
               struct radv_bo *src, uint64_t src_offset,
               struct radv_bo *dest, uint64_t dest_offset,
               int width, int height, int bs)
{
	struct radv_meta_blit2d_surf b_src = {
		.bo = src,
		.base_offset = src_offset,
		.bs = bs,
		.pitch = width * bs,
		.tiling = VK_IMAGE_TILING_LINEAR,
	};
	struct radv_meta_blit2d_surf b_dst = {
		.bo = dest,
		.base_offset = dest_offset,
		.bs = bs,
		.pitch = width * bs,
		.tiling = VK_IMAGE_TILING_LINEAR,
	};
	struct radv_meta_blit2d_rect rect = {
		.width = width,
		.height = height,
	};
	radv_meta_blit2d(cmd_buffer, &b_src, &b_dst, 1, &rect);
}

static void
meta_copy_buffer_to_image(struct radv_cmd_buffer *cmd_buffer,
                          struct radv_buffer* buffer,
                          struct radv_image* image,
                          uint32_t regionCount,
                          const VkBufferImageCopy* pRegions,
                          bool forward)
{
	struct radv_meta_saved_state saved_state;

	/* The Vulkan 1.0 spec says "dstImage must have a sample count equal to
	 * VK_SAMPLE_COUNT_1_BIT."
	 */
	assert(image->samples == 1);

	radv_meta_begin_blit2d(cmd_buffer, &saved_state);

	for (unsigned r = 0; r < regionCount; r++) {

		/**
		 * From the Vulkan 1.0.6 spec: 18.3 Copying Data Between Images
		 *    extent is the size in texels of the source image to copy in width,
		 *    height and depth. 1D images use only x and width. 2D images use x, y,
		 *    width and height. 3D images use x, y, z, width, height and depth.
		 *
		 *
		 * Also, convert the offsets and extent from units of texels to units of
		 * blocks - which is the highest resolution accessible in this command.
		 */
		const VkOffset3D img_offset_el =
			meta_region_offset_el(image, &pRegions[r].imageOffset);
		const VkExtent3D bufferExtent = {
			.width  = pRegions[r].bufferRowLength ?
			pRegions[r].bufferRowLength : pRegions[r].imageExtent.width,
			.height = pRegions[r].bufferImageHeight ?
			pRegions[r].bufferImageHeight : pRegions[r].imageExtent.height,
		};
		const VkExtent3D buf_extent_el =
			meta_region_extent_el(image, &bufferExtent);

		/* Start creating blit rect */
		const VkExtent3D img_extent_el =
			meta_region_extent_el(image, &pRegions[r].imageExtent);
		struct radv_meta_blit2d_rect rect = {
			.width = img_extent_el.width,
			.height =  img_extent_el.height,
		};

		fprintf(stderr, "blit miplevel: %d\n", pRegions[r].imageSubresource.mipLevel);
		/* Create blit surfaces */
		VkImageAspectFlags aspect = pRegions[r].imageSubresource.aspectMask;
		const struct radeon_surf *img_surf = &image->surface;
		struct radv_meta_blit2d_surf img_bsurf =
			blit_surf_for_image_level(image, img_surf, pRegions[r].imageSubresource.mipLevel);
		struct radv_meta_blit2d_surf buf_bsurf = {
			.bo = buffer->bo,
			.base_offset = buffer->offset + pRegions[r].bufferOffset,
			.bs = img_bsurf.bs,
			.pitch = buf_extent_el.width * buf_bsurf.bs,

			.tiling = VK_IMAGE_TILING_LINEAR,
		};

		/* Set direction-dependent variables */
		struct radv_meta_blit2d_surf *dst_bsurf = forward ? &img_bsurf : &buf_bsurf;
		struct radv_meta_blit2d_surf *src_bsurf = forward ? &buf_bsurf : &img_bsurf;
		uint32_t *x_offset = forward ? &rect.dst_x : &rect.src_x;
		uint32_t *y_offset = forward ? &rect.dst_y : &rect.src_y;

		/* Loop through each 3D or array slice */
		unsigned num_slices_3d = img_extent_el.depth;
		unsigned num_slices_array = pRegions[r].imageSubresource.layerCount;
		unsigned slice_3d = 0;
		unsigned slice_array = 0;
		while (slice_3d < num_slices_3d && slice_array < num_slices_array) {

#if 0
			/* Finish creating blit rect */
			isl_surf_get_image_offset_el(&img_surf->isl,
						     pRegions[r].imageSubresource.mipLevel,
						     pRegions[r].imageSubresource.baseArrayLayer
						     + slice_array,
						     img_offset_el.z + slice_3d,
						     x_offset,
						     y_offset);
#endif
			*x_offset += img_offset_el.x;
			*y_offset += img_offset_el.y;

			/* Perform Blit */
			radv_meta_blit2d(cmd_buffer, src_bsurf, dst_bsurf, 1, &rect);

			/* Once we've done the blit, all of the actual information about
			 * the image is embedded in the command buffer so we can just
			 * increment the offset directly in the image effectively
			 * re-binding it to different backing memory.
			 */
			buf_bsurf.base_offset += buf_extent_el.width *
				buf_extent_el.height * buf_bsurf.bs;

			if (image->type == VK_IMAGE_TYPE_3D)
				slice_3d++;
			else
				slice_array++;
		}
	}
	radv_meta_end_blit2d(cmd_buffer, &saved_state);
}

void radv_CmdCopyBufferToImage(
	VkCommandBuffer                             commandBuffer,
	VkBuffer                                    srcBuffer,
	VkImage                                     destImage,
	VkImageLayout                               destImageLayout,
	uint32_t                                    regionCount,
	const VkBufferImageCopy*                    pRegions)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_image, dest_image, destImage);
	RADV_FROM_HANDLE(radv_buffer, src_buffer, srcBuffer);

	meta_copy_buffer_to_image(cmd_buffer, src_buffer, dest_image,
				  regionCount, pRegions, true);
}

void radv_CmdCopyImageToBuffer(
	VkCommandBuffer                             commandBuffer,
	VkImage                                     srcImage,
	VkImageLayout                               srcImageLayout,
	VkBuffer                                    destBuffer,
	uint32_t                                    regionCount,
	const VkBufferImageCopy*                    pRegions)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_image, src_image, srcImage);
	RADV_FROM_HANDLE(radv_buffer, dst_buffer, destBuffer);

	meta_copy_buffer_to_image(cmd_buffer, dst_buffer, src_image,
				  regionCount, pRegions, false);
}

void radv_CmdCopyImage(
	VkCommandBuffer                             commandBuffer,
	VkImage                                     srcImage,
	VkImageLayout                               srcImageLayout,
	VkImage                                     destImage,
	VkImageLayout                               destImageLayout,
	uint32_t                                    regionCount,
	const VkImageCopy*                          pRegions)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_image, src_image, srcImage);
	RADV_FROM_HANDLE(radv_image, dest_image, destImage);
	struct radv_meta_saved_state saved_state;

	/* From the Vulkan 1.0 spec:
	 *
	 *    vkCmdCopyImage can be used to copy image data between multisample
	 *    images, but both images must have the same number of samples.
	 */
	assert(src_image->samples == dest_image->samples);

	radv_meta_begin_blit2d(cmd_buffer, &saved_state);

	for (unsigned r = 0; r < regionCount; r++) {
		assert(pRegions[r].srcSubresource.aspectMask ==
		       pRegions[r].dstSubresource.aspectMask);

		VkImageAspectFlags aspect = pRegions[r].srcSubresource.aspectMask;

		/* Create blit surfaces */
		struct radeon_surf *src_surf = &src_image->surface;
		struct radeon_surf *dst_surf = &dest_image->surface;
		struct radv_meta_blit2d_surf b_src =
			blit_surf_for_image(src_image, src_surf);
		struct radv_meta_blit2d_surf b_dst =
			blit_surf_for_image(dest_image, dst_surf);

		/**
		 * From the Vulkan 1.0.6 spec: 18.4 Copying Data Between Buffers and Images
		 *    imageExtent is the size in texels of the image to copy in width, height
		 *    and depth. 1D images use only x and width. 2D images use x, y, width
		 *    and height. 3D images use x, y, z, width, height and depth.
		 *
		 * Also, convert the offsets and extent from units of texels to units of
		 * blocks - which is the highest resolution accessible in this command.
		 */
		const VkOffset3D dst_offset_el =
			meta_region_offset_el(dest_image, &pRegions[r].dstOffset);
		const VkOffset3D src_offset_el =
			meta_region_offset_el(src_image, &pRegions[r].srcOffset);
		const VkExtent3D img_extent_el =
			meta_region_extent_el(src_image, &pRegions[r].extent);

		/* Start creating blit rect */
		struct radv_meta_blit2d_rect rect = {
			.width = img_extent_el.width,
			.height = img_extent_el.height,
		};

		/* Loop through each 3D or array slice */
		unsigned num_slices_3d = img_extent_el.depth;
		unsigned num_slices_array = pRegions[r].dstSubresource.layerCount;
		unsigned slice_3d = 0;
		unsigned slice_array = 0;
		while (slice_3d < num_slices_3d && slice_array < num_slices_array) {

			/* Finish creating blit rect */
#if 0
			isl_surf_get_image_offset_el(&dst_surf->isl,
						     pRegions[r].dstSubresource.mipLevel,
						     pRegions[r].dstSubresource.baseArrayLayer
						     + slice_array,
						     dst_offset_el.z + slice_3d,
						     &rect.dst_x,
						     &rect.dst_y);
			isl_surf_get_image_offset_el(&src_surf->isl,
						     pRegions[r].srcSubresource.mipLevel,
						     pRegions[r].srcSubresource.baseArrayLayer
						     + slice_array,
						     src_offset_el.z + slice_3d,
						     &rect.src_x,
						     &rect.src_y);
#endif
			rect.dst_x += dst_offset_el.x;
			rect.dst_y += dst_offset_el.y;
			rect.src_x += src_offset_el.x;
			rect.src_y += src_offset_el.y;

			/* Perform Blit */
			radv_meta_blit2d(cmd_buffer, &b_src, &b_dst, 1, &rect);

			if (dest_image->type == VK_IMAGE_TYPE_3D)
				slice_3d++;
			else
				slice_array++;
		}
	}

	radv_meta_end_blit2d(cmd_buffer, &saved_state);
}

void radv_CmdCopyBuffer(
	VkCommandBuffer                             commandBuffer,
	VkBuffer                                    srcBuffer,
	VkBuffer                                    destBuffer,
	uint32_t                                    regionCount,
	const VkBufferCopy*                         pRegions)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_buffer, src_buffer, srcBuffer);
	RADV_FROM_HANDLE(radv_buffer, dest_buffer, destBuffer);

	struct radv_meta_saved_state saved_state;

	radv_meta_begin_blit2d(cmd_buffer, &saved_state);

	for (unsigned r = 0; r < regionCount; r++) {
		uint64_t src_offset = src_buffer->offset + pRegions[r].srcOffset;
		uint64_t dest_offset = dest_buffer->offset + pRegions[r].dstOffset;
		uint64_t copy_size = pRegions[r].size;

		/* First, we compute the biggest format that can be used with the
		 * given offsets and size.
		 */
		int bs = 16;

		int fs = ffs(src_offset) - 1;
		if (fs != -1)
			bs = MIN2(bs, 1 << fs);
		assert(src_offset % bs == 0);

		fs = ffs(dest_offset) - 1;
		if (fs != -1)
			bs = MIN2(bs, 1 << fs);
		assert(dest_offset % bs == 0);

		fs = ffs(pRegions[r].size) - 1;
		if (fs != -1)
			bs = MIN2(bs, 1 << fs);
		assert(pRegions[r].size % bs == 0);

		/* This is maximum possible width/height our HW can handle */
		uint64_t max_surface_dim = 1 << 14;

		/* First, we make a bunch of max-sized copies */
		uint64_t max_copy_size = max_surface_dim * max_surface_dim * bs;
		while (copy_size >= max_copy_size) {
			do_buffer_copy(cmd_buffer, src_buffer->bo, src_offset,
				       dest_buffer->bo, dest_offset,
				       max_surface_dim, max_surface_dim, bs);
			copy_size -= max_copy_size;
			src_offset += max_copy_size;
			dest_offset += max_copy_size;
		}

		uint64_t height = copy_size / (max_surface_dim * bs);
		assert(height < max_surface_dim);
		if (height != 0) {
			uint64_t rect_copy_size = height * max_surface_dim * bs;
			do_buffer_copy(cmd_buffer, src_buffer->bo, src_offset,
				       dest_buffer->bo, dest_offset,
				       max_surface_dim, height, bs);
			copy_size -= rect_copy_size;
			src_offset += rect_copy_size;
			dest_offset += rect_copy_size;
		}

		if (copy_size != 0) {
			do_buffer_copy(cmd_buffer, src_buffer->bo, src_offset,
				       dest_buffer->bo, dest_offset,
				       copy_size / bs, 1, bs);
		}
	}

	radv_meta_end_blit2d(cmd_buffer, &saved_state);
}

void radv_CmdUpdateBuffer(
	VkCommandBuffer                             commandBuffer,
	VkBuffer                                    dstBuffer,
	VkDeviceSize                                dstOffset,
	VkDeviceSize                                dataSize,
	const uint32_t*                             pData)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_buffer, dst_buffer, dstBuffer);
	struct radv_meta_saved_state saved_state;

	radv_meta_begin_blit2d(cmd_buffer, &saved_state);

	/* We can't quite grab a full block because the state stream needs a
	 * little data at the top to build its linked list.
	 */
#if 0
	const uint32_t max_update_size =
		cmd_buffer->device->dynamic_state_block_pool.block_size - 64;

	assert(max_update_size < (1 << 14) * 4);
#endif
	const uint32_t max_update_size = 0;
	while (dataSize) {
		const uint32_t copy_size = MIN2(dataSize, max_update_size);
#if 0 
		struct radv_state tmp_data =
			radv_cmd_buffer_alloc_dynamic_state(cmd_buffer, copy_size, 64);
		memcpy(tmp_data.map, pData, copy_size);
#endif


		int bs;
		if ((copy_size & 15) == 0 && (dstOffset & 15) == 0) {
			bs = 16;
		} else if ((copy_size & 7) == 0 && (dstOffset & 7) == 0) {
			bs = 8;
		} else {
			assert((copy_size & 3) == 0 && (dstOffset & 3) == 0);
			bs = 4;
		}

		do_buffer_copy(cmd_buffer,
			       NULL,
#if 0
			       &cmd_buffer->device->dynamic_state_block_pool.bo,
#endif
			       0,//tmp_data.offset,
			       dst_buffer->bo, dst_buffer->offset + dstOffset,
			       copy_size / bs, 1, bs);

		dataSize -= copy_size;
		dstOffset += copy_size;
		pData = (void *)pData + copy_size;
	}

	radv_meta_end_blit2d(cmd_buffer, &saved_state);
}