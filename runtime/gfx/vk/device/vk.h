// Vulkan-internal header of the graphics layer. Host code only; the guest never sees it.
#pragma once
#include <vulkan/vulkan.h>
#include "vk_destroy_count.h"
#include <stdint.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#include "vk_mem_alloc.h"
#pragma clang diagnostic pop
#include "nvn_obj.h"   // pool_t, tex_t, view_t, samp_par, NVNcopyRegion (memory and textures block)
#include "gfx.h"       // GfxShader, GfxShaderRes, GfxShaderAnswers (pipelines block)

// Required (device choice fails without them): Vulkan 1.3; VK_KHR_swapchain, VK_KHR_push_descriptor,
// VK_EXT_depth_clip_control, VK_EXT_external_memory_host; features timelineSemaphore, dynamicRendering,
// synchronization2, shaderStorageImageWriteWithoutFormat, depthClipControl, shaderInt64, samplerAnisotropy,
// independentBlend, geometryShader, textureCompressionBC.
// Optional (flag set in VkCtx when enabled): the has_* members.
typedef struct VkCtx {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem_props;
    uint32_t gfx_family;                    // graphics + compute + presentation on win32
    uint32_t xfer_family;                   // a transfer-only family when the GPU has one, else gfx_family
    VkQueue gfx_queue;                      // graphics + present
    VkQueue xfer_queue;                     // equals gfx_queue when xfer_shared
    int xfer_shared;                        // 1: no separate queue; serialise with gfx_queue
    VmaAllocator vma;
    VkSemaphore timeline;                   // one timeline semaphore; values are issued by vk_sync.c
    VkDebugUtilsMessengerEXT messenger;
    volatile int32_t validation_errors;        // errors seen by the messenger (MKW_VK_VALIDATION=1)
    int validation;                         // layer active
    int has_custom_border_color, has_eds, has_eds2, has_eds3, has_vertex_input_dynamic_state,
        has_conditional_rendering, has_transform_feedback, has_provoking_vertex, has_device_fault, has_memory_budget,
        has_robustness2, has_buffer_device_address, has_fragment_shader_interlock,
        has_unused_attachments,             // VK_EXT_dynamic_rendering_unused_attachments
        has_buffer_marker,                  // VK_AMD_buffer_marker: the GPU breadcrumbs (vk_crumbs.c)
        has_nv_checkpoints,                 // VK_NV_device_diagnostic_checkpoints: the same on NVIDIA
        has_exec_stats,                     // VK_KHR_pipeline_executable_properties, with MKW_SPEC_STATS=1 only
        has_mesh,                           // VK_EXT_mesh_shader with taskShader and meshShader (MKW's 25 task/mesh)
        has_astc_ldr,                       // textureCompressionASTC_LDR (enabled when reported)
        has_calibrated,                     // VK_KHR_calibrated_timestamps, with MKW_TRACE_TL=1 only
        has_placed;                         // VK_EXT_map_memory_placed with unmap-reserve: pools as driver memory
                                            // mapped at their guest address (vk_memory.c placed_pool)
    // Feedback loops (maxwell.md 1.9, vk_exec_draw.c feedback_prepare): a draw samples a target of its open pass that
    // an earlier draw of the pass wrote. MKW_FEEDBACK=0: VK_FEEDBACK_OFF (sampled in GENERAL without a barrier, the
    // old behaviour); VK_FEEDBACK_LAYOUT with VK_EXT_attachment_feedback_loop_layout and dynamicRenderingLocalRead
    // (attachable images get feedback usage, a by-region barrier inside the pass); else (or MKW_FEEDBACK=restart)
    // VK_FEEDBACK_RESTART (the pass ends and restarts behind the full barrier).
    int feedback;                           // VK_FEEDBACK_*
    uint32_t min_imported_host_alignment;   // VK_EXT_external_memory_host
    int device_index;                       // index in vkEnumeratePhysicalDevices
} VkCtx;
enum { VK_FEEDBACK_OFF, VK_FEEDBACK_RESTART, VK_FEEDBACK_LAYOUT };

// vk_caps.c: fallbacks for GPUs other than the RX 9070, decided at device creation.
typedef struct {
    int float64;                       // shaderFloat64 enabled; 0: shaders are translated with softfloat64
    uint32_t sbuf_align;               // nonzero: storage buffers bind at their address rounded down to this
    uint32_t sg_size;                  // requiredSubgroupSize on sg_pin_stages (32)
    VkShaderStageFlags sg_pin_stages;  // 0: no pin, stages run at the device's subgroupSize
    int gpl;                           // VK_EXT_graphics_pipeline_library enabled with fast linking (MKW_GPL=0: off)
} VkCaps;
extern VkCaps vk_caps;
void vk_caps_features(VkPhysicalDeviceFeatures *f);   // before vkCreateDevice (MKW_VK_NO_FLOAT64 clears shaderFloat64)
// Before vkCreateDevice, with the driver's graphics pipeline library feature and properties: sets vk_caps.gpl,
// 1 = enable the extensions (VK_KHR_pipeline_library, VK_EXT_graphics_pipeline_library) and the feature.
int vk_caps_gpl(const VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *f,
                const VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT *p);
void vk_caps_init(VkCtx *c);                          // after it: pretend limits, fallbacks, the boot report

VkCtx *vk_ctx(void);                        // NULL until vk_init() succeeded
int vk_init(void);                          // idempotent; 1 on success, 0 on failure (reason logged with rt_log)
void vk_shutdown(void);
int vk_selftest(void);                      // 0 on success

// ---- window and present (gfx_present.c). Host threads only.
// vk_present_image: blits the crop rectangle {x, y, w, h} of src (NULL, or w/h <= 0: the whole w x h image), scaled
// to the swapchain extent, into the acquired swapchain image and presents it. src is in layout, which is
// VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL (its writes made visible to the transfer stage by the caller's barrier) or
// VK_IMAGE_LAYOUT_GENERAL (vk_texture.c images: the present makes earlier writes visible and orders later writes after
// the blit). All its writes must be submitted to gfx_queue before the call (queue order does the rest). Starts the
// window on first use. A minimised window or a lost swapchain skips the frame. Calls serialise on an internal lock.
void vk_present_image(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4]);
// vk_present_image split in two: vk_present_blit submits the blit and leaves the present waiting, vk_present_flip
// queues it. The next blit or a swapchain rebuild queues a waiting present first.
void vk_present_blit(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4]);
// FSR 1 on the present (vk_fsr.c): records EASU (+ RCAS) of src's crop rectangle {x, y, w, h} into the dw x dh
// rectangle at {dx, dy} of dst, a swapchain image in TRANSFER_DST; src is in GENERAL and fmt its format. 0 = nothing
// recorded (the setting says bilinear, no upscale is needed, or FSR is not available): the caller blits.
int vk_fsr_record(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, int sw, int sh, const int crop[4],
                  VkImage dst, int dx, int dy, uint32_t dw, uint32_t dh);
void vk_fsr_release(void);   // before the device goes away
void vk_present_flip(void);
// Every submission to ctx->gfx_queue (and xfer_queue when shared) takes this lock; vk_present_image and the swapchain
// rebuild do. Recursive.
void vk_queue_lock(void);
void vk_queue_unlock(void);
void vk_sync_parts_flush(void);   // queue lock held: the parts waiting for the submit thread to the queue (vk_sync.c)
// Copy of the last presented swapchain image, for probes: on, the next presents read the image back into a host buffer
// and wait for the GPU. *px stays valid until the next present; the format is the swapchain's. Returns 0 with no frame.
// The rectangle {x, y, w, h} of a dw x dh swapchain image a cw x ch picture is presented in (gfx_present.c,
// MKW_PICTURE_FIT); the rest is black.
void gfx_fit_rect(int cw, int ch, int dw, int dh, int out[4]);
void vk_present_capture(int on);
int vk_present_captured(const uint8_t **px, int *w, int *h, VkFormat *fmt);

