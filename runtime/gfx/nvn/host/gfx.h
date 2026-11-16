// Interface of the graphics layer: window, shaders, back ends and the NVN front end.
#pragma once
#include <stdint.h>
#include "host.h"
#include "nvn_obj.h"

// ---- moved from ov.h
enum { OE_MSG_FOCUS_STATE_CHANGED = 15, OE_MSG_OPERATION_MODE_CHANGED = 30, OE_MSG_PERFORMANCE_MODE_CHANGED = 31 };
void oe_push_message(uint32_t message);                   // ov_system.c
void oe_set_focus(int in_focus);                          // ov_system.c: GetCurrentFocusState; MKW_ASSUME_FOCUS pins 1
typedef struct { uint64_t alloc, free, realloc, user; } OvNvAllocator;
typedef struct { OvNvAllocator graphics, devtools; uint64_t memory, memory_size; int initialized; } OvNvState;
const OvNvState *ov_nv_state(void);                        // ov_gfx.c

// ---- GPU address window for CPU-invisible pools (section 3.1)
#define RT_GPU_VA_BASE 0x8000000000ull                     // 2^39, never mapped on the host
#define RT_GPU_VA_SIZE (64ull << 30)

// ---- window, vsync, focus (gfx_present.c and the window back end, gfx_wsys.h; vk_device.c). Host threads only; never run guest code.
void gfx_window_start(void);                               // window thread, Vulkan device, swapchain; idempotent
void gfx_window_early(void);                               // the window at boot, black until the first image
void gfx_vsync_bind(uint64_t system_event);                // nn::vi vsync event; signalled at 60 Hz
int gfx_hdr_output(uint16_t *peak, uint16_t *white);       // 1: the window shows HDR10 (peak, paper white nits)
void gfx_hdr_to_srgb8(const uint8_t *in, uint8_t *out, size_t px, int a2r);   // 10-bit PQ to sRGB bytes at paper white
int gfx_window_focused(void);
int gfx_window_lines(void);   // lines of the picture the window will show (monitor in fullscreen); 0 unknown
int gfx_window_shape(int *w, int *h);   // its width and lines; 0 unknown (w and h then untouched)

// ---- prepared shaders (gfx_shader.c)
enum { GFX_STAGE_VS, GFX_STAGE_TCS, GFX_STAGE_TES, GFX_STAGE_GS, GFX_STAGE_FS, GFX_STAGE_CS };
enum { GFX_MESH_NONE, GFX_MESH_TASK, GFX_MESH_MESH };      // gfx_shader_mesh_kind
enum { GFX_TYPE_FLOAT, GFX_TYPE_SINT, GFX_TYPE_UINT };
typedef struct {                                           // what a draw's state says of its program
    uint8_t attr_type[16];                                 // GFX_TYPE_* of each vertex attribute's format: integer
                                                           // ones get an integer-input variant (int_input_variant)
    uint8_t out_type[8];                                   // fragment: GFX_TYPE_* of each colour target's format,
                                                           // integer ones an integer-output variant (int_output_variant)
    uint64_t img_uint, img_sint;                           // compute: bindings (bit = binding) of the TEX/IMG whose
                                                           // bound view has a uint / sint format (int_image_variant)
} GfxShaderAnswers;
typedef struct GfxShader GfxShader;                        // SPIR-V words + parsed reflection; owned by the cache
// The prepared SPIR-V of a program (tools/shaders/translate.py), as its integer-input variant for the attribute types
// in a; NULL when the program has none (logged). Loads it on its first use.
const GfxShader *gfx_shader_get(const uint8_t *control, const uint8_t *code, const GfxShaderAnswers *a);
// The integer variant of a prepared vertex or fragment shader for the attribute or target types in a, or s (pipeline
// key records).
const GfxShader *gfx_shader_int_variant(const GfxShader *s, const GfxShaderAnswers *a);
// gfx_spirv_io.c: w with the variables of storage class sc (1 Input, 3 Output) at the Locations in umask / smask
// retyped to uvec4 / ivec4, each load bitcast back to float and each stored value to the integer type
// (int_input_variant, int_output_variant); NULL when it cannot. The words are malloc'd.
uint32_t *spirv_int_io(const uint32_t *w, uint64_t n, uint32_t umask, uint32_t smask, uint32_t sc, uint64_t *out_n);
// gfx_spirv_img.c: w with the image bindings in umask / smask retyped to uint / int images (int_image_variant); NULL
// when it cannot (logged by the caller). The words are malloc'd. why, when supplied, receives a static failure reason;
// out_n is zero on failure. Neither diagnostic changes the input module.
uint32_t *spirv_int_images(const uint32_t *w, uint64_t n, uint64_t umask, uint64_t smask, uint64_t *out_n,
                           const char **why);
