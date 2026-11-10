"""SM86 -> GLSL: the declarations before main() (version, extensions, helper functions, inputs and outputs,
resource bindings and their reflection) and the task/mesh wrappers around the translated body."""

from glsl.common import HMMA_FN, LDSM_FN, LOP3_FN, MOVM_FN, NMINMAX_FN, PRMT_FN, SG32_HELPERS, SHFL_FN
from glsl.thread import ThreadOps


def interp(b, stage: str, attr: int) -> str:
    """A PS input's interpolation from the SPH input map, as eden-nx2 translate_program.cpp:78-87: constant -> flat,
    screen linear -> noperspective. Declared smooth, the flat inputs that carry integers as float bits (the race
    bushes' material index in in_attr1.w) were interpolated and broke (01/11/2026); a component mix never occurs (census
    of the 7 262 PS: 992 flat, 2 439 linear attributes)."""
    if stage != "ps" or not b.sph:
        return ""
    byte = (b.sph.words[6 + (attr >> 2)] >> ((attr & 3) * 8)) & 0xFF
    modes = {(byte >> (k * 2)) & 3 for k in range(4)} - {0}
    return {1: "flat ", 3: "noperspective "}.get(modes.pop(), "") if len(modes) == 1 else ""

def sampler_type(glsl_type: str) -> int:
    """Ryujinx SamplerType (gfx_shader.c GfxShaderRes.type): 1 1D, 3 2D, 4 3D, 5 cube | 0x100 array, 0x200
    multisample, 0x400 shadow."""
    base = glsl_type.removeprefix("sampler").removeprefix("u").removeprefix("image")
    ms = "MS" in base
    code = {"1D": 1, "2D": 3, "3D": 4, "Cube": 5}[base.removesuffix("Shadow").removesuffix("Array").replace("MS", "")]
    return code | (0x100 if "Array" in base else 0) | (0x200 if ms else 0) | (0x400 if base.endswith("Shadow") else 0)


