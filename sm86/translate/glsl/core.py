"""SM86 program -> GLSL 4.6 (version 0: straight translation, a PC dispatcher for control flow).

Registers R0..R254 are `uint` variables (RZ reads 0), predicates P0..P6 `bool` (PT true). Each instruction becomes C-like
statements on them; float operations go through uintBitsToFloat / floatBitsToUint. Control flow: every branch target
and every instruction after a branch starts a `case` of a switch on `pc` inside a loop, so any jump is `pc = T;
continue;` and straight code falls through. Correct for any control flow; structuring comes later.

Resources (version 0): constant bank N is `cbN.d[]` (uvec4, std140), byte address A is `.d[A >> 4][(A >> 2) & 3]`;
at a dynamic address, its word view `cbNw.w[]` (mem.cb_word).
PS generic inputs come from IPA a[0x80 + 16 i + 4 c] -> `in_attr[i][c]`; colour outputs are the registers the SPH maps
(OmapTargets: 4 enable bits per render target, registers assigned in order from R0). VS ALD/AST read and write
a[0x80 + 16 i] generics and a[0x70] position. Anything not handled raises Unsupported with the instruction class.
"""

import os
import re
import struct

import flow
from program import Insn, Sph

from glsl.alu import AluOps
from glsl.common import MUFU, PT, RZ, TURNS, URZ, Unsupported, pred, reg
from glsl.head import (
    MESH_MAIN,
    MESH_MAIN_CS,
    TASK_MAIN,
    TASK_MAIN_CS,
    declarations,
    mesh_head,
    mesh_head_cs,
    task_head,
    task_head_cs,
)
from glsl.mem import MemOps
from glsl.tex import TexOps
from glsl.thread import ThreadOps

# SM86_SPEC=1: fixed-address constant words read through specialization constants (Builder.const) for MKW_SPEC's
# variants. Off by default: on the Radeon 660M the variants left the GPU time as it was (sp-ab2, 01/11/2026) and the
# constants make the SPIR-V 43 % larger.
SPEC_ON = os.environ.get("SM86_SPEC", "0") == "1"