uint64_t gfx_shader_count(void);                           // shaders in memory (MKW_TRACE=perf, flight recorder)
const uint8_t *gfx_shader_key(const GfxShader *s);          // the 32-byte cache key (program key; variants their own)
const uint8_t *gfx_shader_input_id(const GfxShader *s);     // the 32-byte id in pipeline key records: the program key
// By program key from memory or the prepared directory. NULL if absent.
const GfxShader *gfx_shader_load_input(const uint8_t iid[32]);
// Boot prewarm of the pipeline key records' shaders into out[i]; off with prepared SPIR-V (all NULL, gfx_shader.c).
void gfx_shader_prewarm(const uint8_t (*iids)[32], size_t n, const GfxShader **out);
const uint32_t *gfx_shader_spirv(const GfxShader *s, uint64_t *words);
// Reflection: parsed once per shader from its .res.json and the SPIR-V before it enters the cache.
enum { GFX_RES_UBO, GFX_RES_SSBO, GFX_RES_TEX, GFX_RES_IMG, GFX_RES_KINDS };
typedef struct {
    uint16_t set, binding;      // as the SPIR-V decorates them: set 0, bindings from 1 per shader (0: support buffer)
    int16_t slot;               // UBO: c[slot]; SSBO: storage buffer index; TEX: c2 combined slot (handle at 0x20 + 8 slot)
                                // or, separate, texture slot (0x168 + 8 slot); IMG: image slot (0x120 + 8 slot); -1: other
    uint16_t type;              // TEX/IMG: Ryujinx SamplerType: 1 1D, 2 buffer, 3 2D, 4 3D, 5 cube | 0x100 array,
                                // 0x200 multisample, 0x400 shadow
    uint8_t format;             // IMG: declared format as a GfxShaderAnswers.img_format code, 0 unknown
    uint8_t flags;              // SSBO: 1 written; UBO: 2 the bank's word view, bound as a storage buffer (cbNw);
                                // TEX/IMG: Ryujinx TextureUsageFlags (4 image store, 8 coherent)
    int8_t cbuf_slot;           // TEX/IMG: constant buffer of the handle (-1: bound texture buffer); SSBO: of its address
    uint16_t handle;            // TEX/IMG: raw handle (word offset; separate: texture | sampler << 14); SSBO: address offset
    uint16_t count;             // descriptors (array length)
    uint8_t separate;           // TEX: texture and sampler handles are separate
} GfxShaderRes;
typedef struct { uint8_t type, comps, packed; } GfxShaderIo;   // GFX_TYPE_*, components, 1 with a Component decoration
int gfx_shader_stage(const GfxShader *s);                   // GFX_STAGE_VS, GFX_STAGE_FS or GFX_STAGE_CS
int gfx_shader_resources(const GfxShader *s, int kind, const GfxShaderRes **res);   // count of GFX_RES_* kind; array in *res
int gfx_shader_spec(const GfxShader *s, const uint32_t **words);   // fixed-address constant words: bank << 16 | byte
// s with word k fixed to val[k] where fix[k] (specialization constants' defaults); lives for the process; NULL if none
const GfxShader *gfx_shader_variant(const GfxShader *s, const uint32_t *val, const uint8_t *fix);
const GfxShader *gfx_shader_variant_of(const GfxShader *s);   // the generic shader of a variant, NULL for a generic one
uint32_t gfx_shader_inputs(const GfxShader *s, GfxShaderIo io[32]);    // Input Locations (VS attributes, FS varyings) as a mask
uint32_t gfx_shader_outputs(const GfxShader *s, GfxShaderIo io[32]);   // Output Locations (VS varyings, FS colour targets)
int gfx_shader_mesh_kind(const GfxShader *s);             // GFX_MESH_*, from the "stage" of the prepared reflection
// Task/mesh as compute: the program's compute form, NULL if none; and whether s is one.
const GfxShader *gfx_shader_cs_variant(const GfxShader *s);
int gfx_shader_cs_of_mesh(const GfxShader *s);
const GfxShader *gfx_shader_mesh_cs_vs(void);              // its draw's vertex shader (17 vec4 attributes), NULL if missing
int gfx_shader_is_mesh_cs_vs(const GfxShader *s);
#define MESH_CS_VWORDS 68                                  // words a vertex: position and 16 generics (head.py MESH_VWORDS)
const uint8_t *gfx_shader_cs_table(const GfxShader *s);   // NVN2 compute c0 table counts [5] (cs_res), NULL if unknown

