/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
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

#include "radv_private.h"

#include "vk_format.h"
#include "sid.h"
#include "r600d_common.h"
uint32_t radv_translate_buffer_dataformat(const struct vk_format_description *desc,
					  int first_non_void)
{
    unsigned type;
    int i;

    assert(first_non_void >= 0);
    type = desc->channel[first_non_void].type;

    if (type == VK_FORMAT_TYPE_FIXED)
	return V_008F0C_BUF_DATA_FORMAT_INVALID;
    if (desc->nr_channels == 4 &&
	desc->channel[0].size == 10 &&
	desc->channel[1].size == 10 &&
	desc->channel[2].size == 10 &&
	desc->channel[3].size == 2)
	return V_008F0C_BUF_DATA_FORMAT_2_10_10_10;

    /* See whether the components are of the same size. */
    for (i = 0; i < desc->nr_channels; i++) {
	if (desc->channel[first_non_void].size != desc->channel[i].size)
	    return V_008F0C_BUF_DATA_FORMAT_INVALID;
    }

    switch (desc->channel[first_non_void].size) {
    case 8:
	switch (desc->nr_channels) {
	case 1:
	    return V_008F0C_BUF_DATA_FORMAT_8;
	case 2:
	    return V_008F0C_BUF_DATA_FORMAT_8_8;
	case 3:
	case 4:
	    return V_008F0C_BUF_DATA_FORMAT_8_8_8_8;
	}
	break;
    case 16:
	switch (desc->nr_channels) {
	case 1:
	    return V_008F0C_BUF_DATA_FORMAT_16;
	case 2:
	    return V_008F0C_BUF_DATA_FORMAT_16_16;
	case 3:
	case 4:
	    return V_008F0C_BUF_DATA_FORMAT_16_16_16_16;
	}
	break;
    case 32:
	/* From the Southern Islands ISA documentation about MTBUF:
	 * 'Memory reads of data in memory that is 32 or 64 bits do not
	 * undergo any format conversion.'
	 */
	if (type != VK_FORMAT_TYPE_FLOAT &&
	    !desc->channel[first_non_void].pure_integer)
	    return V_008F0C_BUF_DATA_FORMAT_INVALID;

	switch (desc->nr_channels) {
	case 1:
	    return V_008F0C_BUF_DATA_FORMAT_32;
	case 2:
	    return V_008F0C_BUF_DATA_FORMAT_32_32;
	case 3:
	    return V_008F0C_BUF_DATA_FORMAT_32_32_32;
	case 4:
	    return V_008F0C_BUF_DATA_FORMAT_32_32_32_32;
	}
	break;
    }

    return V_008F0C_BUF_DATA_FORMAT_INVALID;
}

uint32_t radv_translate_buffer_numformat(const struct vk_format_description *desc,
					 int first_non_void)
{
    //	if (desc->format == PIPE_FORMAT_R11G11B10_FLOAT)
    //		return V_008F0C_BUF_NUM_FORMAT_FLOAT;

	assert(first_non_void >= 0);

	switch (desc->channel[first_non_void].type) {
	case VK_FORMAT_TYPE_SIGNED:
		if (desc->channel[first_non_void].normalized)
			return V_008F0C_BUF_NUM_FORMAT_SNORM;
		else if (desc->channel[first_non_void].pure_integer)
			return V_008F0C_BUF_NUM_FORMAT_SINT;
		else
			return V_008F0C_BUF_NUM_FORMAT_SSCALED;
		break;
	case VK_FORMAT_TYPE_UNSIGNED:
		if (desc->channel[first_non_void].normalized)
			return V_008F0C_BUF_NUM_FORMAT_UNORM;
		else if (desc->channel[first_non_void].pure_integer)
			return V_008F0C_BUF_NUM_FORMAT_UINT;
		else
			return V_008F0C_BUF_NUM_FORMAT_USCALED;
		break;
	case VK_FORMAT_TYPE_FLOAT:
	default:
		return V_008F0C_BUF_NUM_FORMAT_FLOAT;
	}
}