class Builder(AluOps, MemOps, TexOps, ThreadOps):
    def __init__(self, sph: Sph | None, stage: str) -> None:
        self.sph, self.stage = sph, stage
        self.cbufs: set[int] = set()
        self.cbufs_w: set[int] = set()  # banks also read at dynamic addresses, through their word view (mem.cb_word)
        self.spec: dict[tuple[int, int], int] = {}  # (bank, byte) -> k of a fixed-address constant word (const())
        self.in_attrs: set[int] = set()
        self.out_attrs: set[int] = set()
        self.regs: set[int] = set()
        self.uregs: set[int] = set()
        self.uses_lop3 = False
        self.uses_subgroup = self.uses_shfl = self.uses_shared = self.uses_prmt = self.uses_arith = False
        self.uses_nminmax = False
        self.txq_tex: set[tuple[int, int]] = set()  # textures TXQ reads the size of (txqsz_* functions)
        self.uses_shadow_lod = False
        self.lmem_bytes = 0  # local memory (LDL/STL): the furthest constant extent, 0 = none
        self.task = False  # a task program (VS header, ISBEWR to its payload): set by translate()
        self.mesh = False  # a mesh program (ISBEWR.SKEW per-vertex writes): set by translate()
        self.images: dict[tuple[int, int], str] = {}
        self.ssbos: set[int] = set()  # driver bank 0 offsets of the storage buffer addresses read
        self.ssbos_written: set[int] = set()  # those a STG, ATOMG or RED stores into (reflection flag 1)
        self.ur_ssbo: dict[int, int] = {}  # uniform register -> the c[0] offset its 64-bit value came from
        self.reg_ssbo: dict[int, int] = {}
        self.ur_derived: dict[
            int, int
        ] = {}  # uniform register -> storage buffer it points into (an address, not the base)
        # IPA w fold (fold_ipa_w): register -> ("w",) gl_FragCoord.w, ("rw",) its reciprocal, ("aw", plain) a
        # perspective varying times w; valid in straight-line code, forgotten at jump targets, after jumps and when
        # a predicated instruction may write the register (predicated: set by translate for the current one).
        self.fold: dict[int, tuple] = {}
        self.predicated = False
        self.fold_new: set[int] = (
            set()
        )  # registers the current instruction gave a fold state  # register -> the c[0] offset of the storage buffer its address is in
        self.textures: dict[tuple[int, int], str] = {}  # (handle bank, handle index) -> GLSL sampler type
        self.uses_ldsm = self.uses_hmma = self.uses_movm = False  # LDSM_FN, HMMA_FN, MOVM_FN in the head
        self.gsearch_load = self.gsearch_store = False  # global_search used: gld/gst go into the head

    # ---- operands
    def const(self, bank: int, byte: int, spec: bool = True) -> str:
        """A constant word at a fixed address. With spec, word k reads through specialization constants SCU{k} (bool,
        constant_id 2k) and SCV{k} (uint, 2k + 1): the back end may build a pipeline with a value it saw in every draw
        fixed, and the driver folds what depends on it. Unset, SCU is false and
        the driver keeps the plain read. Storage buffer addresses stay plain: the traced bases subtract them."""
        if bank == 0 and byte == 0x644 and self.stage == "vs" and not (self.task or self.mesh):
            # The draw's base instance, which NVN's indirect draw macros write into c0 from the GPU-written arguments
            # (eden-nx2 macro.cpp:149-155 swaps the read for the BaseInstance attribute; an idea). The runtime can only
            # fill c0 for direct draws. 139 VS add it to InstanceId; on indirect draws the trees' VS read the first
            # instances' matrices and materials: trees out of place and black (06/11/2026).
            return "uint(gl_BaseInstance)"
        self.cbufs.add(bank)
        word = f"cb{bank}.d[{byte >> 4}][{(byte >> 2) & 3}]"
        if not spec or not SPEC_ON:
            return word
        k = self.spec.setdefault((bank, byte), len(self.spec))
        return f"(SCU{k} ? SCV{k} : {word})"

    def src_u(self, i: Insn, slot: str) -> str:
        """Operand slot 'a'/'b'/'c' as a uint expression: register, immediate or constant."""
        if i.has(f"R{slot}"):
            n = i.v(f"R{slot}")
            if n != RZ:
                self.regs.add(n)
            return reg(n)
        if i.has(f"UR{slot}"):
            n = i.v(f"UR{slot}")
            if n == URZ:
                return "0u"
            self.uregs.add(n)
            return f"ur{n}"
        if i.has(f"S{slot}"):
            return f"{i.v(f'S{slot}') & 0xFFFFFFFF:#x}u"
        for k, iv in i.f.items():
            if k.startswith("ConstBankAddress") and f"S{slot}_bank" in k:
                bank, word = (v for _, v in iv.slot_values)
                return self.const(bank, word * 4)
        raise Unsupported(f"{i.cls}: operand {slot}")

    @staticmethod
    def operand(i: Insn, slot: str) -> str:
        """Name of operand slot's field, whose @negate/@absolute/@invert flags go with it. A uniform register is URb or
        URc: reading Sc's flags for it dropped the minus of FADD R33, R1, -UR11 (the title's colour-grading LUT got
        blue = graded + 2 x input, 31/10/2026)."""
        return next((n for n in (f"R{slot}", f"UR{slot}", f"S{slot}") if i.has(n)), f"S{slot}")

    def src_f(self, i: Insn, slot: str) -> str:
        e = f"uintBitsToFloat({self.src_u(i, slot)})"
        name = self.operand(i, slot)
        if i.v(f"{name}@absolute", 0):
            e = f"abs({e})"
        if i.v(f"{name}@negate", 0):
            e = f"(-{e})"
        return e

    def dst(self, i: Insn, expr_u: str) -> str:
        if i.has("URd"):
            n = i.v("URd")
            if n == URZ:
                return ""
            self.uregs.add(n)
            return f"ur{n} = {expr_u};"
        n = i.v("Rd")
        if n == RZ:
            return ""
        self.regs.add(n)
        self.fold.pop(n, None)
        return f"r{n} = {expr_u};"

    def dst_f(self, i: Insn, expr_f: str) -> str:
        if i.sym("sat") == "SAT":
            self.uses_nminmax = True
            expr_f = f"nclamp({expr_f}, 0.0, 1.0)"
        return self.dst(i, f"floatBitsToUint({expr_f})")

    # ---- instructions
    def insn(self, i: Insn) -> str:
        op = i.op
        # BREAK only takes the thread out of a convergence barrier (BSSY/BSYNC, dropped too); the BRA after it with
        # the same predicate does the jump (agl_technique_shdw: `@!P1 BREAK B2; @!P1 BREAK B1; @!P1 BRA`)
        if op in ("NOP", "DEPBAR", "BSSY", "BSYNC", "BREAK", "WARPSYNC", "MEMBAR", "ERRBAR"):
            return ""
        if op == "FMUL":
            plain = self.fold_ipa_w(i)
            if plain:
                return self.dst_f(i, plain)
            return self.dst_f(i, f"({self.src_f(i, 'a')} * {self.src_f(i, 'b')})")
        if op == "FADD":
            return self.dst_f(i, f"({self.src_f(i, 'a')} + {self.src_f(i, 'c')})")
        if op == "FFMA":
            return self.dst_f(i, f"fma({self.src_f(i, 'a')}, {self.src_f(i, 'b')}, {self.src_f(i, 'c')})")
        if op == "MOV":
            return self.dst(i, self.src_u(i, "b"))
        if op == "MUFU":
            fn = i.sym("mufuop")
            if fn not in MUFU:
                raise Unsupported(f"MUFU.{fn}")
            x = self.src_f(i, "b")
            if fn in TURNS:
                x = f"({x} * 6.2831853071795864)"
            w = (
                fn == "RCP"
                and i.has("Rb")
                and self.fold.get(i.v("Rb")) == ("w",)
                and x == f"uintBitsToFloat(r{i.v('Rb')})"
            )
            out = self.dst_f(i, MUFU[fn].format(x))
            if w:
                self.fold_set(i, ("rw",))
            return out
        if op == "FMNMX":
            p = pred(i.v("Pp"), i.v("Pp@not"))
            a, b = self.src_f(i, "a"), self.src_f(i, "b")
            self.uses_nminmax = True
            return self.dst_f(i, f"({p} ? nmin({a}, {b}) : nmax({a}, {b}))")
        if op == "FSEL":
            return self.dst_f(i, f"({pred(i.v('Pp'), i.v('Pp@not'))} ? {self.src_f(i, 'a')} : {self.src_f(i, 'b')})")
        if op in ("FSETP", "ISETP", "UISETP"):
            return self.setp(i)
        if op in ("F2FP", "R2UR", "I2F", "UP2UR", "P2R", "TXQ", "FLO", "SGXT", "IABS", "UBREV", "PRMT", "UFLO",
                  "R2P", "ULEA", "USEL", "UPLOP3", "SUST", "SULD", "SURED", "SUATOM"):  # fmt: skip
            return getattr(self, op.lower())(i)
        if op == "IPA":
            return self.ipa(i)
        if op == "ULDC":
            return self.uldc(i)
        if op in ("LDG", "STG"):
            return self.global_mem(i)
        if op == "LDC":
            return self.ldc(i)
        if op in ("TEX", "TLD4"):
            return self.tex(i)
        if op == "TLD":
            return self.tld(i)
        if op == "TMML":
            return self.tmml(i)
        if op in ("S2R", "S2UR"):
            return self.s2r(i)
        if op in ("LDS", "STS"):
            return self.shared_mem(i)
        if op in ("LDL", "STL"):
            return self.local_mem(i)
        if op == "BAR":
            if i.sym("barmode") != "SYNC" or i.v("Sb", 0) != 0:
                raise Unsupported(f"BAR.{i.sym('barmode')}")
            return "barrier();"
        if op in ("LDSM", "HMMA", "MOVM", "HMNMX2"):
            return getattr(self, op.lower())(i)
        if op == "SHFL":
            return self.shfl(i)
        if op in ("VOTE", "VOTEU"):
            return self.vote(i)
        if op == "ELECT":
            return self.elect(i)
        if op == "UMOV":
            n = i.v("URd")
            if n == URZ:
                return ""
            self.uregs.add(n)
            return f"ur{n} = {self.src_u(i, 'b')};"
        if op == "PLOP3":
            return self.plop3(i)
        if op == "KILL":
            return f"if ({pred(i.v('Pp'), i.v('Pp@not'))}) discard;"
        if op == "LEA":
            return self.lea(i)
        if op == "USHF":
            return self.shf(i, "U")
        if op in ("IADD3", "IMAD", "SHF", "LOP3", "ULOP3", "SEL", "CS2R", "F2I", "I2FP", "FRND", "FSET", "UIADD3",
                  "UIMAD", "IMNMX"):  # fmt: skip
            return getattr(self, op.lower())(i)
        if op in ("ATOMG", "RED"):
            return self.atom_global(i)
        if op == "UPOPC":
            return self.dst(i, f"uint(bitCount({self.src_u(i, 'b')}))")
        if op in ("HADD2", "HSETP2", "MATCH", "POPC", "REDUX"):
            return getattr(self, op.lower())(i)
        if op == "FSWZADD":
            return self.fswzadd(i)
        if op in ("ALD", "AST"):
            return self.attr_mem(i)
        if op in ("ISBEWR", "ISBERD") and self.task:
            return self.task_isbe(i)
        if op in ("ISBEWR", "ISBERD") and self.mesh:
            return self.mesh_isbe(i)
        if op == "EXIT":
            return self.exit()
        raise Unsupported(i.cls)