// The window system's part of device creation (gfx_window_sdl.c): the instance
// extensions a surface needs (their count, at most max), and whether a queue family of pd can present to it.
int gfx_window_vk_extensions(const char **out, int max);
int gfx_window_vk_presentable(VkInstance inst, VkPhysicalDevice pd, uint32_t family);
void gfx_window_resize(int client_w, int client_h);   // programmatic resize; returns after the swapchain is rebuilt
void gfx_vsync_bind_counter(volatile int32_t *counter);  // host-side stand-in for a bound event: incremented per tick
unsigned gfx_window_focus_messages(void);          // OE_MSG_FOCUS_STATE_CHANGED pushes made by the window thread
void gfx_window_release_vk(void);                  // vk_shutdown calls it first: destroys swapchain, surface, present objects
long vk_validation_errors_total(void);             // validation errors since the first vk_init, across vk_shutdown

// ---- memory and textures (vk_memory.c, vk_texture.c)
// Guest or host threads; every call needs vk_init(). The first nine have the GfxBackend signatures of gfx.h; vk_backend.c builds
// gfx_vulkan_backend from them.
int  vk_pool_init(pool_t *p);              // CPU-visible: guest storage imported (VK_EXT_external_memory_host) as one
                                           // VkBuffer; physical: device-local memory; virtual: a 64 KiB page table
void vk_pool_fini(pool_t *p);              // drops images cut from the pool; waits for the GPU if it used the pool
int  vk_pool_map_virtual(pool_t *v, int n, const void *requests);   // nvn_mapping_t[n], checked by the front end
void vk_texture_register(int id, const tex_t *t, const view_t *v);  // texture descriptor slot (copied)
// MKW_REG_LATE (vk_tex_slots.c): the registration count, read at each submit, and the replay thread's limit while it
// records that submission (0 none); a lookup of a slot registered past it is logged
uint32_t vk_texture_reg_serial(void);
extern uint32_t vk_tex_reg_limit;
void vk_sampler_register(int id, const samp_par *s);                // sampler descriptor slot (copied)
int  vk_texture_trim(uint64_t bytes);      // out of memory: released and idle mirrored textures go (vk_staging)
// ASTC kept as RGBA8 copied into a BC7 image: decodes the source's blocks into dst (vk_texture.c); 0 when it cannot
int  vk_texture_astc_copy(const tex_t *src, uint32_t level, uint32_t layer, uint32_t layers, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h, VkImage dst, VkImageAspectFlags aspect, uint32_t dlevel, uint32_t dlayer,
                          int32_t dx, int32_t dy, int bc7);
void vk_texture_release(const tex_t *t);   // TextureFinalize: the image stays for slots that still name its storage
void vk_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img);
void vk_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row, uint64_t img);
void vk_guest_written(uint64_t addr, uint64_t size);   // a host write write-watch may miss: textures there re-upload
int vk_glyphs_due(void);      // per present: the button icon sheet's icons changed (ui/glyphs/glyphs.c); at most 10 times a second
void vk_glyphs_redo(void);    // ... then, replays drained: it uploads again, redrawn
int vk_pool_needs_drain(const pool_t *p);  // its GPU storage is guest memory: queued replays must be done first

// GPU address -> backing (section 3.1). avail: bytes from offset that stay contiguous in buffer. alloc/alloc_offset:
// the device-local memory behind physical and (through the page table) virtual pool addresses, NULL for imported pools.
// cls: the pool class of the backing (VK_MCLS_*), for the traffic census.
typedef struct {
    VkBuffer buffer;
    VkDeviceSize offset, avail;
    VmaAllocation alloc;
    VkDeviceSize alloc_offset;
    int cls;
    void *pool;   // the imported pool record behind the span (NULL for device memory), for VK_MEM_BOUND
} VkMemSpan;
// Traffic census (vk_memory.c vk_mem_census): bytes the replay binds or copies, by pool class and use. Imported pools by
// their CPU access flag; DEVICE is physical memory (directly or through a virtual pool); SCRATCH the replay's transient
// copies. CPU_READ counts bytes host code reads from pool storage in place of the GPU (misaligned binds, byte indices,
// counter words); GPU_WRITE what copies, fills, counter reports and uniform updates write.
enum { VK_MCLS_NONE, VK_MCLS_HOST_NOACC, VK_MCLS_HOST_UNC, VK_MCLS_HOST_CACHED, VK_MCLS_DEVICE, VK_MCLS_SCRATCH, VK_MCLS_N };
enum { VK_MUSE_VB, VK_MUSE_IB, VK_MUSE_UBO, VK_MUSE_SSBO, VK_MUSE_IND, VK_MUSE_COPY_SRC, VK_MUSE_GPU_WRITE,
       VK_MUSE_CPU_READ, VK_MUSE_N };
