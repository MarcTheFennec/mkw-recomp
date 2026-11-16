# runtime/gfx

| Folder | Contents |
|---|---|
| [cap/](cap/README.md) | Frame capture (`MKW_CAPTURE`) and its replay without the game (`mkw --replay`) |
| [nvn/](nvn/README.md) | NVN front end: implements the `nvn*` functions on our own objects stored inside the game's objects; NVN2, textures (ASTC, deswizzle), window, shaders |
| [vk/](vk/README.md) | Vulkan back end: records what the front end logged and submits it; memory, pipelines, textures, synchronization, FSR |

For MKW: NVN2 (`tools/gpu/`: 249 functions used, 5 objects grow) and an SM86 shader translator instead of Ryujinx's.

MKW's own environment variables:

| Variable | Effect |
|---|---|
| `MKW_SPIRV` | Folder of the prepared SPIR-V (default `<exe>/spirv`) |
| `MKW_SPIRV_DUMP=<dir>` | Dumps programs without prepared SPIR-V and logs the stack of those the game generates at run time |
| `MKW_GPU_MEM_LIMIT=<MiB>` | Cap on the memory Vulkan allocates, split between heaps. On an integrated GPU that memory is RAM outside a memory-capped cgroup: without a cap, one run ended in the system OOM killer |
| `MKW_GPU_CRUMBS=0/1/ab` | GPU breadcrumbs for a device loss, and for a GPU that stops without one, after 5 s, two fifths of amdgpu's explicit `lockup_timeout` (2 s on the Steam Deck), or 1 s when its timeout is unset or unavailable (`vk/diag/vk_crumbs.c`, `VK_AMD_buffer_marker`, else NVIDIA checkpoints); on by default |
| `MKW_GPU_HANG_MS=<ms>` | When the breadcrumbs report a GPU that stopped without a device loss (`vk/device/vk_sync.c`; `0`: never) |
| `MKW_VK_FORMAT_LIST=0/1` | Render targets list their view formats so RADV keeps DCC compression (smo-recomp `dcc-format-list`); on by default on integrated GPUs |
| `MKW_VK_PLACED=1` | Experiment: every CPU-visible pool as driver memory mapped at its address (`VK_EXT_map_memory_placed`), without a shadow copy; one run reached frame 300 against 151 |
| `MKW_TEXPRE=1` / `<dir>` | Precomputed BC7 of the ASTC textures (`vk_texpre.c`; `1`: `<exe>/texpre`). Off by default: slower than the GPU encoder; the ones it lacks are recorded in `<dir>/src` for `build/mkw --texpre` (`MKW_TEXPRE_RECORD=0`: don't; `MKW_TEXPRE_JOBS`: threads) |
| `MKW_ASTC_BC7=0` | ASTC of 256x256 or more stays RGBA8 (by default it is recompressed to BC7 on the GPU) |
| `MKW_PHYS_LAZY=0` | Physical pools get memory at `MapVirtual`, not at the GPU's first use (4 MiB chunks) |
| `MKW_SPARSE_IMAGES=0` | Sparse textures without a Vulkan sparse image: a whole image, like the ones that cannot be sparse (depth on RADV) |
| `MKW_SPARSE_MIB=<MiB>` | Cap on the sparse tile memory (768 by default, 0 without a cap); what does not fit reads as zero |
| `MKW_SPEC=1` / `ab` | Specialized variants (`vk_spec.c`): after 120 frames, the constant bank words that did not change are fixed in a copy of the SPIR-V; a draw uses the variant when its values match and its pipeline is ready, the generic one otherwise. `MKW_SPEC_MAX` (512) caps how many |
| `MKW_CB_CENSUS=<file>` | Every 120 frames, per shader: fixed bank words that are stable, changing and unreadable, with the stable values (for `tools/shaders/spec_ceiling.py`) |
| `MKW_SPEC_STATS=1` | Enables `VK_KHR_pipeline_executable_properties` and logs the driver statistics of each optimized pipeline and each compute (`tools/shaders/spec_stats.py`); measurement only |
| `MKW_SPEC_ISA=KEY,...` | With `MKW_SPEC_STATS=1`: the driver's internal representations (on RADV, NIR, ACO and the RDNA assembly) of the pipelines with a shader whose key starts with one of those prefixes, in `build/isa/<vs>_<fs>_<stage>.txt`; measurement only |
| `MKW_SPARSE_TRACE=1` | Logs each sparse texture, each page the game maps and the copies to sparse (`tools/gpu/sparse_fit.py`) |
