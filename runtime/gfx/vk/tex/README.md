# runtime/gfx/vk/tex

Textures: a cache of NVN images as Vulkan images, uploads with deswizzle and ASTC, descriptor slots and samplers,
sparse and precomputed BC7.

| File | What it is |
|---|---|
| `vk_texture.c`, `vk_tex_internal.h`, `vk_formats.c` | Image cache: its state, keys and statistics; the header is what the `vk_tex_*.c` parts share; NVN to Vulkan format table |
| `vk_tex_image.c` | Image creation, with cube compatibility only for NVN cube targets, and the preallocated texture pool, joined and freed before allocator shutdown |
| `vk_tex_gfx9.c`, `vk_tex_gfx9.h` | The GFX9 RADV decision on block-view COLOR_ATTACHMENT, free of Vulkan so `tests/test_gfx9_block_ca.py` builds it alone |
| `vk_tex_evict.c` | Image destruction, freed entries (kept up to a cap; past it, those a later image was written over go: `MKW_TEX_OVERWRITTEN`), eviction under memory pressure and the list of textures the GPU wrote |
| `vk_tex_upload.c` | Uploads with deswizzle and ASTC, and the check of whether the storage changed |
| `vk_tex_alias.c` | Storage aliases (`MKW_TEX_ALIAS`) and GPU writes to mirrored textures |
| `vk_tex_slots.c` | Texture and sampler descriptor slots, and the VkSamplers |
| `vk_tex_texel.c` | ReadTexels / WriteTexels and CPU reads of what the GPU wrote |
| `vk_astc.c` | ASTC on the GPU: decoded by compute and recompressed to BC7 (`MKW_ASTC_BC7`) |
| `vk_sparse.c` | MKW's own: sparse textures as Vulkan sparse images, tiles bound at each `MapVirtual`; cube compatibility only for cube targets; private small-mip backing initialized separately from shared tiles and sparse resources released before allocator shutdown |
| `vk_texpre.c` | MKW's own: BC7 of the ASTC textures encoded offline (`MKW_TEXPRE`, off) |