void vk_mem_count(int cls, int use, uint64_t bytes);
// Dirty census (MKW_DIRTY_CENSUS=1 with MKW_TRACE=perf, vk_memory.c): the replay notes each imported pool it binds as a
// vertex, index, uniform, storage or indirect buffer; at the start of every submit the pages the CPU wrote in such
// pools since the previous submit are counted from the shared page generations and logged as `mem: dirty` every 60
// frames. Its own switch: its write-watch query per submit was 18 % of the replay's context switches (rt-o1) and
// skewed every MKW_TRACE=perf measurement. Off: VK_MEM_BOUND is one flag test at the call site.
extern int vk_dirty_census;
void vk_mem_bound_note(void *pool);
#define VK_MEM_BOUND(sp) do { if (vk_dirty_census && (sp).pool) vk_mem_bound_note((sp).pool); } while (0)
void vk_mem_dirty_submit(void);                   // vk_sync_submit, under its lock, before the replay
// VRAM mirror of buffer-bound pools (vk_memory.c). vk_mirror_on: MKW_MEMORY=mirror|verify, or the
// device class is discrete and MKW_MEMORY is unset or auto; set at device selection. The replay (submitting thread
// under vk_sync_submit's lock) calls:
// - VK_MEM_USE at a vertex, index, uniform, storage or indirect binding of a span vk_mem_resolve returned: the pool's
//   mirror is created on first use and sp.buffer becomes it (same offsets); a pool that stays imported keeps its buffer.
// - VK_MEM_PICK where a copy, fill or update reads or writes a span: the mirror if the pool has one, never a new one.
// - vk_mem_gpu_wrote after recording a GPU write of [sp.offset, sp.offset + size) into sp.buffer: a mirror's bytes go
//   back to guest memory at the end of the submission. No-op for imported buffers.
// - vk_mem_mirror_begin at the start of a submission's recording, vk_mem_mirror_end (records the write-backs; returns
//   the commands recorded) after its last barrier.
// vk_sync.c records the copies the mirrors' first accesses queued (vk_mem_mirror_pending) into a prologue command
// buffer with vk_mem_mirror_prologue and submits it right before each part.
extern int vk_mirror_on;
void vk_mem_use(VkMemSpan *sp);
void vk_mem_pick(VkMemSpan *sp);
#define VK_MEM_USE(sp) do { if (vk_mirror_on && (sp).pool) vk_mem_use(&(sp)); } while (0)
#define VK_MEM_PICK(sp) do { if (vk_mirror_on && (sp).pool) vk_mem_pick(&(sp)); } while (0)
void vk_mem_gpu_wrote(const VkMemSpan *sp, uint64_t size);
// Uniform blocks as of the submit call (vk_usnap.c): vk_exec_record copies the uniform ranges a submission binds before
// it records, ubo bindings take the copy (0: not copied, bind memory), GPU writes of the replay drop the copies they touch.
void vk_usnap_take(const uint64_t *handles, int n);   // MKW_USNAP_AT_SUBMIT (vk_usnap_take.c), on the submitting thread
void vk_usnap_begin(const uint64_t *handles, int n, uint64_t fence);
void vk_usnap_end(uint64_t fence);
int vk_usnap_find(uint64_t addr, uint64_t n, uint64_t align, VkDescriptorBufferInfo *out);
void vk_usnap_drop(uint64_t addr, uint64_t n);   // the replay makes the GPU write [addr, addr+n) of guest memory
uint64_t vk_usnap_gen(void);                      // changes whenever a binding may resolve differently
const uint8_t *vk_usnap_peek(uint64_t addr, uint64_t n, uint64_t align);   // vk_usnap_find's host copy, uncounted
extern int ubo_ptr_snap;   // MKW_UBO_PTR_SNAP (vk_exec_ubuf.c): read by vk_usnap_begin, which keeps the host copy for it
struct VkExec;
void vk_ubo_watch(const struct VkExec *ex, int st);   // MKW_UBO_WATCH (diag/vk_ubo_watch.c)
void vk_cs_hash(struct VkExec *ex, uint64_t caller, uint64_t ind, int post);   // MKW_CS_HASH (diag/vk_cs_hash.c)
extern int vk_usnap_host_wanted;   // set by a reader of vk_usnap_peek: the host copy is kept from the next submission
// MKW_REPLAY_PAR: a chunk that records before an earlier chunk's GPU write still binds the copy there. With a log set
// on a thread, its snapshot hits are listed as region index (vk_usnap_regions of them) and replay position, so the join
// finds the hits a drop at an earlier position (vk_usnap_drop_at) has made wrong and records those chunks again.
typedef struct { int *hit, nhit, chit, *hpos, nhpos, chpos; } VkUsnapLog;
void vk_usnap_log(VkUsnapLog *l);   // NULL: stop
int vk_usnap_regions(void);
uint64_t vk_usnap_drop_at(int region);
uint64_t vk_exec_op_pos(void);   // vk_exec.c: the replay position of the op this thread records (1 for the first)
// vk_spec.c: specialization of fixed-address constant words
int vk_spec_census_on(void);
void vk_spec_observe(const GfxShader *s, const uint32_t *val, const uint8_t *known);
void vk_spec_frame(uint64_t frame);               // at a present
int vk_spec_mode(void);                           // MKW_SPEC: 0 off, 1 on, 2 ab
typedef void (*VkSpecRead)(void *ctx, const uint32_t *idx, int n, uint32_t *val, uint8_t *known);
const GfxShader *vk_spec_pick(const GfxShader *s, uint64_t frame, VkSpecRead read, void *ctx);
void vk_usnap_shutdown(void);                     // vk_exec_draw_shutdown: frees the arenas
void vk_usnap_summary(void);                      // the summary line now (a capture replay's end)
void vk_mem_mirror_begin(void);
int vk_mem_mirror_end(VkCommandBuffer cb, uint64_t fence);
int vk_mem_mirror_pending(void);
void vk_mem_mirror_prologue(VkCommandBuffer cb);
// Shadow storage (vk_memory.c): vk_sync.c calls vk_mem_shadow_sync right before each submit; the replay notes every GPU
// write into CPU-visible memory at record time (vk_mem_gpu_pending) and reports it at the fence with vk_gpu_written,
// which copies the bytes back into guest memory before telling the texture cache (vk_guest_written).
void vk_mem_shadow_sync(uint64_t fence);   // fence: the submission it is for (later parts skip pools synced for it)
extern uint64_t vk_replay_submit;          // the submit number the replay thread is recording (vk_mem_capture.c)
// What backs addr for the CPU's writes (diagnosis): 0 no pool, 1 imported (the GPU reads guest memory), 2 a shadow
// synced from guest memory, 3 a shadow on the capture list not covered yet, 4 a shadow fed by submit captures.
int vk_mem_shadow_state(uint64_t addr);
void vk_mem_shadow_capture(uint64_t sub);  // game thread, at submit sub: the CPU's writes as they are (vk_mem_capture.c)
int vk_mem_shadow_on(void);                // shadow mode (probes)
// 1 when a GPU write recorded and not copied back yet may touch [addr, addr+size), or nothing tells (no shadow)
int vk_mem_gpu_may_write(uint64_t addr, uint64_t size);
// MKW_SHADOW_LOST (vk_mem_lost.c): an indirect draw of `vs` reads its arguments at [addr, addr+size) of span.pool; one
// line per present of what the shadow lost of the CPU's writes.
void vk_mem_lost_watch(const void *pool, uint64_t addr, uint64_t size, uint32_t vs);
void vk_mem_lost_log(uint64_t frame);
void vk_mem_gpu_pending(uint64_t addr, uint64_t size);
void vk_gpu_written(uint64_t addr, uint64_t size);
int vk_mem_mirror_probe(uint64_t addr);   // --probe-vkexec: the pool at addr gets its mirror as on a first buffer use;
                                          // 1 when it has one
void vk_mem_class_log(const VkPhysicalDeviceProperties *p, const VkPhysicalDeviceMemoryProperties *m);   // once
void vk_mem_census(uint64_t frame);               // at each present: pool census and traffic, logged at frame 1800 and,
                                                  // with MKW_TRACE=perf, every 1800 frames
// VRAM in use and its budget, summed over the DEVICE_LOCAL heaps. exact 1: VK_EXT_memory_budget (the whole process),
// 0: this allocator's own allocations against the heap sizes. 0 when unknown.
int vk_mem_vram(uint64_t *used, uint64_t *budget, int *exact);
void vram_log_now(void);                                  // the "mem: vram" report now (vk_texture.c on image failures)
// VRAM headroom (vk_headroom.c): the VMA callbacks that see new device memory, and the thread that allocates the next
// block of each memory type in use ahead of the render thread.
const VmaDeviceMemoryCallbacks *vk_headroom_callbacks(void);
void vk_headroom_start(void);
void vk_tex_pool_start(void);   // MKW_VK_TEX_POOL_MIN: texture memory made at start (vk_tex_image.c)
void vk_headroom_stop(void);   // before vmaDestroyAllocator
int vk_mem_resolve(uint64_t addr, uint64_t size, VkMemSpan *out);   // 1 when all of [addr, addr+size) is backed
int vk_mem_virt_phys(uint64_t addr, uint64_t *phys);   // virtual pool page at addr: 1 and the GPU address it maps to