uint32_t radv_translate_texformat(VkFormat format,
				  const struct vk_format_description *desc,
				  int first_non_void)
{
   bool uniform = true;
   int i;

   if (!desc)
      return ~0;
   /* Colorspace (return non-RGB formats directly). */
   switch (desc->colorspace) {
      /* Depth stencil formats */
   case VK_FORMAT_COLORSPACE_ZS:
      switch (format) {
      case VK_FORMAT_D16_UNORM:
	 return V_008F14_IMG_DATA_FORMAT_16;
      case VK_FORMAT_D24_UNORM_S8_UINT:
	 return V_008F14_IMG_DATA_FORMAT_8_24;
      case VK_FORMAT_S8_UINT:
	 return V_008F14_IMG_DATA_FORMAT_8;
      case VK_FORMAT_D32_SFLOAT:
	 return V_008F14_IMG_DATA_FORMAT_32;
      case VK_FORMAT_D32_SFLOAT_S8_UINT:
	 return V_008F14_IMG_DATA_FORMAT_X24_8_32;
      default:
	 goto out_unknown;
      }

   case VK_FORMAT_COLORSPACE_YUV:
      goto out_unknown; /* TODO */

   case VK_FORMAT_COLORSPACE_SRGB:
      if (desc->nr_channels != 4 && desc->nr_channels != 1)
	 goto out_unknown;
      break;

   default:
      break;
   }

   if (desc->layout == VK_FORMAT_LAYOUT_S3TC) {
       switch(format) {
       case VK_FORMAT_BC2_UNORM_BLOCK:
       case VK_FORMAT_BC2_SRGB_BLOCK:
	   return V_008F14_IMG_DATA_FORMAT_BC2;
       case VK_FORMAT_BC3_UNORM_BLOCK:
       case VK_FORMAT_BC3_SRGB_BLOCK:
	   return V_008F14_IMG_DATA_FORMAT_BC3;
       default:
	   break;
       }
   }

   if (format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) {
      return V_008F14_IMG_DATA_FORMAT_5_9_9_9;
   } else if (format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
      return V_008F14_IMG_DATA_FORMAT_10_11_11;
   }

   /* R8G8Bx_SNORM - TODO CxV8U8 */

   /* hw cannot support mixed formats (except depth/stencil, since only
    * depth is read).*/
   if (desc->is_mixed && desc->colorspace != VK_FORMAT_COLORSPACE_ZS)
      goto out_unknown;

   /* See whether the components are of the same size. */
   for (i = 1; i < desc->nr_channels; i++) {
      uniform = uniform && desc->channel[0].size == desc->channel[i].size;
   }

   /* Non-uniform formats. */
   if (!uniform) {
      switch(desc->nr_channels) {
      case 3:
	 if (desc->channel[0].size == 5 &&
	     desc->channel[1].size == 6 &&
	     desc->channel[2].size == 5) {
	    return V_008F14_IMG_DATA_FORMAT_5_6_5;
	 }
	 goto out_unknown;
      case 4:
	 if (desc->channel[0].size == 5 &&
	     desc->channel[1].size == 5 &&
	     desc->channel[2].size == 5 &&
	     desc->channel[3].size == 1) {
	    return V_008F14_IMG_DATA_FORMAT_1_5_5_5;
	 }
	 if (desc->channel[0].size == 10 &&
	     desc->channel[1].size == 10 &&
	     desc->channel[2].size == 10 &&
	     desc->channel[3].size == 2) {
	    return V_008F14_IMG_DATA_FORMAT_2_10_10_10;
	 }
	 goto out_unknown;
      }
      goto out_unknown;
   }

   if (first_non_void < 0 || first_non_void > 3)
      goto out_unknown;

   /* uniform formats */
   switch (desc->channel[first_non_void].size) {
   case 4:
      switch (desc->nr_channels) {
#if 0 /* Not supported for render targets */
      case 2:
	 return V_008F14_IMG_DATA_FORMAT_4_4;
#endif
      case 4:
	 return V_008F14_IMG_DATA_FORMAT_4_4_4_4;
      }
      break;
   case 8:
      switch (desc->nr_channels) {
      case 1:
	 return V_008F14_IMG_DATA_FORMAT_8;
      case 2:
	 return V_008F14_IMG_DATA_FORMAT_8_8;
      case 4:
	 return V_008F14_IMG_DATA_FORMAT_8_8_8_8;
      }
      break;
   case 16:
      switch (desc->nr_channels) {
      case 1:
	 return V_008F14_IMG_DATA_FORMAT_16;
      case 2:
	 return V_008F14_IMG_DATA_FORMAT_16_16;
      case 4:
	 return V_008F14_IMG_DATA_FORMAT_16_16_16_16;
      }
      break;
   case 32:
      switch (desc->nr_channels) {
      case 1:
	 return V_008F14_IMG_DATA_FORMAT_32;
      case 2:
	 return V_008F14_IMG_DATA_FORMAT_32_32;
#if 0 /* Not supported for render targets */
      case 3:
	 return V_008F14_IMG_DATA_FORMAT_32_32_32;
#endif
      case 4:
	 return V_008F14_IMG_DATA_FORMAT_32_32_32_32;
      }
   }

 out_unknown:
   /* R600_ERR("Unable to handle texformat %d %s\n", format, vk_format_name(format)); */
   return ~0;
}