def declarations(b, stage: str, shape: tuple[int, int, int, int] | None) -> list[str]:
    """Lines before main() for Builder b once the body is translated; sets b.reflection."""
    head = [
        "#version 460",
        "#extension GL_ARB_gpu_shader_int64 : require",
        # jumps or loop passes before giving up: a wrong loop must not hang the GPU. Graphics stages run per pixel or
        # vertex, so their cap is lower: 65536 passes in every pixel of a 1280x720 target take the GPU past amdgpu's
        # timeout (run93 reset the GPU, cause not pinned down; 4096 is a safety bound, not a measured need)
        f"#define LOOP_CAP {'0x10000u' if stage == 'cs' else '0x1000u'}",
    ]
    if b.uses_subgroup:
        head += [
            "#extension GL_KHR_shader_subgroup_basic : require",
            "#extension GL_KHR_shader_subgroup_ballot : require",
            "#extension GL_KHR_shader_subgroup_vote : require",
            "#extension GL_KHR_shader_subgroup_shuffle : require",
        ]
        if b.uses_arith:
            head.append("#extension GL_KHR_shader_subgroup_arithmetic : require")  # fmt: skip
        head.append(SG32_HELPERS)
    if stage == "cs":
        # Workgroup and shared memory size from the program's control section (compute_shape). They were
        # specialization constants that no back end set: every dispatch ran 1x1x1 groups until 31/10/2026.
        x, y, z, _ = shape or (1, 1, 1, 0)
        head += [f"layout(local_size_x = {x}, local_size_y = {y}, local_size_z = {z}) in;"]
    if b.uses_shared:
        words = (shape[3] + 3) // 4 if shape and shape[3] else 16384
        head += [f"const uint SMEM_WORDS = {words}u;", "shared uint smem[SMEM_WORDS];"]
        if b.uses_ldsm:
            head.append(LDSM_FN)
    if b.uses_hmma:
        head.append(HMMA_FN)
    if b.uses_movm:
        head.append(MOVM_FN)
    if b.lmem_bytes:
        sph_bytes = b.sph.words[1] & 0xFFFFFF if b.sph else 0
        head += [f"const uint LMEM_WORDS = {(max(b.lmem_bytes, sph_bytes) + 3) // 4}u;", "uint lmem[LMEM_WORDS];"]
    if b.uses_lop3:
        head.append(LOP3_FN)
    if b.uses_shfl:
        head.append(SHFL_FN)
    if b.uses_prmt:
        head.append(PRMT_FN)
    if getattr(b, "uses_nminmax", False):
        head.insert(2, "#extension GL_EXT_spirv_intrinsics : require")
        head.append(NMINMAX_FN)
    if b.images:
        head.insert(2, "#extension GL_EXT_shader_image_load_formatted : require")
    if b.cbufs_w:   # cbNw (mem.cb_word); scalarBlockLayout, enabled by vk_device.c
        head.insert(2, "#extension GL_EXT_scalar_block_layout : require")
    if b.uses_shadow_lod:
        head.insert(2, "#extension GL_EXT_texture_shadow_lod : require")
    head += [f"layout(location = {a}) {interp(b, stage, a)}in vec4 in_attr{a};" for a in sorted(b.in_attrs)]
    if stage == "ps":
        head += [f"layout(location = {t}) out vec4 out_color{t};" for t in sorted(b.out_attrs)]
    else:
        head += [f"layout(location = {t}) out vec4 out_attr{t};" for t in sorted(b.out_attrs)]
        if getattr(b, "clip", 0):
            head.append(f"out float gl_ClipDistance[{b.clip}];")
    # Bindings from 1 (0 is the back end's support buffer), set 0, in this order: constant banks, storage buffers,
    # textures, images; the reflection names each the way runtime/gfx/nvn/host/gfx_shader.c add_res() reads it.
    refl: dict = {"stage": "task" if b.task else "mesh" if b.mesh else stage, "cbufs": [], "sbufs": [], "textures": [], "images": []}
    nb = 1
    for n in sorted(b.cbufs):
        head.append(f"layout(std140, binding = {nb}) uniform CB{n} {{ uvec4 d[4096]; }} cb{n};")
        if n in b.cbufs_w:   # the same buffer as words for dynamic addresses (mem.cb_word)
            head.append(f"layout(scalar, binding = {nb}) uniform CB{n}W {{ uint w[16384]; }} cb{n}w;")
        refl["cbufs"].append({"binding": nb, "slot": n})
        nb += 1
    for o in sorted(b.ssbos):
        head.append(f"layout(std430, binding = {nb}) buffer SB{o:x} {{ uint d[]; }} sb{o:x};")
        # flags 1: the shader stores into it. The replay copies such a range back to guest memory and keeps the CPU's
        # writes out of it until then; every buffer marked so hid the HUD's glyph records (01/11/2026).
        refl["sbufs"].append({"binding": nb, "slot": len(refl["sbufs"]), "cb_slot": 0, "cb_offset": o // 4,
                              "flags": 1 if o in b.ssbos_written else 0})  # fmt: skip
        nb += 1
    for (bank, idx), t in sorted(b.textures.items()):
        head.append(f"layout(binding = {nb}) uniform {t} t{bank}_{idx};")
        refl["textures"].append({"binding": nb, "type": sampler_type(t), "format": "Unknown", "cbuf_slot": bank,
                                 "handle": idx, "array": 1, "separate": False, "flags": 0})  # fmt: skip
        nb += 1
    for (bank, idx), t in sorted(b.images.items()):
        fmt = "r32ui, " if t.startswith("uimage") else ""
        head.append(f"layout({fmt}binding = {nb}) uniform {t} i{bank}_{idx};")
        refl["images"].append({"binding": nb, "type": sampler_type(t), "format": "R32Uint" if fmt else "Unknown", "cbuf_slot": bank,
                               "handle": idx, "array": 1, "separate": False, "flags": 4})  # fmt: skip
        nb += 1
    for bank, idx in sorted(b.txq_tex):
        n = {"sampler1D": 1, "sampler2D": 2, "sampler3D": 3, "samplerCube": 2, "sampler2DArray": 3,
             "samplerCubeArray": 3, "sampler1DArray": 2}.get(b.textures[(bank, idx)].replace("Shadow", ""), 2)  # fmt: skip
        sz = f"textureSize(t{bank}_{idx}, l)"
        v = sz if n == 3 else f"ivec3({sz}, 1)" if n == 2 else f"ivec3({sz}, 1, 1)"
        head.append(f"ivec3 txqsz_{bank}_{idx}(int l) {{ return {v}; }}")
    # Specialization constants of the fixed-address constant words (Builder.const); "spec" lists each k's [bank, byte].
    for (bank, byte), k in b.spec.items():
        head.append(f"layout(constant_id = {2 * k}) const bool SCU{k} = false;")
        head.append(f"layout(constant_id = {2 * k + 1}) const uint SCV{k} = 0u;")
    refl["spec"] = [[bank, byte] for (bank, byte), _ in sorted(b.spec.items(), key=lambda e: e[1])]
    b.reflection = refl
    return head