// Sparse textures as Vulkan sparse images (vk_sparse.c). create: 1 and *img for a sparse texture of a virtual pool
// whose format has standard sparse blocks (MKW_SPARSE_IMAGES=0 off); its tiles follow every MapVirtual (remap). order:
// queue lock held, before each graphics-queue submission: the changed bindings (binds), behind every earlier submission,
// and the wait and signal of that submission on the sparse order timeline.
void vk_texture_report(uint64_t addr);   // MKW_CTL "tex ADDR": textures over addr (vk_texture.c)
int vk_sparse_create(const VkImageCreateInfo *ci, const nvn_texdesc *d, uint64_t addr, uint64_t size, VkImage *img);
uint64_t vk_sparse_destroy(VkImage img);   // the submission its binds are done by (0: none)
int vk_sparse_is(VkImage img);   // 1: img is a sparse image made by vk_sparse_create
void vk_sparse_remap(uint64_t addr, uint64_t size);
void vk_texture_remap_virtual(uint64_t lo, uint64_t size);   // routes MapVirtual to non-sparse VMA-backed fallback images
// A copy or a pass wrote the region (w == 0: the whole level) of img: counted when img is sparse.
void vk_sparse_written(VkImage img, uint32_t level, uint32_t layer0, uint32_t layers, int32_t x, int32_t y, uint32_t w,
                       uint32_t h);
typedef struct { VkSemaphore wsem, ssem; uint64_t wait, signal; } VkSparseOrder;   // a NULL semaphore: no wait / signal
VkSparseOrder vk_sparse_order(uint64_t fence, int binds);
// Changes whenever vk_mem_resolve plus VK_MEM_USE may give another span for an address: a pool added, finalized or
// mapped, a mirror created, refused or forgotten, a submission started.
uint64_t vk_mem_gen(void);
// Page write generations over guest memory (mem_written_pages plus vk_guest_written). Returns 1 when a page of the
// range was written after generation `since`, 0 when not, -1 when the range is not write-watched (treat as written);
// *now is the generation to pass next time.
int vk_mem_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now);
// The same answer from per-epoch write-watch snapshots of 2 MiB regions: no system call once the regions were queried in
// this epoch (a guest submission). For checks that may see writes made during the epoch one epoch late.
int vk_mem_changed_snap(uint64_t addr, uint64_t size, uint32_t since, uint32_t epoch, uint32_t *now);
void vk_mem_snap_stats(uint64_t *checks, uint64_t *calls, uint64_t *untracked);   // since the last call

// Upload batch: one command buffer on gfx_queue that texture uploads and texel copies record into. begin/end bracket
// the recording (recursive lock); vk_staging (between them) returns a mapped host buffer that lives until the batch
// completes, or, with alloc non-NULL, until the caller's vmaDestroyBuffer after vk_upload_flush(1). The replay calls
// vk_upload_flush(0) before each submission that may read uploaded images; queue order and the batch's closing
// barrier make the uploads visible to it. Returns the batch sequence number; wait=1 blocks until it completed.
// MKW_FIX_ZERO (default 1): physical pools are zeroed once at creation and GPU-only images with memory of their own
// are cleared when created; 0 leaves both as the driver hands them out.
int vk_fix_zero(void);
VkCommandBuffer vk_upload_begin(void);
void vk_upload_end(void);
void *vk_staging(VkDeviceSize size, VkBuffer *buf, VmaAllocation *alloc);
// Out of memory for what (a log name): textures trimmed, the open batch sent when no vk_upload_begin is open, the GPU
// waited for and what it held freed; the caller tries once more
void vk_mem_relieve(uint64_t bytes, const char *what);
// vk_upload_begin plus vk_staging(size, buf, NULL) into *map, sending the open batch first when out of memory
VkCommandBuffer vk_upload_begin_staging(VkDeviceSize size, VkBuffer *buf, void **map);
uint64_t vk_upload_flush(int wait);
void vk_upload_flush_over(VkDeviceSize limit);   // sends the open batch once it holds that much staging or ASTC work
void vk_upload_work(uint64_t texels);            // ASTC texels the open batch decodes (vk_upload_flush_over)
// Deferred destruction: freed once the upload batches recorded so far and the timeline value last noted have completed.
void vk_defer_image(VkImage img, VmaAllocation alloc);   // alloc NULL: vkDestroyImage only
void vk_defer_view(VkImageView view);
void vk_exec_image_gone(VkImage image);   // destroy_tex, texture lock held: the replay's views of image go with it
void vk_defer_buffer(VkBuffer buf, VmaAllocation alloc);
void vk_defer_buffer_in(VmaAllocator vma, VkBuffer buf, VmaAllocation alloc);   // a buffer of another allocator
void vk_defer_call(void (*fn)(void *), void *arg);   // fn(arg) when the upload batches recorded so far are done
// ---- ASTC (vk_astc.c). MKW_ASTC = auto | gpu | native | cpu | check; see vk_astc.c.
enum { VK_ASTC_CPU, VK_ASTC_GPU, VK_ASTC_NATIVE, VK_ASTC_CHECK };
int vk_astc_mode(void);                 // decided once per device (logged); VK_ASTC_CPU before vk_init
int vk_astc_native(VkFormat astc_fmt);  // 1: images of this ASTC format are created as such and get the raw blocks
int vk_astc_gpu(void);                  // 1: ASTC images decoded to RGBA8 by the compute decoder (vk_astc_record)
int vk_astc_bc7(void);                  // 1: and encoded again as BC7 (MKW_ASTC_BC7, default on): 1 byte a texel, not 4
// One image region decoded from raw blocks: tight rows of 16-byte blocks at byte src of the source buffer (slices one
// after the other), w x h texels and slices deep, written at offset of level / layer.
typedef struct { VkDeviceSize src; uint32_t w, h, slices, level, layer; VkOffset3D offset; } VkAstcRegion;
// Records into cb, the open upload batch (between vk_upload_begin and vk_upload_end): src (TRANSFER_SRC, src_size
// bytes of blocks) goes to a device-local scratch buffer, a dispatch per region decodes it, and the texels are copied
// into img (VK_IMAGE_LAYOUT_GENERAL). The caller records its usual barriers around an image copy before and after the
// call. readback (optional, host-visible, TRANSFER_DST): also gets the decoded texels, the regions one after the other.
// 0 when the decoder is unavailable or the scratch buffer cannot be allocated (nothing recorded).
int vk_astc_record(VkCommandBuffer cb, VkBuffer src, VkDeviceSize src_size, const VkAstcRegion *rg, int n, uint32_t bw,
                   uint32_t bh, int srgb, VkImage img, VkImageAspectFlags aspect, int bc7, VkBuffer readback);   // bc7: img is BC7
// MKW_ASTC_TIME=1: GPU timestamps around each vk_astc_record's dispatches (logged per footprint and the largest one)
void vk_astc_time_enable(int on);
int vk_astc_time_get(uint32_t bw, uint32_t bh, uint64_t *texels, uint64_t *calls, double *us);   // totals so far
void vk_astc_time_summary(void);        // logs the totals
void vk_astc_release(void);             // vk_device.c, before the device goes: frees the decoder's objects
// ---- precomputed BC7 of ASTC textures (vk_texpre.c; MKW_TEXPRE, MKW_TEXPRE_RECORD)
typedef struct { uint32_t w, h, slices, level, layer; } VkTexpreRegion;   // one region of an upload, in upload order
int vk_texpre_on(void);
uint64_t vk_texpre_hash(const void *p, uint64_t n);
uint64_t vk_texpre_region_bytes(const VkTexpreRegion *r);   // BC7 bytes of a region
int vk_texpre_has(uint64_t h);          // 1: the directory held BC7 for h when it was listed
int vk_texpre_load(uint64_t h, void *dst, uint64_t size);   // 1: dst holds the size bytes of BC7 stored for h
void vk_texpre_record(uint64_t h, const nvn_texdesc *d, const VkTexpreRegion *rg, int n, const void *mem, uint64_t size);
void vk_mem_note_submit(uint64_t part_value);      // vk_sync.c: after each submit, the part value it signals
void vk_mem_recording(uint64_t fence);             // this thread records submission fence (0: done): what it destroys
                                                   // waits for the whole submission