static bool radv_is_sampler_format_supported(struct radv_physical_device *physical_device,
					     VkFormat format)
{
   const struct vk_format_description *desc = vk_format_description(format);
   if (!desc || format == VK_FORMAT_UNDEFINED)
      return false;
   return radv_translate_texformat(format, vk_format_description(format),
				   vk_format_get_first_non_void_channel(format)) != ~0U;
}

static void
radv_physical_device_get_format_properties(struct radv_physical_device *physical_device,
                                          VkFormat format,
                                          VkFormatProperties *out_properties)
{
   VkFormatFeatureFlags linear = 0, tiled = 0, buffer = 0;
   const struct vk_format_description *desc = vk_format_description(format);

   if (!desc) {
	   out_properties->linearTilingFeatures = linear;
	   out_properties->optimalTilingFeatures = tiled;
	   out_properties->bufferFeatures = buffer;
	   return;
   }
   if (vk_format_is_depth_or_stencil(format)) {
     tiled |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
     tiled |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
     tiled |= VK_FORMAT_FEATURE_BLIT_SRC_BIT |
       VK_FORMAT_FEATURE_BLIT_DST_BIT;
   } else {
     if (radv_is_sampler_format_supported(physical_device, format)) {
       linear |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
	 VK_FORMAT_FEATURE_BLIT_SRC_BIT;
       tiled |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
	 VK_FORMAT_FEATURE_BLIT_SRC_BIT;
     }
   }
   out_properties->linearTilingFeatures = linear;
   out_properties->optimalTilingFeatures = tiled;
   out_properties->bufferFeatures = buffer;
}

uint32_t radv_translate_colorformat(VkFormat format)
{
	const struct vk_format_description *desc = vk_format_description(format);

#define HAS_SIZE(x,y,z,w) \
	(desc->channel[0].size == (x) && desc->channel[1].size == (y) && \
         desc->channel[2].size == (z) && desc->channel[3].size == (w))

	//	if (format == PIPE_FORMAT_R11G11B10_FLOAT) /* isn't plain */
	//		return V_028C70_COLOR_10_11_11;

	if (desc->layout != VK_FORMAT_LAYOUT_PLAIN)
		return V_028C70_COLOR_INVALID;

	/* hw cannot support mixed formats (except depth/stencil, since
	 * stencil is not written to). */
	if (desc->is_mixed && desc->colorspace != VK_FORMAT_COLORSPACE_ZS)
		return V_028C70_COLOR_INVALID;

	switch (desc->nr_channels) {
	case 1:
		switch (desc->channel[0].size) {
		case 8:
			return V_028C70_COLOR_8;
		case 16:
			return V_028C70_COLOR_16;
		case 32:
			return V_028C70_COLOR_32;
		}
		break;
	case 2:
		if (desc->channel[0].size == desc->channel[1].size) {
			switch (desc->channel[0].size) {
			case 8:
				return V_028C70_COLOR_8_8;
			case 16:
				return V_028C70_COLOR_16_16;
			case 32:
				return V_028C70_COLOR_32_32;
			}
		} else if (HAS_SIZE(8,24,0,0)) {
			return V_028C70_COLOR_24_8;
		} else if (HAS_SIZE(24,8,0,0)) {
			return V_028C70_COLOR_8_24;
		}
		break;
	case 3:
		if (HAS_SIZE(5,6,5,0)) {
			return V_028C70_COLOR_5_6_5;
		} else if (HAS_SIZE(32,8,24,0)) {
			return V_028C70_COLOR_X24_8_32_FLOAT;
		}
		break;
	case 4:
		if (desc->channel[0].size == desc->channel[1].size &&
		    desc->channel[0].size == desc->channel[2].size &&
		    desc->channel[0].size == desc->channel[3].size) {
			switch (desc->channel[0].size) {
			case 4:
				return V_028C70_COLOR_4_4_4_4;
			case 8:
				return V_028C70_COLOR_8_8_8_8;
			case 16:
				return V_028C70_COLOR_16_16_16_16;
			case 32:
				return V_028C70_COLOR_32_32_32_32;
			}
		} else if (HAS_SIZE(5,5,5,1)) {
			return V_028C70_COLOR_1_5_5_5;
		} else if (HAS_SIZE(10,10,10,2)) {
			return V_028C70_COLOR_2_10_10_10;
		}
		break;
	}
	return V_028C70_COLOR_INVALID;
}

