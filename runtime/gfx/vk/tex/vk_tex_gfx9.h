// The GFX9 RADV decision on block-view COLOR_ATTACHMENT: a pure function (no Vulkan), so tests/test_gfx9_block_ca.py
// builds it alone; the why is in vk_tex_gfx9.c.
#ifndef VK_TEX_GFX9_H
#define VK_TEX_GFX9_H

#include <stdint.h>

// 1 when an image of this shape must be made without COLOR_ATTACHMENT on this device: Mesa RADV on GFX9 (RADV's
// pre-RDNA maxImageDimension3D of 2048) and a 3D block-compressed image with mips whose depth reaches the block
// counts, the combination addrlib's swizzle filter empties into a SIGTRAP. Everything else keeps its block views.
int vk_tex_gfx9_skip_block_ca(int mesa_radv, uint32_t vendor_id, uint32_t max_image_dimension_3d, int image_3d,
                              uint32_t mip_levels, uint32_t depth, uint32_t width, uint32_t height, uint32_t block_w,
                              uint32_t block_h);

#endif