void vk_mem_defer_idle(int on);                    // around destroying what the submission being recorded does not use
void vk_mem_defer_use(uint64_t use);               // around a deferred destruction: MKW_GPU_GUARD's last part using it
uint64_t vk_mem_defer_fence(uint64_t fence);       // returns the previous value; around a deferred destruction: wait for that submission (0: off)
void vk_mem_collect(void);                         // frees what has completed
typedef struct { uint64_t imported_pools, physical_pools, virtual_pools, import_failures, upload_batches; } VkMemStats;
void vk_mem_stats(VkMemStats *s);
void vk_mem_shutdown(void);                        // before vk_shutdown: waits idle, frees every memory and texture object

// Textures. Images are keyed by storage GPU address plus descriptor and stay in VK_IMAGE_LAYOUT_GENERAL. A mirrored
// texture (CPU-written pool storage, MNG tex_mirrored) is uploaded on first use and again when its storage pages were
// written; once the GPU wrote it (vk_texture_gpu_written) the image is its content. A returned pointer stays valid
// until the next call that may drop textures (release, pool fini); copy what outlives that.
typedef struct {
    VkImage image;
    VkFormat format;               // after the fallbacks of tex_vk_format (ASTC -> RGBA8, RGB -> RGBA)
    VkImageType type;
    VkImageViewType view_type;     // view of the whole image
    VkImageAspectFlags aspect;
    VkImageUsageFlags usage;
    uint32_t width, height, depth, levels, layers, samples;
    uint64_t addr;                 // storage GPU address
    uint8_t alpha_one;             // X format: sample alpha as 1 through the view swizzle
    uint8_t chan[4];               // TexVkFormat.chan: component order of a stand-in format, applied by the view swizzle
    uint8_t mirrored, gpu_owned;
    uint8_t bound;                 // aliases physical-pool memory (physical and virtual pools)
    uint8_t block_views;           // BC image that renders through uncompressed views of its blocks (BLOCK_TEXEL_VIEW_
                                   // COMPATIBLE + EXTENDED_USAGE: only those views may use COLOR_ATTACHMENT)
    uint8_t swz[4];               // NVN texture swizzle selectors (nvnTextureBuilderSetSwizzle); a view swizzle replaces them
    uint8_t fb_usage;              // created with ATTACHMENT_FEEDBACK_LOOP usage (VK_FEEDBACK_LAYOUT, attachable images)
    uint8_t listed;                // a render target whose MUTABLE_FORMAT lists its views' formats: format and twin
    VkFormat twin;                 // its sRGB / UNORM twin, the only other format a view of it may have (listed)
    uint8_t fb_learned;            // a draw sampled it while it was a written attachment of the open pass: its passes
                                   // attach it in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT (vk_exec.c vk_exec_pass_begin)
} VkTexImage;
void vk_exec_recent_log(void);   // the last programs drawn or dispatched with (vk_exec_dstat.c), at a device loss
// GPU breadcrumbs (diag/vk_crumbs.c, VK_AMD_buffer_marker): a start and an end mark around each upload batch, render
// pass and dispatch, and at a device loss the last started and finished of each with what it was.
enum { VK_CRUMB_PASS, VK_CRUMB_DISPATCH, VK_CRUMB_UPLOAD };
void vk_crumbs_init(void);       // after the allocator exists
void vk_crumbs_release(void);    // before the allocator goes
// The start mark into cb (through the recorder thread when it owns cb); returns its id for vk_crumb_end, 0 when off.
// Pass: w, h, d the render area and layers with the attachment formats; dispatch: groups (0 for indirect); upload:
// w the batch number.
uint32_t vk_crumb_begin(VkCommandBuffer cb, int kind, uint32_t w, uint32_t h, uint32_t d, uint32_t ncolor,
                        const VkFormat *color, VkFormat depth);
void vk_crumb_end(VkCommandBuffer cb, int kind, uint32_t id);
void vk_crumbs_report(void);     // at a device loss (vk_exec_recent_log), once
void vk_crumbs_last_check(void); // startup: logs [host] last run crashed at: <pass/shader/command> from last_crumb.bin and clears it
void vk_crumbs_set_pass(uint32_t w, uint32_t h, uint32_t d, uint32_t ncolor, const VkFormat *color, VkFormat depth);
void vk_crumbs_set_state(uint64_t pipe_hash, const uint8_t *sa, const uint8_t *sb, const char *cmd, int in_mesh_compute);
void vk_crumbs_set_mesh_compute(int in_mesh_compute);
void vk_crumbs_dump_active(void);
void vk_crumbs_clear_active(void);
VkFormat vk_texture_view_format(const VkTexImage *img, VkFormat want);   // a target view's format the image allows
const VkTexImage *vk_texture_get(const tex_t *t);   // created and made current; NULL for BUFFER or unmapped formats
// A texture slot; a slot whose texture was finalized follows the live texture at the same storage (hle_nvn.c).
const VkTexImage *vk_texture_by_id(int id, view_t *view, int *has_view);
VkFormat vk_srgb_twin(VkFormat f);                    // the sRGB / UNORM twin of an 8-bit colour format, else UNDEFINED
uint32_t vk_texture_slot_format(int id, uint64_t *addr);   // NVN format the game registered in slot id (0: none)
// The replay looks up many slots per draw: it takes the texture lock once around them (vk_texture_lock) and uses the
// _locked lookups; vk_texture_slots_gen changes whenever a slot may resolve to another image or view.
void vk_texture_lock(void);
void vk_texture_unlock(void);
const VkTexImage *vk_texture_by_id_locked(int id, view_t *view, int *has_view);
VkSampler vk_sampler_by_id_locked(int id);
// The slot's sampler with nearest filtering and no anisotropy: a depth texture read without a compare lands on one texel
// on the Switch although the game binds a linear sampler, and MacNativeGames samples it the same way. VK_NULL_HANDLE
// for an empty slot.
VkSampler vk_sampler_point_by_id_locked(int id);
uint32_t vk_texture_slots_gen(void);
int vk_texture_checked(int id, uint32_t ep);   // unlocked: slot id's check in submit ep still stands (vk_tex_slots.c)
// What vk_texture_by_id(id) returns can change only when this does: the slot is registered again, or a texture entry at
// its storage address is created, dropped, released or taken back into use. Unlocked; read it before the lookup it
// guards (a lookup may create an entry). 0 when the slot was never registered: nothing is kept under 0.
uint64_t vk_texture_slot_token(int id);
uint32_t vk_sampler_gen(void);                      // changes whenever a sampler slot is registered
void vk_texture_gpu_written(const tex_t *t);        // rendered or copied into by the GPU: no re-upload from storage
// runtime/gfx/cap: of the live image of t, if any (none is made): VK_TEXCAP_GPU its content is what the GPU wrote,
// VK_TEXCAP_RECENT a draw sampled it within the last `recent` submissions, VK_TEXCAP_SPARSE it is a Vulkan sparse image
// (a sparse NVN texture may be a whole image: depth formats in RADV).
enum { VK_TEXCAP_GPU = 1, VK_TEXCAP_RECENT = 2, VK_TEXCAP_SPARSE = 4 };
int vk_texture_capture_state(const tex_t *t, uint32_t recent);
// vk_mem_cap.c (runtime/gfx/cap): every backed chunk of the physical pools out to emit, with its GPU address; and one
// chunk written back at its address (0 when no physical pool chunk starts there). Both wait for the GPU.
uint64_t vk_mem_phys_dump(void (*emit)(uint64_t addr, const void *p, uint64_t n, void *ctx), void *ctx);
int vk_mem_phys_load(uint64_t addr, const void *p, uint64_t n);
// vk_sparse.c (runtime/gfx/cap): the sparse tile slots written so far, by key; a saved slot is given its texels when
// it is made again (a replay's MapVirtual and first use).
uint64_t vk_sparse_dump(void (*emit)(const void *key, uint32_t klen, const void *p, uint64_t n, void *ctx), void *ctx);
void vk_sparse_load(const void *key, uint32_t klen, const void *p, uint64_t n);
void vk_texture_storage_bound_locked(const VkTexImage *img);   // bound as a storage image in the recording submission
uint64_t vk_texture_last_write(const tex_t *t);    // timeline value of the newest submission recorded to write it; 0: none
size_t vk_texture_texel_span(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint64_t row, uint64_t img);   // bytes ReadTexels spans; 0 for a bad region
int vk_texture_small_dirty(void);                   // a small mirrored target holds GPU texels its storage lacks
void vk_texture_write_back_small(uint64_t fence);   // writes those rendered by work up to fence back to storage
void vk_texture_debug(const VkTexImage *img, uint32_t *uploads, int *last_change, int *pages_written);   // vk_dump
int vk_texture_slot_swz(int id, uint8_t swz[4]);    // vk_dump: swizzle of the descriptor in slot id
int vk_texture_read_image(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row,
                          uint64_t img);            // always from the image (GPU copy, waited for); 0 on failure
