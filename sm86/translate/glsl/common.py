"""SM86 -> GLSL: helpers shared by glsl.Builder and its mixins (GLSL helper functions, operand names, tables)."""




class Unsupported(Exception):
    pass


# Wave32 subgroup emulation on Wave64 hardware (e.g., AMD GCN 5.1 / Vega APUs where minSubgroupSize == 64 and
# requiredSubgroupSizeStages == 0): each 64-wide subgroup is logically split into two independent 32-lane virtual
# warps via warp_half = gl_SubgroupInvocationID & 32u and lane_id = gl_SubgroupInvocationID & 31u. On native Wave32
# devices warp_half is always 0u.
SG32_HELPERS = """uvec4 sg32_ballot(bool pred) {
    uint warp_half = gl_SubgroupInvocationID & 32u;
    uvec4 ballot = subgroupBallot(pred);
    return (warp_half == 0u) ? uvec4(ballot.x, 0u, 0u, 0u) : uvec4(ballot.y, 0u, 0u, 0u);
}
uint sg32_ballot_bit_count(uvec4 b) { return subgroupBallotBitCount(uvec4(b.x, 0u, 0u, 0u)); }
uint sg32_ballot_excl_bit_count(uvec4 b) {
    uint lane_id = gl_SubgroupInvocationID & 31u;
    return subgroupBallotBitCount(uvec4(b.x & ((1u << lane_id) - 1u), 0u, 0u, 0u));
}
uint sg32_ballot_incl_bit_count(uvec4 b) {
    uint lane_id = gl_SubgroupInvocationID & 31u;
    uint m = (lane_id == 31u) ? 0xffffffffu : ((1u << (lane_id + 1u)) - 1u);
    return subgroupBallotBitCount(uvec4(b.x & m, 0u, 0u, 0u));
}
bool sg32_all(bool pred) { return sg32_ballot(pred).x == sg32_ballot(true).x; }
bool sg32_any(bool pred) { return sg32_ballot(pred).x != 0u; }
bool sg32_all_equal(bool pred) {
    uint m = sg32_ballot(pred).x, a = sg32_ballot(true).x;
    return m == 0u || m == a;
}
bool sg32_elect() {
    uint warp_half = gl_SubgroupInvocationID & 32u;
    uvec4 ballot = subgroupBallot(true);
    return (gl_SubgroupInvocationID & 31u) == uint(findLSB((warp_half == 0u) ? ballot.x : ballot.y));
}
uint sg32_shfl(uint val, uint src_lane) {
    uint warp_half = gl_SubgroupInvocationID & 32u;
    return subgroupShuffle(val, warp_half | (src_lane & 31u));
}
uvec2 sg32_shfl(uvec2 val, uint src_lane) {
    uint warp_half = gl_SubgroupInvocationID & 32u;
    return subgroupShuffle(val, warp_half | (src_lane & 31u));
}
uint sg32_shfl_xor(uint val, uint mask) {
    uint warp_half = gl_SubgroupInvocationID & 32u, lane_id = gl_SubgroupInvocationID & 31u;
    return subgroupShuffle(val, warp_half | ((lane_id ^ mask) & 31u));
}
uint sg32_shfl_up(uint val, uint delta) {
    uint warp_half = gl_SubgroupInvocationID & 32u, lane_id = gl_SubgroupInvocationID & 31u;
    return subgroupShuffle(val, warp_half | ((lane_id - delta) & 31u));
}
uint sg32_shfl_down(uint val, uint delta) {
    uint warp_half = gl_SubgroupInvocationID & 32u, lane_id = gl_SubgroupInvocationID & 31u;
    return subgroupShuffle(val, warp_half | ((lane_id + delta) & 31u));
}"""

# SHFL as Ryujinx HelperFunctionManager.cs GenerateShuffle* (MIT): c = clamp (bits 0-4) | segment mask (bits 8-12);
# an out-of-range source lane keeps the thread's own value and clears the validity predicate. Virtual 32-lane warp
# within Wave32 or Wave64 subgroups.
SHFL_FN = """uint shfl(uint v, uint src, uint c, uint mode, out bool ok) {
    uint warp_half = gl_SubgroupInvocationID & 32u, lane = gl_SubgroupInvocationID & 31u;
    uint seg = (c >> 8) & 31u, lo = lane & seg, hi = (c & 31u & ~seg) | lo;
    uint s;
    if (mode == 0u) { s = (src & ~seg) | lo; ok = s <= hi; }
    else if (mode == 1u) { s = lane - src; ok = int(s) >= int(lo); }
    else if (mode == 2u) { s = lane + src; ok = s <= hi; }
    else { s = lane ^ src; ok = s <= hi; }
    uint r = subgroupShuffle(v, warp_half | (s & 31u));
    return ok ? r : v;
}"""