# Task programs: 32 invocations a workgroup (a warp: the programs use lane masks and ballots), the payload as a uint
# array, the mesh count in shared memory; EmitMeshTasksEXT runs once, after every invocation has finished the program
# (on the console the last lane writes the count and exits).
TASK_MAIN = """void main() {
    if (gl_LocalInvocationIndex == 0u) task_count = 0u;
    barrier();
    sm_main();
    barrier();
    EmitMeshTasksEXT(task_count, 1u, 1u);
}
"""


# Mesh programs: 32 invocations, outputs kept in shared memory while the program runs (MV_WORDS words in the SKEW
# layout, index bytes, the primitive count), then emitted: 32 vertices with the position and generics 0-15, triangles.
# The outputs start at zero: shared memory keeps the previous workgroup's bytes on AMD, and an index or vertex the
# program leaves unwritten drew long triangles across the shadow map (Hookshot ae4fb30, RX 7700S; GPL: the idea only).
MESH_GENERICS = 16
MESH_VERTS = 32
MESH_ZERO = """    for (uint k = gl_LocalInvocationIndex; k < MV_WORDS; k += 32u) mv[k] = 0u;
    for (uint k = gl_LocalInvocationIndex; k < MESH_PRIMS * 3u; k += 32u) mib[k] = 0u;
"""
MESH_MAIN = (
    """void main() {
    uint virtual_warp_id = gl_LocalInvocationIndex >> 5u, virtual_lane_id = gl_LocalInvocationIndex & 31u;
    if (virtual_warp_id == 0u && virtual_lane_id == 0u) mprims = 0u;
"""
    + MESH_ZERO
    + """    barrier();
    if (virtual_warp_id == 0u) sm_main();
    barrier();
    uint np = min(mprims, MESH_PRIMS), v = virtual_lane_id;
    SetMeshOutputsEXT(32u, np);
    gl_MeshVerticesEXT[v].gl_Position = uintBitsToFloat(uvec4(mv[v], mv[32u + v], mv[64u + v], mv[96u + v]));
"""
    + "".join(f"    out_attr{g}[v] = uintBitsToFloat(uvec4(mv[{(4 + 4 * g) * 32}u + v], mv[{(5 + 4 * g) * 32}u + v], "
              f"mv[{(6 + 4 * g) * 32}u + v], mv[{(7 + 4 * g) * 32}u + v]));\n" for g in range(MESH_GENERICS))
    + """    for (uint p = v; p < np; p += 32u) gl_PrimitiveTriangleIndicesEXT[p] = min(uvec3(mib[3u * p], mib[3u * p + 1u], mib[3u * p + 2u]), uvec3(31u));
}
"""
)