def inline_single_call(insns: list[Insn]) -> list[Insn]:
    """A subroutine with one caller placed at its call: the body moved after the CALL, both CALL and RET dropped, so
    control falls into it and back out in list order and flow.py can structure the program (the four largest effect
    compute programs kept the switch for one CALL/RET pair, 31/10/2026). Branch targets are absolute pcs, so moving
    code keeps them. Only when CALL and RET are unpredicated, the body [target, RET] is contiguous, nothing else jumps
    into it and the instruction before it does not fall into it."""
    calls = [k for k, i in enumerate(insns) if i.op == "CALL"]
    rets = [k for k, i in enumerate(insns) if i.op == "RET"]
    if len(calls) != 1 or len(rets) != 1:
        return insns
    c, r = calls[0], rets[0]
    call, ret = insns[c], insns[r]
    if any(x.v("Pg", PT) != PT or x.v("Pg@not", 0) for x in (call, ret)):
        return insns
    target = call.pc + 16 + 4 * call.sv("Sa")
    t = next((k for k, i in enumerate(insns) if i.pc == target), None)
    if t is None or not (c < t <= r or r < c):  # body after the call site, or wholly before it
        return insns
    if t <= c <= r or t == 0 or insns[t - 1].op not in ("EXIT", "BRA") or insns[t - 1].v("Pg", PT) != PT:
        return insns
    body = range(t, r)
    lo, hi = insns[t].pc, ret.pc
    for k, i in enumerate(insns):
        if k in body or i.op not in ("BRA", "CALL"):
            continue
        dest = i.pc + 16 + 4 * i.sv("sImm" if i.op == "BRA" else "Sa")
        if lo <= dest <= hi and k != c:
            return insns
    for k in body:
        i = insns[k]
        if i.op == "BRA" and not lo <= i.pc + 16 + 4 * i.sv("sImm") <= hi:
            return insns  # leaves the body some other way
    rest = [i for k, i in enumerate(insns) if k not in body and k not in (c, r)]
    at = rest.index(insns[c + 1]) if c + 1 < len(insns) else len(rest)
    return rest[:at] + [insns[k] for k in body] + rest[at:]