uint32_t radv_colorformat_endian_swap(uint32_t colorformat)
{
    if (0/*SI_BIG_ENDIAN*/) {
		switch(colorformat) {
		/* 8-bit buffers. */
		case V_028C70_COLOR_8:
			return V_028C70_ENDIAN_NONE;

		/* 16-bit buffers. */
		case V_028C70_COLOR_5_6_5:
		case V_028C70_COLOR_1_5_5_5:
		case V_028C70_COLOR_4_4_4_4:
		case V_028C70_COLOR_16:
		case V_028C70_COLOR_8_8:
			return V_028C70_ENDIAN_8IN16;

		/* 32-bit buffers. */
		case V_028C70_COLOR_8_8_8_8:
		case V_028C70_COLOR_2_10_10_10:
		case V_028C70_COLOR_8_24:
		case V_028C70_COLOR_24_8:
		case V_028C70_COLOR_16_16:
			return V_028C70_ENDIAN_8IN32;

		/* 64-bit buffers. */
		case V_028C70_COLOR_16_16_16_16:
			return V_028C70_ENDIAN_8IN16;

		case V_028C70_COLOR_32_32:
			return V_028C70_ENDIAN_8IN32;

		/* 128-bit buffers. */
		case V_028C70_COLOR_32_32_32_32:
			return V_028C70_ENDIAN_8IN32;
		default:
			return V_028C70_ENDIAN_NONE; /* Unsupported. */
		}
	} else {
		return V_028C70_ENDIAN_NONE;
	}
}

uint32_t radv_translate_dbformat(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_D16_UNORM:
		return V_028040_Z_16;
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D24_UNORM_S8_UINT:
		return V_028040_Z_24; /* deprecated on SI */
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return V_028040_Z_32_FLOAT;
	default:
		return V_028040_Z_INVALID;
	}
}

unsigned radv_translate_colorswap(VkFormat format, bool do_endian_swap)
{
	const struct vk_format_description *desc = vk_format_description(format);

#define HAS_SWIZZLE(chan,swz) (desc->swizzle[chan] == VK_SWIZZLE_##swz)

	//	if (format == PIPE_FORMAT_R11G11B10_FLOAT) /* isn't plain */
	//		return V_0280A0_SWAP_STD;

	if (desc->layout != VK_FORMAT_LAYOUT_PLAIN)
		return ~0U;

	switch (desc->nr_channels) {
	case 1:
		if (HAS_SWIZZLE(0,X))
			return V_0280A0_SWAP_STD; /* X___ */
		else if (HAS_SWIZZLE(3,X))
			return V_0280A0_SWAP_ALT_REV; /* ___X */
		break;
	case 2:
		if ((HAS_SWIZZLE(0,X) && HAS_SWIZZLE(1,Y)) ||
		    (HAS_SWIZZLE(0,X) && HAS_SWIZZLE(1,NONE)) ||
		    (HAS_SWIZZLE(0,NONE) && HAS_SWIZZLE(1,Y)))
			return V_0280A0_SWAP_STD; /* XY__ */
		else if ((HAS_SWIZZLE(0,Y) && HAS_SWIZZLE(1,X)) ||
			 (HAS_SWIZZLE(0,Y) && HAS_SWIZZLE(1,NONE)) ||
		         (HAS_SWIZZLE(0,NONE) && HAS_SWIZZLE(1,X)))
			/* YX__ */
			return (do_endian_swap ? V_0280A0_SWAP_STD : V_0280A0_SWAP_STD_REV);
		else if (HAS_SWIZZLE(0,X) && HAS_SWIZZLE(3,Y))
			return V_0280A0_SWAP_ALT; /* X__Y */
		else if (HAS_SWIZZLE(0,Y) && HAS_SWIZZLE(3,X))
			return V_0280A0_SWAP_ALT_REV; /* Y__X */
		break;
	case 3:
		if (HAS_SWIZZLE(0,X))
			return (do_endian_swap ? V_0280A0_SWAP_STD_REV : V_0280A0_SWAP_STD);
		else if (HAS_SWIZZLE(0,Z))
			return V_0280A0_SWAP_STD_REV; /* ZYX */
		break;
	case 4:
		/* check the middle channels, the 1st and 4th channel can be NONE */
		if (HAS_SWIZZLE(1,Y) && HAS_SWIZZLE(2,Z)) {
			return V_0280A0_SWAP_STD; /* XYZW */
		} else if (HAS_SWIZZLE(1,Z) && HAS_SWIZZLE(2,Y)) {
			return V_0280A0_SWAP_STD_REV; /* WZYX */
		} else if (HAS_SWIZZLE(1,Y) && HAS_SWIZZLE(2,X)) {
			return V_0280A0_SWAP_ALT; /* ZYXW */
		} else if (HAS_SWIZZLE(1,Z) && HAS_SWIZZLE(2,W)) {
			/* YZWX */
			if (desc->is_array)
				return V_0280A0_SWAP_ALT_REV;
			else
				return (do_endian_swap ? V_0280A0_SWAP_ALT : V_0280A0_SWAP_ALT_REV);
		}
		break;
	}
	return ~0U;
}

