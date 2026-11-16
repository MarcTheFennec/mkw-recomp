"""SM86 -> GLSL, glsl.core.Builder methods for threads and subgroups, system values, task/mesh outputs and EXIT. A mixin: it
uses the Builder's operand helpers (src_u, dst...) and state."""

from typing import ClassVar

from program import Insn

from glsl.common import PT, RZ, URZ, Unsupported, multi_load, pred, reg


class ThreadOps:
    def fswzadd(self, i: Insn) -> str:
        """FSWZADD Rd, Ra, Rc, mask: per quad lane, Ra * {1,1,-1,0}[k] + Rc * {1,-1,1,1}[k] with k the lane's 2 bits
        of the mask; used for derivatives across a quad. SM86 swaps codes 1 and 2 against Maxwell's table (Ryujinx
        SwizzleAdd.glsl, MIT): nvdisasm names code 2 NP (c - a) and 3 ZP (c), as eden-nx2 lower_to_maxwell.h FSWZADD."""
        self.uses_subgroup = True
        k = f"(({i.v('npCtrl')}u >> ((gl_SubgroupInvocationID & 3u) * 2u)) & 3u)"
        a, c = self.src_f(i, "a"), self.src_f(i, "c")
        return self.dst_f(i, f"({a} * vec4(1.0, 1.0, -1.0, 0.0)[{k}] + {c} * vec4(1.0, -1.0, 1.0, 1.0)[{k}])")

    REDUX: ClassVar[dict[str, str]] = {"SUM": "subgroupAdd", "MIN": "subgroupMin", "MAX": "subgroupMax",
                                       "AND": "subgroupAnd", "OR": "subgroupOr", "XOR": "subgroupXor"}  # fmt: skip

    def redux(self, i: Insn) -> str:
        """REDUX: a reduction over the active lanes into a uniform register (unsigned unless .S32)."""
        op = i.sym("op")
        if op not in self.REDUX:
            raise Unsupported(f"REDUX.{op}")
        self.uses_subgroup = self.uses_arith = True
        a = self.src_u(i, "a")
        signed = i.sym("sz") == "S32"
        e = f"uint({self.REDUX[op]}(int({a})))" if signed else f"{self.REDUX[op]}({a})"
        # Keep the native reduction on 32-lane hosts. Wider subgroups reduce only active lanes of this logical warp;
        # a subgroup-wide reduction would mix independent NVIDIA warps even though each receives a uniform result.
        initial = {"SUM": "0u", "AND": "0xffffffffu", "OR": "0u", "XOR": "0u",
                   "MIN": "0x7fffffffu" if signed else "0xffffffffu",
                   "MAX": "0x80000000u" if signed else "0u"}[op]
        combine = {"SUM": "rv + rx", "AND": "rv & rx", "OR": "rv | rx", "XOR": "rv ^ rx",
                   "MIN": "uint(min(int(rv), int(rx)))" if signed else "min(rv, rx)",
                   "MAX": "uint(max(int(rv), int(rx)))" if signed else "max(rv, rx)"}[op]
        return (f"if (gl_SubgroupSize <= 32u) {{ {self.dst(i, e)} }} else {{ "
                f"uint ra = {a}, rv = {initial}, wmask = warp_ballot(true); "
                f"for (uint k = 0u; k < 32u; k++) {{ uint rx = warp_shuffle(ra, k); "
                f"if (((wmask >> k) & 1u) != 0u) rv = {combine}; }} {self.dst(i, 'rv')} }}")

    SR: ClassVar[dict[str, str]] = {
        "SR_TID.X": "gl_LocalInvocationID.x", "SR_TID.Y": "gl_LocalInvocationID.y", "SR_TID.Z": "gl_LocalInvocationID.z",
        "SR_CTAID.X": "gl_WorkGroupID.x", "SR_CTAID.Y": "gl_WorkGroupID.y", "SR_CTAID.Z": "gl_WorkGroupID.z",
        "SR_LANEID": "(gl_SubgroupInvocationID & 31u)",
        "SR_LTMASK": "gl_SubgroupLtMask[gl_SubgroupInvocationID >> 5u]",
        "SR_LEMASK": "gl_SubgroupLeMask[gl_SubgroupInvocationID >> 5u]",
        "SR_GTMASK": "gl_SubgroupGtMask[gl_SubgroupInvocationID >> 5u]",
        "SR_GEMASK": "gl_SubgroupGeMask[gl_SubgroupInvocationID >> 5u]",
        "SR_EQMASK": "gl_SubgroupEqMask[gl_SubgroupInvocationID >> 5u]",
        "SRZ": "0u", "SR255": "0u",
    }  # fmt: skip

    def s2r(self, i: Insn) -> str:
        names = i.f["SRa"].symbols or []
        sr = next((n for n in names if n in self.SR), None)
        if sr is None:
            raise Unsupported(f"{i.op} {'/'.join(names)}")
        self.uses_subgroup |= "Subgroup" in self.SR[sr]
        if i.op == "S2UR":
            n = i.v("URd")
            if n == URZ:
                return ""
            self.uregs.add(n)
            return f"ur{n} = {self.SR[sr]};"
        return self.dst(i, self.SR[sr])

    def ureg(self, n: int) -> str:
        if n == URZ:
            return "0u"
        self.uregs.add(n)
        return f"ur{n}"

    def ldsm(self, i: Insn) -> str:
        """LDSM.16.M88.4 / MT88.4: four 8x8 matrices of 16-bit elements from shared memory, matrix k into Rd+k (ldsm in
        LDSM_FN; the row addresses come from the other lanes). The x1 and x2 forms do not occur in MKW."""
        if i.sym("sz") != "16" or i.sym("mode") not in ("M88", "MT88") or i.sym("num") != "4":
            raise Unsupported(f"LDSM.{i.sym('sz')}.{i.sym('mode')}.{i.sym('num')}")
        self.uses_subgroup = self.uses_shared = self.uses_ldsm = True
        addr = f"({self.src_u(i, 'a')} + {i.sv('Ra_offset') & 0xFFFFFFFF:#x}u)"
        n, trans = i.v("Rd"), "true" if i.sym("mode") == "MT88" else "false"
        loads = [(n + k, f"ldsm({addr}, {k}u, {trans})") for k in range(4) if n != RZ]
        self.regs.update(r for r, _ in loads)
        return multi_load(loads)

    def movm(self, i: Insn) -> str:
        """MOVM.16.MT88: the 8x8 matrix of 16-bit elements spread over the lanes, transposed (movm in MOVM_FN)."""
        if i.sym("sz") != "16" or i.sym("mode") != "MT88":
            raise Unsupported(f"MOVM.{i.sym('sz')}.{i.sym('mode')}")
        self.uses_subgroup = self.uses_movm = True
        return self.dst(i, f"movm({self.src_u(i, 'a')})")

    def hmma(self, i: Insn) -> str:
        """HMMA.16816.F16: D (Rd, Rd+1) = A (Ra..Ra+3) x B (Rb, Rb+1) + C (Rc, Rc+1), the fragments of a 16x16 by 16x8
        fp16 product spread over the 32 lanes (hmma in HMMA_FN)."""
        if i.sym("size") != "16816" or i.sym("dstfmt") != "F16" or i.sym("srcfmt") != "F16":
            raise Unsupported(f"HMMA.{i.sym('size')}.{i.sym('dstfmt')}.{i.sym('srcfmt')}")
        if i.v("Ra@negate", 0) or i.v("Rb@negate", 0):
            raise Unsupported("HMMA with a negated source")
        self.uses_subgroup = self.uses_hmma = True

        def regs(name: str, n: int) -> list[str]:
            r = i.v(name)
            if r != RZ:
                self.regs.update(range(r, r + n))
            return [reg(r + k if r != RZ else RZ) for k in range(n)]

        a, b, c, d = regs("Ra", 4), regs("Rb", 2), regs("Rc", 2), i.v("Rd")
        call = f"hmma({', '.join(a + b + c)}, hd0, hd1);"
        if d == RZ:
            return f"{{ uint hd0, hd1; {call} }}"
        self.regs.update((d, d + 1))
        return f"{{ uint hd0, hd1; {call} r{d} = hd0; r{d + 1} = hd1; }}"

    def shfl(self, i: Insn) -> str:
        mode = {"IDX": 0, "UP": 1, "DOWN": 2, "BFLY": 3}.get(i.sym("shflmd"))
        if mode is None:
            raise Unsupported(f"SHFL.{i.sym('shflmd')}")
        self.uses_subgroup = self.uses_shfl = True
        pu, n = i.v("Pu"), i.v("Rd")
        call = f"shfl({self.src_u(i, 'a')}, {self.src_u(i, 'b')}, {self.src_u(i, 'c')}, {mode}u, sok)"
        out = [f"uint sr = {call};"]
        if n != RZ:
            self.regs.add(n)
            out.append(f"r{n} = sr;")
        if pu != PT:
            out.append(f"p{pu} = sok;")
        return "{ bool sok; " + " ".join(out) + " }"

    def match(self, i: Insn) -> str:
        """MATCH.ANY: the mask of the active lanes whose Ra equals this lane's; MATCH.ALL: the active mask and Pu set
        when every active lane in the logical 32-lane warp holds the same Ra, else 0 and Pu clear."""
        if i.sym("sz") not in ("U32", "U64"):
            raise Unsupported(f"MATCH.{i.sym('sz')}")
        self.uses_subgroup = True
        a, n = self.src_u(i, "a"), i.v("Rd")
        if i.sym("sz") == "U64":
            a = f"uvec2({a}, {self.src_u_hi(i, 'a')})"
        if i.sym("op") == "ALL":
            out = [f"{'uvec2' if 'uvec2' in a else 'uint'} mv = {a}; uint wmask = warp_ballot(true);",
                   "bool meq = warp_all(mv == warp_shuffle(mv, uint(findLSB(wmask))));",
                   "uint mm = meq ? wmask : 0u;"]
            if i.v("Pu", PT) != PT:
                out.append(f"p{i.v('Pu')} = meq;")
        else:
            out = [
                f"{'uvec2' if 'uvec2' in a else 'uint'} mv = {a}; uint mm = 0u;",
                "for (uint k = 0u; k < 32u; k++) if (warp_shuffle(mv, k) == mv) mm |= 1u << k;",
                "mm &= warp_ballot(true);",
            ]
        if n != RZ:
            self.regs.add(n)
            out.append(f"r{n} = mm;")
        return "{ " + " ".join(out) + " }"

    def vote(self, i: Insn) -> str:
        """VOTE(U).ALL/ANY/EQ: the ballot of Pp to Rd (URd), the vote to Pu (UPu)."""
        self.uses_subgroup = True
        p = pred(i.v("Pp"), i.v("Pp@not"))
        # an exited lane (Builder.zombie) is not active on the console: out of the ballot and the votes. EQ keeps it
        # in (none of MKW's programs with BAR votes EQ after an EXIT)
        eq_p, all_p = p, f"({p}) || exited" if self.zombie else p
        if self.zombie:
            p = f"({p}) && !exited"
        v = {"ALL": f"warp_all({all_p})", "ANY": f"warp_any({p})", "EQ": f"warp_equal({eq_p})"}.get(
            i.sym("voteop")
        )
        if v is None:
            raise Unsupported(f"{i.op}.{i.sym('voteop')}")
        out = []
        u = "U" if i.op == "VOTEU" else ""
        d = i.v(f"{u}Rd")
        if d != (URZ if u else RZ):
            (self.uregs if u else self.regs).add(d)
            out.append(f"{'ur' if u else 'r'}{d} = warp_ballot({p});")
        pu = i.v(f"{u}Pu")
        if pu != PT:
            out.append(f"{'up' if u else 'p'}{pu} = {v};")
        return " ".join(out)

    TASK_WORDS: ClassVar[int] = 1024  # payload words (4 KiB; RADV allows 16 KiB): 32 records of 10 words fit

    def task_isbe(self, i: Insn) -> str:
        """Task programs (the NV_mesh_shader model on Ampere, eden-nx2 rows 89 and 97): ISBEWR.O.ATTR [0x4] is the
        number of mesh workgroups, ISBEWR.O.ATTR [Ra + 0x10 + n] the payload the mesh workgroups read at the same
        offsets; ISBERD.ATTR [RZ] reads word 0 of the input, the workgroup index (the task multiplies it by 32 and adds
        the lane). Measured on MKW's 7 task programs (31/10/2026)."""
        if i.sym("base") != "ATTR" or i.sym("sz") not in ("32", ""):
            raise Unsupported(f"task {i.op}.{i.sym('base')}.{i.sym('sz')}")
        ra, off = i.v("Ra"), i.v("Ra_offset")
        if i.op == "ISBERD":
            if ra != RZ or off != 0 or i.sym("io") == "O":
                raise Unsupported(f"task ISBERD [{'R' if ra != RZ else ''}{off:#x}]")
            return self.dst(i, "WG_INDEX")
        b = i.v("Rb")
        if b != RZ:
            self.regs.add(b)
        if ra == RZ and off == 4:
            return f"task_count = {reg(b)};"
        if ra == RZ and off < 0x10:
            raise Unsupported(f"task ISBEWR [{off:#x}]")
        addr = f"({self.src_u(i, 'a')} + {off:#x}u)" if ra != RZ else f"{off:#x}u"
        return f"tp[min(({addr} - 0x10u) >> 2, {self.TASK_WORDS - 1}u)] = {reg(b)};"

    def mesh_isbe(self, i: Insn) -> str:
        """Mesh programs (NV_mesh_shader on Ampere), measured on 02d9d23c (31/10/2026): ISBEWR.O.ATTR.SKEW [a] writes a
        per-vertex output word at a = (VS attribute address - 0x70) * 32 + vertex * 4 (position 0x0-0x180, generic 0
        from 0x200; 32 vertices); ISBEWR.O [4 + 3p + k] is index k of triangle p (a byte); ISBEWR.O.32 [0x3] the
        primitive count. ISBERD.ATTR [RZ] reads the workgroup index, ISBERD.ATTR [Ra + 0x10 + n] the task payload.
        Everything goes to shared memory and is emitted after the program (EXT wants SetMeshOutputsEXT first)."""
        ra, off, b = i.v("Ra"), i.v("Ra_offset"), i.v("Rb") if i.has("Rb") else RZ
        addr = f"({self.src_u(i, 'a')} + {off:#x}u)" if ra != RZ else f"{off:#x}u"
        if i.op == "ISBERD":
            if i.sym("base") != "ATTR" or i.sym("io") == "O":
                raise Unsupported(f"mesh ISBERD.{i.sym('base')}.{i.sym('io')}")
            if ra == RZ and off == 0:
                return self.dst(i, "WG_INDEX")
            return self.dst(i, f"tp[min(({addr} - 0x10u) >> 2, {self.TASK_WORDS - 1}u)]")
        if b != RZ:
            self.regs.add(b)
        if i.sym("base") == "ATTR" and i.sym("skew") == "SKEW":
            return f"{{ uint w = {addr} >> 2; if (w < MV_WORDS) mv[w] = {reg(b)}; }}"
        if i.sym("base") == "ATTR":
            raise Unsupported("mesh ISBEWR.ATTR without SKEW")
        if ra == RZ and off == 3 and i.sym("sz") == "32":
            return f"mprims = {reg(b)};"
        if i.sym("sz") not in ("", "8", "U8"):
            raise Unsupported(f"mesh ISBEWR.{i.sym('sz')} [{off:#x}]")
        return f"{{ uint ib = {addr} - 4u; if (ib < MESH_PRIMS * 3u) mib[ib] = {reg(b)} & 0xffu; }}"

    def exit(self, i: Insn) -> str:
        if self.stage != "ps":
            # a predicated EXIT of a program with BAR (Builder.zombie): the lane goes on as exited; the unconditional
            # one at the end still returns, as every lane reaching it does
            predicated = i.v("Pg", PT) != PT or i.v("Pg@not", 0)
            return "exited = true;" if self.zombie and predicated else "return;"
        # Each target with any component takes four registers, component c in base + c, written or not (Ryujinx
        # EmitterContext.PrepareForReturn, MIT; eden-nx2 exit_program.cpp): packing the written ones read the wrong
        # register in 117 FS with a partial mask (4617398a, omap 0b1001: w in R3, was R1).
        out, r = [], 0
        for rt in range(8):
            mask = (self.sph.omap_targets >> (rt * 4)) & 0xF
            if not mask:
                continue
            for comp in range(4):
                if mask >> comp & 1:
                    self.regs.add(r + comp)
                    out.append(f"out_color{rt}[{comp}] = uintBitsToFloat(r{r + comp});")
                    self.out_attrs.add(rt)
            r += 4
        # Depth two registers after the last colour target (the one between is the sample mask), as eden-nx2
        # exit_program.cpp; Ryujinx GetDepthRegister counts components, the same with whole targets. Without it a
        # depth-only FS had no effect and the driver dropped it: the race's half-resolution depth stayed at 1.0 and its
        # lighting at 0 (run-race29, 31/10/2026).
        if self.sph.omap_depth:
            self.regs.add(r + 1)
            out.append(f"gl_FragDepth = uintBitsToFloat(r{r + 1});")
        return " ".join(out) + " return;"