def mesh_head(head: list[str]) -> list[str]:
    out = [head[0], "#extension GL_EXT_mesh_shader : require", *head[1:]]
    out += ["layout(local_size_x = 32) in;", "layout(triangles, max_vertices = 32, max_primitives = 64) out;",
            "#define WG_INDEX gl_WorkGroupID.x",
            "const uint MESH_VERTS = 32u;", "const uint MESH_PRIMS = 64u;", f"const uint MV_WORDS = {(4 + 4 * MESH_GENERICS) * 32}u;",
            f"taskPayloadSharedEXT uint tp[{ThreadOps.TASK_WORDS}];", "shared uint mv[MV_WORDS];",
            "shared uint mib[MESH_PRIMS * 3u];", "shared uint mprims;"]  # fmt: skip
    out = [x for x in out if not x.startswith("layout(location") and "gl_ClipDistance" not in x]
    return out + [f"layout(location = {g}) out vec4 out_attr{g}[];" for g in range(MESH_GENERICS)]


def task_head(head: list[str]) -> list[str]:
    out = [head[0], "#extension GL_EXT_mesh_shader : require", *head[1:]]
    out += ["layout(local_size_x = 32) in;", f"taskPayloadSharedEXT uint tp[{ThreadOps.TASK_WORDS}];",
            "shared uint task_count;", "#define WG_INDEX gl_WorkGroupID.x"]  # fmt: skip
    return [x for x in out if not x.startswith("layout(location") and "gl_ClipDistance" not in x]


# Task/mesh as compute, for devices without VK_EXT_mesh_shader (the idea from AnyPS5
# #6528, no code): the same translated body, other wrappers. Buffers by address (push constants): trec the task records
# (TASK_REC words a workgroup: the payload, then task_count, at most MESH_PER_TASK: a ballot count in MKW's 7 task
# programs), mmap the mesh workgroups the tasks asked for, packed as task << 5 | index (runtime/shaders/mesh_compact.comp
# between the two passes, which also writes the indirect dispatch and draw), mvo the vertices (MESH_VWORDS words each:
# position and the MESH_GENERICS generics) and mto the triangles as absolute vertex indices, those past the mesh's
# count written degenerate. The draw is indexed, through MESH_CS_VS.
MESH_PER_TASK = 32
MESH_VWORDS = 4 + 4 * MESH_GENERICS
TASK_REC = 1024 + 1
# The three buffers by GPU address in push constants (bufferDeviceAddress, core in Vulkan 1.2): the programs' own set 0
# stays as it is, and the runtime adds only a push-constant range to these layouts.
CS_BUFFERS = [
    "#extension GL_EXT_buffer_reference : require",
    "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require",
    "layout(buffer_reference, std430, buffer_reference_align = 4) buffer U32s { uint d[]; };",
    "layout(push_constant) uniform MeshPass { U32s trec, mvo, mto, map; uint has_task, max_groups; } mp;",
    "#define trec mp.trec",
    "#define mvo mp.mvo",
    "#define mto mp.mto",
    "#define mmap mp.map",
]

TASK_MAIN_CS = f"""void main() {{
    uint virtual_warp_id = gl_LocalInvocationIndex >> 5u, virtual_lane_id = gl_LocalInvocationIndex & 31u;
    if (virtual_warp_id == 0u && virtual_lane_id == 0u) task_count = 0u;
    barrier();
    if (virtual_warp_id == 0u) sm_main();
    barrier();
    if (mp.max_groups != 0u && gl_WorkGroupID.x >= mp.max_groups) return;
    uint base = gl_WorkGroupID.x * {TASK_REC}u;
    for (uint k = virtual_lane_id; k < {TASK_REC - 1}u; k += 32u) trec.d[base + k] = tp[k];
    if (virtual_warp_id == 0u && virtual_lane_id == 0u) trec.d[base + {TASK_REC - 1}u] = min(task_count, {MESH_PER_TASK}u);
}}
"""