// ---- back end (gfx_null.c, vk_*.c). Called by the NVN front end (nvn_api_*.c, nvn_ext.c) on guest threads.
typedef struct {
    const char *name;
    int  (*pool_init)(pool_t *p);                          // 0 fails MemoryPoolInitialize
    void (*pool_fini)(pool_t *p);
    int  (*pool_map_virtual)(pool_t *v, int n, const void *requests);   // NVNmappingRequest[n], 0x28 bytes each
    void (*texture_register)(int id, const tex_t *t, const view_t *v);
    void (*sampler_register)(int id, const samp_par *s);
    void (*texture_release)(const tex_t *t);
    void (*program_release)(const prog_t *p);
    uint64_t (*submit)(const uint64_t *handles, int n);    // replays; returns this submission's fence value
    // A submission queued now whose handles come later through held_fill (nvn_qorder.c: work that waits for another
    // queue's): its fence value, and *slot for held_fill. NULL: the back end replays at once (nothing can be held).
    uint64_t (*submit_held)(void **slot);
    void (*held_fill)(void *slot, const uint64_t *handles, int n);   // copies the handles; slot is the back end's again
    int  (*fence_wait)(uint64_t value, uint64_t timeout_ns);   // 1 reached; caller has given its core up
    uint64_t (*fence_done)(void);                          // highest value whose side effects are written
    uint64_t (*fence_queued)(void);                        // newest value the GPU may be running (a part queued)
    // 1 once every part of value is recorded and handed to the GPU queue (MKW_FENCE_EARLY); NULL: no such point
    int  (*fence_wait_issued)(uint64_t value, uint64_t timeout_ns);
    // Queues the present and returns at once; the host present goes out at at_ns (kern_now_ns clock), at once when that
    // has passed or at_ns is 0.
    void (*present)(const tex_t *t, const int crop[4], uint64_t at_ns);
    void (*finish)(void);
    void (*read_texels)(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img);
    void (*write_texels)(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row, uint64_t img);
    void (*guest_written)(uint64_t addr, uint64_t size);   // host wrote pool storage (3.2)
    void (*guest_read)(uint64_t addr, uint64_t size);      // the CPU reads pool storage: GPU-written texels go there first
    // The CPU waited for fence: small render targets and pitch textures written by work up to it go to their storage
    // (vk_texture.c vk_texture_write_back_small).
    void (*cpu_synced)(uint64_t fence);
} GfxBackend;
extern const GfxBackend gfx_null_backend, gfx_vulkan_backend;
// The front end fills every held submission now (nvn_qorder.c), as they stand: the back end's replay reached one still
// empty and waited MKW_QUEUE_HOLD_MS, a present or a drain is queued behind them. Not under a back-end lock: it fills
// through held_fill. Returns the waits it let go.
int gfx_held_force(void);
const GfxBackend *gfx_backend(void);                       // MKW_GPU=null|vulkan (default vulkan)

// ---- NVN front end (nvn_procs.c)
void nvn_procs_register(void);                             // hostfn_register every proc; before hostfn_seal (ov_gfx.c)
uint64_t nvn_proc_address(const char *name);               // synthetic host address of a proc, 0 for an unknown name

// ---- statistics and frames (nvn_api_*.c, vk_*.c; boot.c prints them)
typedef struct { uint64_t frames, nvn_calls, nvn_distinct, unimplemented, validation_errors, shader_failures,
                 draws_skipped; } GfxStats;
GfxStats gfx_stats(void);
void rt_frame_presented(const tex_t *t, uint64_t n);       // boot.c: --frames N ends the run after the Nth present
                                                           // (saves --shot first through read_texels)