void radv_GetPhysicalDeviceFormatProperties(
    VkPhysicalDevice                            physicalDevice,
    VkFormat                                    format,
    VkFormatProperties*                         pFormatProperties)
{
   RADV_FROM_HANDLE(radv_physical_device, physical_device, physicalDevice);

   radv_physical_device_get_format_properties(physical_device,
					      format,
					      pFormatProperties);
}

VkResult radv_GetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice                            physicalDevice,
    VkFormat                                    format,
    VkImageType                                 type,
    VkImageTiling                               tiling,
    VkImageUsageFlags                           usage,
    VkImageCreateFlags                          createFlags,
    VkImageFormatProperties*                    pImageFormatProperties)
{
	RADV_FROM_HANDLE(radv_physical_device, physical_device, physicalDevice);
	VkFormatProperties format_props;
	VkFormatFeatureFlags format_feature_flags;
	VkExtent3D maxExtent;
	uint32_t maxMipLevels;
	uint32_t maxArraySize;
	VkSampleCountFlags sampleCounts = VK_SAMPLE_COUNT_1_BIT;

	radv_physical_device_get_format_properties(physical_device, format,
						   &format_props);

	switch (type) {
	default:
		unreachable("bad vkimage type\n");
	case VK_IMAGE_TYPE_1D:
		maxExtent.width = 16384;
		maxExtent.height = 1;
		maxExtent.depth = 1;
		maxMipLevels = 15; /* log2(maxWidth) + 1 */
		maxArraySize = 2048;
		sampleCounts = VK_SAMPLE_COUNT_1_BIT;
		break;
	case VK_IMAGE_TYPE_2D:
		maxExtent.width = 16384;
		maxExtent.height = 16384;
		maxExtent.depth = 1;
		maxMipLevels = 15; /* log2(maxWidth) + 1 */
		maxArraySize = 2048;
		break;
	case VK_IMAGE_TYPE_3D:
		maxExtent.width = 2048;
		maxExtent.height = 2048;
		maxExtent.depth = 1;
		maxMipLevels = 12; /* log2(maxWidth) + 1 */
		maxArraySize = 1;
		break;
	}

	*pImageFormatProperties = (VkImageFormatProperties) {
		.maxExtent = maxExtent,
		.maxMipLevels = maxMipLevels,
		.maxArrayLayers = maxArraySize,
		.sampleCounts = sampleCounts,

		/* FINISHME: Accurately calculate
		 * VkImageFormatProperties::maxResourceSize.
		 */
		.maxResourceSize = UINT32_MAX,
	};

	return VK_SUCCESS;
}

void radv_GetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice                            physicalDevice,
    VkFormat                                    format,
    VkImageType                                 type,
    uint32_t                                    samples,
    VkImageUsageFlags                           usage,
    VkImageTiling                               tiling,
    uint32_t*                                   pNumProperties,
    VkSparseImageFormatProperties*              pProperties)
{
   /* Sparse images are not yet supported. */
   *pNumProperties = 0;
}