int vk_texture_cpu_stale(uint64_t addr, uint64_t size);    // a GPU-written mirrored texture there has stale storage
uint64_t vk_texture_dirty_fence(uint64_t addr, uint64_t size);   // newest recorded writer of such a texture; 0: none
void vk_texture_write_back(uint64_t addr, uint64_t size);   // writes those images back into their storage (waits)
void vk_texture_range_gone(uint64_t lo, uint64_t size, VmaAllocation alloc);   // vk_pool_fini: drop the images there
void vk_texture_stats(uint64_t *images, uint64_t *uploads, uint64_t *reuploads);
// Entries now, live and released (TextureFinalize, kept until sweep_released), with the bytes of their own memory
// (images not bound into pool memory).
typedef struct {
    uint64_t live, released, own_live, own_released, bytes_live, bytes_released;
    uint64_t astc_bytes, target_bytes, big_bytes, astc_n, target_n, astc_rgba_bytes;   // astc_rgba: decoded to RGBA8, not BC7   // live own memory: ASTC decoded, render targets
                                                                      // (attachment usage), single images over 64 MiB
} VkTexCensus;
void vk_texture_census(VkTexCensus *s);
void vk_texture_shutdown(void);                     // vk_mem_shutdown calls it
void vk_texture_new_submit(void);                   // a queue submit starts: draw bindings check storage again
uint32_t vk_texture_epoch(void);                    // the write-watch snapshot epoch vk_texture_new_submit started
VkSampler vk_sampler_by_id(int id);                 // VK_NULL_HANDLE for an empty slot
VkSampler vk_sampler_for(const samp_par *s);        // cached by content
int vk_sampler_params(int id, samp_par *out);       // parameters of a sampler slot; 0 for an empty slot

// 8-bit RGBA PNG with stored deflate blocks (boot.c); stops with rt_fatal when the file cannot be written.
void rt_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);

// ---- submission, synchronisation, completion (vk_sync.c, vk_backend.c)
// Fence values are ctx->timeline values: each submission signals the next one, in submit order. A value counts as done
// once the GPU reached it and the completion thread ran its side effects. Guest or host threads.
void vk_sync_start(void);                  // vk_init (fatal without a device) and the completion thread; idempotent
uint64_t vk_sync_submit(const uint64_t *handles, int n);   // records with vk_exec_record, submits; returns the value
uint64_t vk_sync_reserve(void);            // asynchronous replay: the value the next queued submission signals
int vk_sync_fence_wait(uint64_t value, uint64_t timeout_ns);   // 1 when done; 0 on timeout or a value never submitted
uint64_t vk_sync_fence_done(void);         // highest done value
uint64_t vk_sync_fence_queued(void);       // newest value with a part on the GPU queue
uint64_t vk_mem_wb_need_take(uint64_t fence);   // MKW_WB_ORDER: the earlier value whose write-back the part waits for
// vk_timeline.c (MKW_TRACE_TL=1, a measurement): each frame's submissions on one clock, a line every 600 frames.
int vk_tl_on(void);
void vk_tl_sub(uint64_t f);                              // the game submitted value f (under the replay queue lock)
void vk_tl_replay(uint64_t f, int end);                  // the replay began (end 0) or finished (1) recording f
void vk_tl_queue_submit(uint64_t f);                     // a vkQueueSubmit2 of value f
void vk_tl_reached(uint64_t from, uint64_t to, int done);   // (from, to] reached; done: released to the waiters
void vk_tl_gpu(uint64_t f, uint64_t s, uint64_t e);      // a timed part of f: begin and end timestamps (ticks)
void vk_tl_wait(uint64_t f, int64_t t0, int64_t t1);     // a blocking wait for f
void vk_tl_present(uint64_t f, uint64_t frame);          // the replay thread presents value f
enum { VK_TL_IDLE, VK_TL_SUBMIT, VK_TL_PRESENT, VK_TL_RELEASE, VK_TL_REG, VK_TL_KINDS };
void vk_tl_replay_time(int kind, int64_t ns);             // the replay thread's time waiting for items or on one kind
uint64_t vk_sync_issued(void);             // newest submitted value (the one being recorded is this + 1)
int vk_sync_issued_wait(uint64_t value, uint64_t timeout_ns);   // 1 once value is recorded and handed to the queue
// Parts: every queue submission of vk_sync_submit and vk_sync_split also signals the next part value, so what is
// destroyed while a submission is still being recorded can wait for the parts already sent (vk_mem_note_submit).
uint64_t vk_sync_part_done(void);          // newest part value the GPU reached
void vk_sync_part_wait(uint64_t value);    // blocks until the GPU reached part value (submitted already)
void vk_sync_finish(void);                 // every submitted value done
void vk_sync_shutdown(void);               // finish, stop the completion thread, free the command buffers
// The op-stream replay (vk_exec.c): records the command handles of one submission into cb, which the caller has
// begun and ends. fence is the value this submission will signal. Returns the number of commands recorded into the
// command buffer it holds at the end (0: that one stays empty and is not submitted). A weak vk_sync.c version accepts
// empty streams only.
int vk_exec_record(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence);
// Inside vk_exec_record, outside a render pass: ends the command buffer recorded so far, submits it (after the upload
// batch) without a signal, and returns a new begun one for the rest of the submission. The submission's timeline value
// is signalled by its last part, whose signal covers every earlier part in submission order.
VkCommandBuffer vk_sync_split(void);
// The same, with nextra command buffers recorded on other threads (MKW_REPLAY_PAR's chunks, begun and not ended)
// submitted right after the part, in order, before the next part; they are ended here.
#define VK_SPLIT_EXTRA 64
VkCommandBuffer vk_sync_split_with(const VkCommandBuffer *extra, int nextra);
// MKW_PASS_TIMING (vk_passtime.c): GPU timestamps around render passes, summed by pass size.
int vk_pt_on(void);
void vk_pt_mark(VkCommandBuffer cb, uint32_t key);   // outside a pass; key: w << 16 | h of the pass beginning, else 0
void vk_pt_submit_begin(VkCommandBuffer cb);
void vk_pt_submit_end(VkCommandBuffer cb, uint64_t fence);
void vk_pt_frame(uint64_t frame);                    // at a present: the log line every 600
void vk_pt_flush(uint64_t frame, uint64_t frames);   // the line now, over that many frames (end of a replay)
int vk_pt_enabled(void);                             // vk_pt_on's answer without starting it
int vk_pt_draw_mode(void);                           // MKW_PASS_TIMING=draw: marks per draw, summed by program pair
void vk_pt_draw(VkCommandBuffer cb, const GfxShader *a, const GfxShader *b);   // before a draw or dispatch, any place
// Runs fn(arg) on the completion thread once fence is done on the GPU (event writes, counter reports, sync state).
// Callbacks run in the order they were added, after those of earlier values; a value already done runs at once there.
void vk_completion_add(uint64_t fence, void (*fn)(void *), void *arg);
// MKW_REPLAY_PAR: while a list is set on a thread, its vk_completion_add calls go to the list instead, so the chunks
// recorded on several threads hand their callbacks to the queue in op order (the caller adds each entry afterwards).
typedef struct { uint64_t fence; void (*fn)(void *); void *arg; } VkCompletionEnt;
typedef struct { VkCompletionEnt *e; int n, cap; } VkCompletionList;
void vk_completion_defer(VkCompletionList *l);   // NULL: add at once again
// gfx_vulkan_backend (vk_backend.c): presents go to a present thread and run after the submissions made before them.
void rdoc_init(void);                     // runtime/rdoc.c: MKW_RDOC debug capture; before vkCreateInstance
int rdoc_active(void);
void rdoc_after_present(uint64_t frame);
void vk_present_drain(void);               // returns once every queued present was submitted to gfx_queue
int vk_replay_async(void);                 // MKW_REPLAY_ASYNC (default 1): submissions replay on the replay thread
void vk_program_release(const prog_t *p);  // GfxBackend.program_release: vk_pipeline.c; weak no-op in vk_backend.c
void vk_backend_shutdown(void);            // presents, vk_sync_shutdown, vk_mem_shutdown, vk_shutdown in that order