def compute_shape(control: bytes) -> tuple[int, int, int, int]:
    """Workgroup size (x, y, z) and shared memory bytes of a compute program: words 0x58-0x64 of its 0xd8-byte NVN2
    control section (measured 31/10/2026 over the 728 compute stages: sizes 64x1x1, 32x1x1, 8x8x1, 4x4x4..., and no
    LDS/STS/ATOMS offset of the 473 with SASS reaches the shared size; NVN1 kept them at control+0x748)."""
    x, y, z, smem = struct.unpack_from("<4I", control, 0x58)
    return x or 1, y or 1, z or 1, smem


def translate(
    sph: Sph | None, insns: list[Insn], stage: str, shape: tuple[int, int, int, int] | None = None
) -> tuple[str, dict]:
    """shape: compute_shape() of a compute program; without it the workgroup is 1x1x1 (a census without controls)."""
    if stage not in ("ps", "vs", "cs", "tes?"):
        raise Unsupported(f"stage {stage}")
    b = Builder(sph, stage)
    insns = inline_single_call(insns)
    writes = [i for i in insns if i.op == "ISBEWR"]
    # a VS header with payload writes only (no per-vertex SKEW writes, no index bytes) is a task program
    b.task = stage == "vs" and bool(writes) and all(i.sym("base") == "ATTR" and i.sym("skew") != "SKEW" for i in writes)
    b.mesh = any(i.sym("skew") == "SKEW" for i in writes)
    if stage == "tes?" and not b.mesh:
        raise Unsupported(f"stage {stage}")
    targets = {0}
    for i in insns:
        if i.op == "BRA":
            targets.add(i.pc + 16 + 4 * i.sv("sImm"))
            targets.add(i.pc + 16)
        elif i.op == "CALL":
            # CALL.REL.NOINC is a plain jump: the caller stored its return address itself (MOV Rn, pc + 16 just
            # before), and RET.REL.NODEC jumps back through that register.
            targets.add(i.pc + 16 + 4 * i.sv("Sa"))
            targets.add(i.pc + 16)
        elif i.op in ("RET", "EXIT"):
            targets.add(i.pc + 16)
    if any(i.op in ("BRX", "BRXU") for i in insns):
        targets |= {i.pc for i in insns}  # targets come from a table in c[1] at run time: any instruction may be one
    body = []
    stmts: list[str] = []  # per instruction, for the structured form (flow.py)
    branches: dict[int, tuple[int, str | None]] = {}
    # A CALL in a program without RET never comes back: a jump (DrivePassShaderDefault 294, 256 KB, whose switch form
    # took RADV 4 GB to compile in run92). Structured like a BRA then.
    call_jumps = not any(i.op == "RET" for i in insns)
    structurable = not any(i.op in ("RET", "BRX", "BRXU") or (i.op == "CALL" and not call_jumps) for i in insns)
    index = {i.pc: k for k, i in enumerate(insns)}
    for i in insns:
        if i.pc in targets:
            body.append(f"    case {i.pc}u:")
            if i.pc:
                b.fold.clear()  # another path may arrive here
        b.predicated = i.v("Pg", PT) != PT or bool(i.v("Pg@not", 0)) or i.v("UPg", PT) != PT
        if i.op == "CALL":
            if i.sym("depth") != "NOINC":
                raise Unsupported(f"CALL.{i.sym('depth')}")
            target = i.pc + 16 + 4 * i.sv("Sa")
            stmt = f"{{ pc = {target}u; continue; }}"
            if call_jumps:
                b.fold.clear()
                cond = pred(i.v("Pg"), i.v("Pg@not")) if i.v("Pg", PT) != PT or i.v("Pg@not", 0) else None
                branches[len(stmts)] = (index.get(target, len(insns)), cond)
                if target not in index and target < insns[-1].pc + 16:
                    structurable = False
        elif i.op == "RET":
            if i.sym("depth") != "NODEC" or i.sym("addr") != "REL":
                raise Unsupported(f"RET.{i.sym('addr')}.{i.sym('depth')}")
            # Target = Ra + (pc + 16) + 4 * offset; the offset usually cancels pc + 16 (the register holds the
            # absolute return address).
            b.regs.add(i.v("Ra"))
            stmt = f"{{ pc = {reg(i.v('Ra'))} + {(i.pc + 16 + 4 * i.sv('Ra_offset')) & 0xFFFFFFFF}u; continue; }}"
        elif i.op in ("BRX", "BRXU"):
            # BRX Ra offset: target = Ra + pc + 16 + offset; ptxas makes the offset cancel pc + 16 and loads the
            # absolute target from a jump table in c[1] (census: "BRX R0 -0xe0" at 0xd0).
            if i.has("URa"):  # BRXU: the same with a uniform register
                b.uregs.add(i.v("URa"))
                base = f"ur{i.v('URa')}" if i.v("URa") != URZ else "0u"
            else:
                b.regs.add(i.v("Ra"))
                base = reg(i.v("Ra"))
            off = i.sv("UR_offset" if i.op == "BRXU" else "Ra_offset")
            stmt = f"{{ pc = {base} + {(i.pc + 16 + 4 * off) & 0xFFFFFFFF}u; continue; }}"
        elif i.op == "BRA":
            b.fold.clear()
            target = i.pc + 16 + 4 * i.sv("sImm")
            # A branch to itself is the closing loop after the last EXIT: never reached, but must not spin.
            stmt = "return;" if target == i.pc else f"{{ pc = {target}u; continue; }}"
            # BRA's own predicate operand (`@!P2 BRA P4, target`): the jump is taken when the guard and it both hold
            # (eden-nx2 lower_to_maxwell.h:2178-2193). Dropped, 612 jumps in 317 programs went on the guard alone: the
            # wave simulation's atan2 skipped the kx = 0 column and filled its FFT with NaN (01/11/2026).
            pp = pred(i.v("Pp"), i.v("Pp@not")) if i.has("Pp") and (i.v("Pp", PT) != PT or i.v("Pp@not", 0)) else None
            if pp and target != i.pc:
                stmt = f"if ({pp}) {stmt}"
            if target != i.pc:
                cond = None
                if i.v("Pg", PT) != PT or i.v("Pg@not", 0):
                    cond = pred(i.v("Pg"), i.v("Pg@not"))
                elif i.v("UPg", PT) != PT or i.v("UPg@not", 0):
                    cond = pred(i.v("UPg"), i.v("UPg@not"), True)
                if pp:
                    cond = pp if cond is None else f"({cond}) && ({pp})"
                branches[len(stmts)] = (index.get(target, len(insns)), cond)
                if target not in index and target < insns[-1].pc + 16:
                    structurable = False  # into the middle of nothing known
        else:
            b.fold_new.clear()
            stmt = b.insn(i)
            b.track_ssbo(i)
            for n in {int(x) for x in re.findall(r"\br(\d+) = ", stmt or "")} - b.fold_new:
                b.fold.pop(n, None)  # written outside dst() (pairs, vectors): no longer what the fold knew
        if stmt:
            if i.v("Pg", PT) != PT or i.v("Pg@not", 0):
                stmt = f"if ({pred(i.v('Pg'), i.v('Pg@not'))}) {{ {stmt} }}"
            elif i.v("UPg", PT) != PT or i.v("UPg@not", 0):
                stmt = f"if ({pred(i.v('UPg'), i.v('UPg@not'), True)}) {{ {stmt} }}"
            body.append(f"        {stmt}")
        stmts.append(stmt if i.op not in ("BRA", "CALL") or len(stmts) not in branches else "")
    structured = None
    if structurable and branches:
        try:
            structured = flow.structure(stmts, branches)
        except flow.Fail:
            pass
    elif structurable:
        structured = [f"        {x}" for x in stmts if x]
    gsearch = b.global_search_fns() + b.ipa_dyn_fns()  # before the declarations: it reads bank 0 and may mark buffers written
    head = declarations(b, stage, shape)
    regs = ", ".join([f"r{n} = 0u" for n in sorted(b.regs)] + [f"ur{n} = 0u" for n in sorted(b.uregs)]) or "rdummy = 0u"
    preds = ", ".join([f"p{n} = false" for n in range(7)] + [f"up{n} = false" for n in range(7)])
    # precise: no FMUL + FADD fused into an FMA by the compiler (NoContraction, as eden-nx2 marks each FADD, FMUL and
    # FFMA: frontend/maxwell/translate/impl/floating_point_add.cpp:25, floating_point_multiply.cpp:66). Two programs
    # computing one position (a depth pre-pass and an EQUAL colour pass) must get the same bits, as on the console;
    # fused differently, the MKW bushes failed the EQUAL test and stayed black (01/11/2026).
    decl = [f"    precise uint {regs};", f"    bool {preds};", "    uint loops = 0u;"]
    if b.task or b.mesh:
        decl.append("    uint virtual_warp_id = gl_LocalInvocationIndex >> 5u, virtual_lane_id = gl_LocalInvocationIndex & 31u;")
    head_cs = None   # task/mesh: the same body under the compute wrappers (b.reflection["cs_glsl"])
    if b.task:
        head_cs, tail_cs = task_head_cs(head), TASK_MAIN_CS
        head = task_head(head)
        tail_task = TASK_MAIN
    elif b.mesh:
        head_cs, tail_cs = mesh_head_cs(head), MESH_MAIN_CS
        head = mesh_head(head)
        tail_task = MESH_MAIN
    else:
        tail_task = ""
    head = [*head, *gsearch]
    if head_cs is not None:
        head_cs = [*head_cs, *gsearch]
    entry = "void sm_main() {" if b.task or b.mesh else "void main() {"
    if structured is not None:
        prog = [entry, *decl, *structured, "}"]
    else:
        main = [*decl, "    uint pc = 0u;", "    for (; loops < LOOP_CAP; loops++) {", "    switch (pc) {"]
        prog = [entry, *main, *body, "    default: return;", "    }", "    }", "}"]
    if head_cs is not None:
        b.reflection["cs_glsl"] = "\n".join([*head_cs, *prog, tail_cs])
    return "\n".join([*head, *prog, tail_task]), b.reflection