# LDSM.16.M88 / MT88 as PTX ldmatrix.sync.aligned.m8n8.shared.b16 lays it out (PTX ISA, "Warp-level matrix load
# instruction: ldmatrix"): lanes 8i..8i+7 hold the row addresses of matrix i; lane t receives row t/4, columns
# 2(t%4) and 2(t%4)+1 as one register, or with .trans (MT88) those of the transposed matrix.
LDSM_FN = """uint smem16(uint b) { return (smem[b >> 2] >> ((b & 2u) * 8u)) & 0xffffu; }
uint ldsm(uint a, uint i, bool trans) {
    uint warp_half = gl_SubgroupInvocationID & 32u, lane = gl_SubgroupInvocationID & 31u, r = lane >> 2, q = lane & 3u;
    uint p = subgroupShuffle(a, warp_half | ((i * 8u + r) & 31u)),
         c0 = subgroupShuffle(a, warp_half | ((i * 8u + 2u * q) & 31u)),
         c1 = subgroupShuffle(a, warp_half | ((i * 8u + 2u * q + 1u) & 31u));
    return trans ? smem16(c0 + 2u * r) | (smem16(c1 + 2u * r) << 16) : smem[(p >> 2) + q];
}"""

# MOVM.16.MT88 as PTX movmatrix.sync.aligned.m8n8.trans.b16: lane t holds row t/4, columns 2(t%4) and 2(t%4)+1 of an
# 8x8 matrix of 16-bit elements, and receives the same of its transpose: element (row, col) sits in lane row*4 + col/2.
MOVM_FN = """uint movm(uint v) {
    uint warp_half = gl_SubgroupInvocationID & 32u, lane = gl_SubgroupInvocationID & 31u, r = lane >> 2, q = lane & 3u, sh = (r & 1u) * 16u;
    uint lo = subgroupShuffle(v, warp_half | ((2u * q * 4u + (r >> 1)) & 31u)),
         hi = subgroupShuffle(v, warp_half | (((2u * q + 1u) * 4u + (r >> 1)) & 31u));
    return ((lo >> sh) & 0xffffu) | (((hi >> sh) & 0xffffu) << 16);
}"""

# HMMA.16816.F16 as PTX mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 lays out its fragments (PTX ISA, "Matrix
# Fragments for mma.m16n8k16 with floating point type"): group g = lane/4, t = lane%4; A a0 (row g, k 2t..2t+1), a1
# (row g+8), a2 (row g, k 2t+8..), a3 (row g+8, k 2t+8..); B b0 (k 2t..2t+1, column g), b1 (k 2t+8..); C and D c0 (row
# g, columns 2t..2t+1), c1 (row g+8). Products summed in fp32 and rounded to fp16 once, at the end.
HMMA_FN = """void hmma(uint a0, uint a1, uint a2, uint a3, uint b0, uint b1, uint c0, uint c1, out uint d0, out uint d1) {
    uint warp_half = gl_SubgroupInvocationID & 32u, lane = gl_SubgroupInvocationID & 31u, g = lane >> 2, t = lane & 3u;
    vec2 lo = unpackHalf2x16(c0), hi = unpackHalf2x16(c1);
    for (uint s = 0u; s < 4u; s++) {
        uint from = warp_half | ((g * 4u + s) & 31u);
        vec2 ar0 = unpackHalf2x16(subgroupShuffle(a0, from)), ar1 = unpackHalf2x16(subgroupShuffle(a1, from));
        vec2 ar2 = unpackHalf2x16(subgroupShuffle(a2, from)), ar3 = unpackHalf2x16(subgroupShuffle(a3, from));
        for (uint j = 0u; j < 2u; j++) {
            uint col = warp_half | (((2u * t + j) * 4u + s) & 31u);
            vec2 bk0 = unpackHalf2x16(subgroupShuffle(b0, col)), bk1 = unpackHalf2x16(subgroupShuffle(b1, col));
            lo[j] += dot(ar0, bk0) + dot(ar2, bk1);
            hi[j] += dot(ar1, bk0) + dot(ar3, bk1);
        }
    }
    d0 = packHalf2x16(lo);
    d1 = packHalf2x16(hi);
}"""

# FMNMX returns the other operand when one is NaN and .SAT turns NaN into 0; GLSL's min, max and clamp leave NaN
# undefined (RADV propagates it). GLSL.std.450 NMin, NMax and NClamp say exactly that and lower to one min or max:
# SavageWindRecomp (MIT) runtime/nvn/maxwell/translate.c, -18.5 % ACO instructions there.
NMINMAX_FN = """spirv_instruction(set = "GLSL.std.450", id = 79) float nmin(float a, float b);
spirv_instruction(set = "GLSL.std.450", id = 80) float nmax(float a, float b);
spirv_instruction(set = "GLSL.std.450", id = 81) float nclamp(float x, float lo, float hi);"""

# PRMT (IDX mode), as PTX prmt.b32 d, a, b, sel: the bytes {b, a} (a = bytes 0-3, b = 4-7); output byte k takes the
# byte named by selector nibble k (bits 0-2); bit 3 replicates that byte's sign bit.
PRMT_FN = """uint prmt(uint a, uint sel, uint b) {
    uint r = 0u;
    for (int k = 0; k < 4; k++) {
        uint n = (sel >> (4 * k)) & 15u, src = (n & 4u) != 0u ? b : a;
        uint byte = (src >> (8u * (n & 3u))) & 255u;
        if ((n & 8u) != 0u) byte = (byte & 128u) != 0u ? 255u : 0u;
        r |= byte << (8 * k);
    }
    return r;
}"""