// ---- pipelines (vk_pipeline.c)
// Binding model (comment block at the top of vk_pipeline.c): one descriptor set, set 0, per pipeline. Binding 0 is
// Ryujinx's support buffer (a uniform buffer every stage shares); the vertex stage keeps its own bindings, the fragment
// stage's follow them (its SPIR-V is renumbered). Set 0 is a push-descriptor set unless the pipeline needs more than
// maxPushDescriptors (32 here) descriptors; VkPipeInfo.bindings says what goes where. No push constants.
// Everything a graphics pipeline depends on, as the op stream carries it. Fields covered by dynamic state are read by
// vk_pipeline_set_dynamic only and left out of the pipeline key.
typedef struct VkPipeState {
    const GfxShader *vs, *fs;              // gfx_shader_get results; fs NULL: no fragment stage (depth only)
    int32_t color_fmt[8];                  // NVNformat of colour target i (OP_SET_RT), 0 none
    int32_t depth_fmt;                     // NVNformat of the depth/stencil target, 0 none
    uint8_t samples;                       // sample count of the targets; 0 or 1 single-sampled
    uint8_t prim;                          // NVN_DRAW_PRIMITIVE_* of the draw
    uint8_t depth_minus_w;                 // nvn_depth_minus_w(): clip-space depth is -w..w
    uint8_t depth_clamp, raster_discard;   // OP_DEPTH_CLAMP, OP_RASTER_DISCARD
    uint8_t y_flip;                        // the replay's viewports have negative heights (upper-left origin): the
                                           // front face is not mirrored
    uint8_t prim_restart;                  // the draw's indices restart its strip or fan at the all-ones value
                                           // (OP_PRIM_RESTART, set by op_draw_idx); dynamic state only
    uint8_t feedback;                      // the fragment shader samples an attachment of the pass in the feedback
                                           // layout: 1 colour, 2 depth/stencil (VK_PIPELINE_CREATE_*_FEEDBACK_LOOP_BIT_EXT)
    float alpha_ref;                       // OP_ALPHA_REF; 0 unless color.alpha_func tests (the draw clears it). It
                                           // fills padding: KeyRec (pipeline-keys.bin) keeps its size
    blend_st blend[8];                    // OP_BIND_STATE kind 0, indexed by blend_st.target
    color_st color;                        // kinds 1-5
    chmask_st chmask;
    ms_st ms;
    poly_st poly;
    ds_st ds;
    uint32_t vattr[16];                    // OP_BIND_VATTR: va_st words; attribute i feeds vertex shader Location i
    vs_st vstream[16];                     // OP_BIND_VSTREAM
    const GfxShader *mesh;                 // a task/mesh draw whose vs is the task program: its mesh program; NULL
                                           // otherwise (a mesh program alone sits in vs)
} VkPipeState;
_Static_assert(__builtin_offsetof(VkPipeState, blend) == 64, "alpha_ref must stay in the padding (KeyRec size)");
void vk_pipe_state_defaults(VkPipeState *s);   // NVN defaults of every state object (shaders and formats left 0)
enum { VK_PIPE_SUPPORT = GFX_RES_KINDS };      // VkPipeBinding.kind of the support buffer
#define VK_PIPE_STAGE_NVN 0x80                 // VkPipeBinding.stage of a task or mesh binding: this | its NVN stage (6, 7)
#define VK_PIPE_ZERO_BINDING 16                // vertex binding of shader inputs without an attribute: bind a zeroed
                                               // buffer of at least 16 bytes there (stride 0)