MESH_MAIN_CS = (
    f"""void main() {{
    // with a task: the packed workgroup's task and index (runtime/shaders/mesh_compact.comp), launched only as many
    // as the tasks asked for
    uint virtual_warp_id = gl_LocalInvocationIndex >> 5u, virtual_lane_id = gl_LocalInvocationIndex & 31u;
    uint g = gl_WorkGroupID.x, v = virtual_lane_id;
    bool in_bounds = (mp.max_groups == 0u || g < mp.max_groups);
    uint t = (HAS_TASK && in_bounds) ? mmap.d[g] >> 5 : 0u;
    bool live = in_bounds && (virtual_warp_id == 0u);
    if (virtual_warp_id == 0u && v == 0u) mprims = 0u;
{MESH_ZERO}    if (HAS_TASK && in_bounds) for (uint k = v; k < {TASK_REC - 1}u; k += 32u) tp[k] = trec.d[t * {TASK_REC}u + k];
    barrier();
    if (live) sm_main();
    barrier();
    if (!in_bounds) return;
    uint np = live ? min(mprims, MESH_PRIMS) : 0u, vb = (g * MESH_VERTS + min(v, MESH_VERTS - 1u)) * {MESH_VWORDS}u;
    for (uint k = 0u; k < {MESH_VWORDS}u; k++) mvo.d[vb + k] = mv[k * 32u + v];
    for (uint p = v; p < MESH_PRIMS; p += 32u)
        for (uint k = 0u; k < 3u; k++)
            mto.d[(g * MESH_PRIMS + p) * 3u + k] = g * MESH_VERTS + (p < np ? min(mib[3u * p + k], MESH_VERTS - 1u) : 0u);
}}
"""
)

# The vertex shader of every mesh-as-compute draw: the vertex the mesh workgroup wrote, read as MESH_VWORDS / 4
# attributes of one vertex buffer (the vertex buffer is mvo, stride MESH_VWORDS * 4), so the graphics layout needs no
# push constants. A prepared shader like the others, under the fixed key MESH_CS_VS_KEY (runtime gfx_shader.c).
MESH_CS_VS = "\n".join(
    ["#version 460"]
    + [f"layout(location = {a}) in vec4 v{a};" for a in range(MESH_VWORDS // 4)]
    + [f"layout(location = {g}) out vec4 out_attr{g};" for g in range(MESH_GENERICS)]
    + ["void main() {", "    gl_Position = v0;"]
    + [f"    out_attr{g} = v{g + 1};" for g in range(MESH_GENERICS)]
    + ["}", ""]
)
MESH_CS_VS_KEY = "6d6b772d6d6573682d63732d76732d31" * 2   # "mkw-mesh-cs-vs-1" twice, as hex: not a program key


def mesh_head_cs(head: list[str]) -> list[str]:
    """mesh_head as a compute shader: tp is shared memory filled from the task's record."""
    out = [*head, "layout(local_size_x = 32) in;", "const uint MESH_VERTS = 32u;", "const uint MESH_PRIMS = 64u;",
           f"const uint MV_WORDS = {(4 + 4 * MESH_GENERICS) * 32}u;",
           # the runtime knows whether a task program comes first (the pipeline's pair), the translator does not
           "#define HAS_TASK (mp.has_task != 0u)",
           "#define WG_INDEX (HAS_TASK ? mmap.d[gl_WorkGroupID.x] & 31u : gl_WorkGroupID.x)",
           f"shared uint tp[{ThreadOps.TASK_WORDS}];", "shared uint mv[MV_WORDS];", "shared uint mib[MESH_PRIMS * 3u];",
           "shared uint mprims;", *CS_BUFFERS]  # fmt: skip
    return [x for x in out if not x.startswith("layout(location") and "gl_ClipDistance" not in x]


def task_head_cs(head: list[str]) -> list[str]:
    out = [*head, "layout(local_size_x = 32) in;", f"shared uint tp[{ThreadOps.TASK_WORDS}];", "shared uint task_count;",
           "#define WG_INDEX gl_WorkGroupID.x", *CS_BUFFERS]  # fmt: skip
    return [x for x in out if not x.startswith("layout(location") and "gl_ClipDistance" not in x]