# LOP3's 8-bit truth table: bit (a << 2 | b << 1 | c) of lut is the result for those input bits.
LOP3_FN = """uint lop3(uint a, uint b, uint c, uint l) {
    uint r = 0u;
    if ((l & 1u) != 0u) r |= ~a & ~b & ~c;
    if ((l & 2u) != 0u) r |= ~a & ~b & c;
    if ((l & 4u) != 0u) r |= ~a & b & ~c;
    if ((l & 8u) != 0u) r |= ~a & b & c;
    if ((l & 16u) != 0u) r |= a & ~b & ~c;
    if ((l & 32u) != 0u) r |= a & ~b & c;
    if ((l & 64u) != 0u) r |= a & b & ~c;
    if ((l & 128u) != 0u) r |= a & b & c;
    return r;
}"""


RZ, PT, URZ = 255, 7, 63
MUFU = {"COS": "cos({})", "SIN": "sin({})", "EX2": "exp2({})", "LG2": "log2({})", "RCP": "(1.0 / {})",
        "RSQ": "inversesqrt({})", "SQRT": "sqrt({})"}  # fmt: skip
# SM70+ MUFU.SIN/COS take turns: sin(2*pi*x) (eden-nx2 NX2_NOTES.md row 95)
TURNS = {"COS", "SIN"}
# Float compares: glslc makes < <= > >= == ordered (false with a NaN) and != unordered (OpFUnordNotEqual); SM86's ..U
# forms are true with a NaN and NE is ordered, as PTX setp.ltu / setp.ne. Read as the plain operator, GEU/GTU/LEU (45 %
# of MKW's FSETP) were false with a NaN.
FCMP = {"LT": "({a} < {b})", "EQ": "({a} == {b})", "LE": "({a} <= {b})", "GT": "({a} > {b})", "GE": "({a} >= {b})",
        "NE": "({a} < {b} || {a} > {b})", "NEU": "({a} != {b})", "LTU": "!({a} >= {b})", "LEU": "!({a} > {b})",
        "GTU": "!({a} <= {b})", "GEU": "!({a} < {b})", "EQU": "!({a} < {b} || {a} > {b})"}  # fmt: skip


def fcmp(op: str, a: str, b: str) -> str | None:
    """The bool GLSL of float compare op on a and b (operand expressions, braces-free), None if op is unknown."""
    t = FCMP.get(op)
    return t.replace("{a}", f"({a})").replace("{b}", f"({b})") if t else None
ICMP = {"LT": "<", "EQ": "==", "LE": "<=", "GT": ">", "NE": "!=", "GE": ">="}
BOP = {"AND": "&&", "OR": "||", "XOR": "!="}


def lop3_forms() -> dict[int, str]:
    """LOP3 lookup table -> the shortest expression of a, b, c with ~ & | ^ that has it (breadth first over up to three
    operators, operands possibly negated): masks a = 0xF0, b = 0xCC, c = 0xAA as Ryujinx's LOP3 decoding."""
    leaves = {0xF0: "{a}", 0xCC: "{b}", 0xAA: "{c}", 0x0F: "~{a}", 0x33: "~{b}", 0x55: "~{c}", 0x00: "0u",
              0xFF: "0xffffffffu"}  # fmt: skip
    forms = dict(leaves)
    for _ in range(3):
        found = {}
        for m1, e1 in list(forms.items()):
            for m2, e2 in leaves.items():
                for m, op in ((m1 & m2, "&"), (m1 | m2, "|"), (m1 ^ m2, "^")):
                    if m not in forms and m not in found:
                        found[m] = f"({e1} {op} {e2})"
        forms.update(found)
    return forms


LOP3_FORMS = lop3_forms()


def reg(n: int) -> str:
    return "0u" if n == RZ else f"r{n}"


def multi_load(loads: list[tuple[int, str]]) -> str:
    """Several destination registers from several reads: every word is read before any is written, because the address
    may sit in a destination (`LDG.E.128 R16, [R16.64]` read three words from the first one's value: kart and track
    FS lit by garbage, a black race, 31/10/2026)."""
    if len(loads) < 2:
        return " ".join(f"r{n} = {w};" for n, w in loads)
    tmp = ", ".join(f"lw{k} = {w}" for k, (_, w) in enumerate(loads))
    return f"{{ uint {tmp}; {' '.join(f'r{n} = lw{k};' for k, (n, _) in enumerate(loads))} }}"


def pred(n: int, neg: int = 0, u: bool = False) -> str:
    p = "true" if n == PT else f"{'up' if u else 'p'}{n}"
    return f"!{p}" if neg else p


def fbits(x: int) -> str:
    """A float immediate given as its bit pattern, as an exact GLSL float."""
    return f"uintBitsToFloat({x:#x}u)"