typedef struct {
    uint16_t binding;                      // in its set
    uint8_t stage;                         // GFX_STAGE_* that declared it (support buffer: the first stage)
    uint8_t kind;                          // GFX_RES_* or VK_PIPE_SUPPORT
    VkDescriptorType type;
    uint16_t count;
    VkShaderStageFlags stages;
    const GfxShaderRes *res;               // the stage's reflection entry; NULL for the support buffer
    uint8_t set;                           // descriptor set: 0, or 1 for the fragment stage with independent layouts
} VkPipeBinding;
typedef struct {
    VkPipeline pipeline;                   // VK_NULL_HANDLE when creation failed
    VkPipelineLayout layout;
    VkDescriptorSetLayout set_layout;      // set 0
    int push;                              // 1: write set 0 with vkCmdPushDescriptorSetKHR; 0: allocate and bind a set
    VkPipelineBindPoint bind_point;
    uint32_t n_bindings;
    const VkPipeBinding *bindings;
    uint32_t vs_inputs;                    // Locations the vertex shader reads
    // Independent layouts (vk_caps.gpl): set 0 holds the vertex stage's bindings, set 1 the fragment stage's; bindings
    // are ordered by set. One of the two sets at most is a push set. n_sets 1: everything in set 0 (MKW_GPL=0, compute).
    VkDescriptorSetLayout set_layout1;
    int push1;
    uint32_t n_sets;
    // Descriptor update template per set (vk_pipeline.c make_tpl): reads one VkDescSlot per descriptor, in binding order,
    // from the start of the set's descriptors. VK_NULL_HANDLE: the set is written with VkWriteDescriptorSet structs.
    VkDescriptorUpdateTemplate tpl[2];
} VkPipeInfo;
// One descriptor of the packed array a descriptor update template reads (VkPipeInfo.tpl), 24 bytes for every type; the
// bytes a type leaves unused are zero, so equal descriptors compare equal byte for byte.
typedef union { VkDescriptorImageInfo image; VkDescriptorBufferInfo buffer; VkBufferView view; uint64_t w[3]; } VkDescSlot;
// The cached pipeline (created on first use; failures are cached and logged once); info may be NULL. Valid until
// vk_pipeline_shutdown; thread-safe. After vk_pipeline_async_enable a missing pipeline is built by background workers
// and this returns VK_NULL_HANDLE with *pending 1 until it is ready (the draw is skipped).
// wait_ms > 0 waits up to that long for a pending pipeline first. Pipelines built in a run are recorded to
// pipeline-keys.bin next to the pipeline cache and prebuilt at the next boot (MKW_PIPE_PREBUILD=0: not).
VkPipeline vk_pipeline_graphics_async(const VkPipeState *s, const VkPipeInfo **info, int *pending, uint32_t wait_ms);
VkPipeline vk_pipeline_graphics_async_low(const VkPipeState *s, const VkPipeInfo **info, int *pending);   // no wait; a miss from the low queue
// rt_boot calls it unless --shot is given; MKW_PIPE_SYNC=1 makes it a no-op. Probes never call it.
void vk_pipeline_async_enable(void);
// Boot prewarm, before the guest runs (rt_boot): loads the shaders pipeline-keys.bin names, translating the ones not
// cached under the current translator from their stored inputs, and builds every recorded pipeline, on all cores.
// No-op without vk_pipeline_async_enable, with MKW_PIPE_PREBUILD=0 or with MKW_GPU=null.
void vk_pipeline_prewarm(void);
VkPipeline vk_pipeline_compute(const GfxShader *cs, const VkPipeInfo **info);
// Task/mesh as compute (vk_mesh_cs.c): whether DrawMeshTasks goes that way, and its device-local scratch for a
// submission (fence): buffer, offset and GPU address; 0 when no memory.
int vk_mesh_cs_on(void);
int vk_mesh_cs_alloc(uint64_t fence, VkDeviceSize size, VkBuffer *buf, VkDeviceSize *off, VkDeviceAddress *addr);
VkPipeline vk_mesh_cs_compact_pipeline(VkPipelineLayout *layout);   // runtime/shaders/mesh_compact.comp
// Records every dynamic state that VkPipeState determines, after binding a graphics pipeline: cull mode, front face,
// topology, depth and stencil tests and ops, depth bias enable, rasterizer discard, vertex input, and, where the GPU
// has them dynamic, depth clamp, polygon mode, blend enables, equations, write masks and alpha to coverage. The replay
// sets the rest: vkCmdSetViewportWithCount, vkCmdSetScissorWithCount, depth bias, blend constants, stencil compare and
// write masks and references; vertex buffers with vkCmdBindVertexBuffers2 (strides NULL when
// vk_pipeline_vertex_input_dynamic(), else from VkPipeState.vstream).
void vk_pipeline_set_dynamic(VkCommandBuffer cb, const VkPipeState *s);
int vk_pipeline_vertex_input_dynamic(void);    // 1: vertex input is VK_EXT_vertex_input_dynamic_state
// The vertex attribute types of the bound state (attr_type from the vertex attribute formats).
void vk_pipeline_answers(const VkPipeState *s, GfxShaderAnswers *vs);
uint32_t vk_pipeline_target_wmask(const VkPipeState *s, int i);   // colour write mask of target i the pipeline uses
VkPrimitiveTopology vk_pipeline_topology(int nvn_prim);   // quads -> triangle list, line loop -> strip: the replay
                                                          // rewrites those draws; polygon -> fan
VkFormat vk_vertex_format(int nvn_format, int *type);     // attribute format (supported fallback), GFX_TYPE_* in *type
PFN_vkCmdPushDescriptorSetKHR vk_pipeline_push_fn(void);  // after the first vk_pipeline_* call
// Descriptor update templates (after the first vk_pipeline_* call): NULL when templates are off (MKW_DESC_TEMPLATE=0,
// a device below Vulkan 1.1) or, for the push variant, vkCmdPushDescriptorSetWithTemplateKHR is missing.
PFN_vkCmdPushDescriptorSetWithTemplateKHR vk_pipeline_push_tpl_fn(void);
PFN_vkUpdateDescriptorSetWithTemplate vk_pipeline_update_tpl_fn(void);
int vk_pipeline_draw_cache(void);   // MKW_DRAW_CACHE (vk_pipeline.c)
typedef struct { uint64_t graphics, compute, failures, hits, layouts, push_layouts, dyn_over, async_built, pending_skips; } VkPipeStats;
void vk_pipeline_stats(VkPipeStats *s);
void vk_pipeline_save(void);               // writes the VkPipelineCache to pipeline-cache.bin next to mkw
void vk_pipeline_autosave(void);           // vk_pipeline_save at most every 20 s, when pipelines were built since
void vk_pipeline_shutdown(void);           // waits for background builds, saves, destroys pipelines, layouts and the
                                           // cache; before vk_shutdown

// vk_guard.c: MKW_GPU_GUARD checks before submitting. vk_guard: -1 until read.
extern int vk_guard;
int vk_guard_on_slow(void);
static inline int vk_guard_on(void) { int g = __atomic_load_n(&vk_guard, __ATOMIC_ACQUIRE); return g >= 0 ? g : vk_guard_on_slow(); }
void vk_guard_gpu_ok(void);            // stops when the repo's tools/run/gpu_ok.sh refuses the GPU (before vkCreateDevice)
uint64_t vk_guard_use(void);           // the submission part a use recorded now goes into; 0 outside a recording
void vk_guard_note(uint64_t *use);     // *use = max(*use, vk_guard_use())
void vk_guard_dead(const char *what, uint64_t handle, uint64_t lo, uint64_t use);   // stops if part use is not done
void vk_guard_bound_pipe(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p);   // the rc_* wrappers
void vk_guard_bound_sets(VkCommandBuffer cb, VkPipelineBindPoint bp, uint32_t first, uint32_t n, const VkDescriptorSet *s);
void vk_guard_pushed(VkCommandBuffer cb, VkPipelineBindPoint bp, uint32_t set);
void vk_guard_cb_begin(VkCommandBuffer cb);                        // at vkBeginCommandBuffer: nothing bound
void vk_guard_pipe_hit(VkCommandBuffer cb, int k, VkPipeline p);   // the replay skipped a bind: stops if cb differs
void vk_guard_set_hit(VkCommandBuffer cb, VkPipelineBindPoint bp, uint32_t s, VkDescriptorSet set);   // set 0: a push
void vk_guard_address(const char *what, uint64_t addr);   // stops on 0
enum { GUARD_TEST_BIND = 1, GUARD_TEST_FREE };
int vk_guard_test(void);                       // MKW_GPU_GUARD_TEST: a hazard committed on purpose (vk_guard.c)
void vk_guard_test_bind(VkCommandBuffer cb, int k);
void vk_guard_test_free(uint64_t use);
uint64_t vk_sync_part_next(void);      // the part value the next queue submission signals
uint64_t vk_sync_issued_pair(uint64_t *part);   // vk_sync_issued() and the part value of its last part, read together
void vk_guard_tie(const char *what, uint64_t handle, uint64_t lo, uint64_t use, uint64_t tie_part);   // at queueing:
                                       // stops when a part after the one the destruction waits for names the object

#include "vk_rec.h"   // last: its macros route the vkCmd* calls of every file that includes vk.h
