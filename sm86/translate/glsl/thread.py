"""SM86 -> GLSL, glsl.core.Builder methods for threads and subgroups, system values, task/mesh outputs and EXIT. A mixin: it
uses the Builder's operand helpers (src_u, dst...) and state."""

from typing import ClassVar

from program import Insn

from glsl.common import PT, RZ, URZ, Unsupported, multi_load, pred, reg


class ThreadOps:
    def fswzadd(self, i: Insn) -> str:
        """FSWZADD Rd, Ra, Rc, mask: per quad lane, Ra * {1,-1,1,0}[k] + Rc * {1,1,-1,1}[k] with k the lane's 2 bits
        of the mask (Ryujinx SwizzleAdd.glsl, MIT); used for derivatives across a quad."""
        self.uses_subgroup = True
        k = f"(({i.v('npCtrl')}u >> ((gl_SubgroupInvocationID & 3u) * 2u)) & 3u)"
        a, c = self.src_f(i, "a"), self.src_f(i, "c")
        return self.dst_f(i, f"({a} * vec4(1.0, -1.0, 1.0, 0.0)[{k}] + {c} * vec4(1.0, 1.0, -1.0, 1.0)[{k}])")

    REDUX: ClassVar[dict[str, str]] = {"SUM": "subgroupAdd", "MIN": "subgroupMin", "MAX": "subgroupMax",
                                       "AND": "subgroupAnd", "OR": "subgroupOr", "XOR": "subgroupXor"}  # fmt: skip

    def redux(self, i: Insn) -> str:
        """REDUX: a reduction over the active lanes into a uniform register (unsigned unless .S32).
        On Wave64 devices (gl_SubgroupSize > 32u), reduces strictly across the active 32-lane virtual warp."""
        op = i.sym("op")
        if op not in self.REDUX:
            raise Unsupported(f"REDUX.{op}")
        self.uses_subgroup = self.uses_arith = True
        a = self.src_u(i, "a")
        s32 = i.sym("sz") == "S32"
        hw = f"uint({self.REDUX[op]}(int({a})))" if s32 else f"{self.REDUX[op]}({a})"
        ident = {
            "SUM": "0u",
            "AND": "0xffffffffu",
            "OR": "0u",
            "XOR": "0u",
            "MIN": "0x7fffffffu" if s32 else "0xffffffffu",
            "MAX": "0x80000000u" if s32 else "0u",
        }[op]
        step = {
            "SUM": "acc += v;",
            "AND": "acc &= v;",
            "OR": "acc |= v;",
            "XOR": "acc ^= v;",
            "MIN": "acc = uint(min(int(acc), int(v)));" if s32 else "acc = min(acc, v);",
            "MAX": "acc = uint(max(int(acc), int(v)));" if s32 else "acc = max(acc, v);",
        }[op]
        n = i.v("URd") if i.has("URd") else i.v("Rd")
        is_u = i.has("URd")
        if n == (URZ if is_u else RZ):
            return ""
        (self.uregs if is_u else self.regs).add(n)
        dst = f"{'ur' if is_u else 'r'}{n}"
        return (
            f"{{ uint rv = {a}, acc = {ident}; "
            f"if (gl_SubgroupSize <= 32u) {{ acc = {hw}; }} else {{ "
            f"uint m = sg32_ballot(true).x; "
            f"for (uint k = 0u; k < 32u; k++) if (((m >> k) & 1u) != 0u) {{ uint v = sg32_shfl(rv, k); {step} }} "
            f"}} {dst} = acc; }}"
        )

    SR: ClassVar[dict[str, str]] = {
        "SR_TID.X": "gl_LocalInvocationID.x", "SR_TID.Y": "gl_LocalInvocationID.y", "SR_TID.Z": "gl_LocalInvocationID.z",
        "SR_CTAID.X": "gl_WorkGroupID.x", "SR_CTAID.Y": "gl_WorkGroupID.y", "SR_CTAID.Z": "gl_WorkGroupID.z",
        "SR_LANEID": "(gl_SubgroupInvocationID & 31u)",
        "SR_WARPID": "(gl_LocalInvocationIndex >> 5u)",
        "SR_NWARPID": "((gl_WorkGroupSize.x * gl_WorkGroupSize.y * gl_WorkGroupSize.z + 31u) >> 5u)",
        "SR_LTMASK": "((1u << (gl_SubgroupInvocationID & 31u)) - 1u)",
        "SR_LEMASK": "(((gl_SubgroupInvocationID & 31u) == 31u) ? 0xffffffffu : ((1u << ((gl_SubgroupInvocationID & 31u) + 1u)) - 1u))",
        "SR_GTMASK": "(((gl_SubgroupInvocationID & 31u) == 31u) ? 0u : (~((1u << ((gl_SubgroupInvocationID & 31u) + 1u)) - 1u)))",
        "SR_GEMASK": "(~((1u << (gl_SubgroupInvocationID & 31u)) - 1u))",
        "SR_EQMASK": "(1u << (gl_SubgroupInvocationID & 31u))",
        "SRZ": "0u", "SR255": "0u",
    }  # fmt: skip

    def s2r(self, i: Insn) -> str:
        names = i.f["SRa"].symbols or []
        sr = next((n for n in names if n in self.SR), None)
        if sr is None:
            raise Unsupported(f"{i.op} {'/'.join(names)}")
        val = self.SR[sr]
        if (self.task or self.mesh) and sr == "SR_LANEID":
            val = "virtual_lane_id"
        elif (self.task or self.mesh) and sr == "SR_WARPID":
            val = "virtual_warp_id"
        self.uses_subgroup |= "Subgroup" in val
        if i.op == "S2UR":
            n = i.v("URd")
            if n == URZ:
                return ""
            self.uregs.add(n)
            return f"ur{n} = {val};"
        return self.dst(i, val)

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
        when every active lane holds the same Ra, else 0 and Pu clear. Virtual 32-lane warp on Wave32 or Wave64."""
        if i.sym("sz") not in ("U32", "U64"):
            raise Unsupported(f"MATCH.{i.sym('sz')}")
        self.uses_subgroup = True
        a, n = self.src_u(i, "a"), i.v("Rd")
        if i.sym("sz") == "U64":
            a = f"uvec2({a}, {self.src_u_hi(i, 'a')})"
        vtype = "uvec2" if "uvec2" in a else "uint"
        if i.sym("op") == "ALL":
            out = [
                f"{vtype} mv = {a}; uint mact = sg32_ballot(true).x, mm = 0u;",
                "for (uint k = 0u; k < 32u; k++) if (((mact >> k) & 1u) != 0u && sg32_shfl(mv, k) == mv) mm |= 1u << k;",
                "bool meq = (mm == mact); mm = meq ? mact : 0u;",
            ]
            if i.v("Pu", PT) != PT:
                out.append(f"p{i.v('Pu')} = meq;")
        else:
            out = [
                f"{vtype} mv = {a}; uint mact = sg32_ballot(true).x, mm = 0u;",
                "for (uint k = 0u; k < 32u; k++) if (((mact >> k) & 1u) != 0u && sg32_shfl(mv, k) == mv) mm |= 1u << k;",
            ]
        if n != RZ:
            self.regs.add(n)
            out.append(f"r{n} = mm;")
        return "{ " + " ".join(out) + " }"

    def vote(self, i: Insn) -> str:
        """VOTE(U).ALL/ANY/EQ: the ballot of Pp to Rd (URd), the vote to Pu (UPu)."""
        self.uses_subgroup = True
        p = pred(i.v("Pp"), i.v("Pp@not"))
        v = {"ALL": f"sg32_all({p})", "ANY": f"sg32_any({p})", "EQ": f"sg32_all_equal({p})"}.get(
            i.sym("voteop")
        )
        if v is None:
            raise Unsupported(f"{i.op}.{i.sym('voteop')}")
        out = []
        u = "U" if i.op == "VOTEU" else ""
        d = i.v(f"{u}Rd")
        if d != (URZ if u else RZ):
            (self.uregs if u else self.regs).add(d)
            out.append(f"{'ur' if u else 'r'}{d} = sg32_ballot({p}).x;")
        pu = i.v(f"{u}Pu")
        if pu != PT:
            out.append(f"{'up' if u else 'p'}{pu} = {v};")
        return " ".join(out)

    def elect(self, i: Insn) -> str:
        """ELECT Rd, Pu: elect the first active lane in the 32-lane virtual warp."""
        self.uses_subgroup = True
        out = ["uint warp_half = gl_SubgroupInvocationID & 32u; uvec4 ballot = subgroupBallot(true);",
               "uint el = uint(findLSB((warp_half == 0u) ? ballot.x : ballot.y));",
               "bool eok = ((gl_SubgroupInvocationID & 31u) == el);"]
        d = i.v("Rd", RZ)
        if d != RZ:
            self.regs.add(d)
            out.append(f"r{d} = el;")
        pu = i.v("Pu", PT)
        if pu != PT:
            out.append(f"p{pu} = eok;")
        return "{ " + " ".join(out) + " }"

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

    def exit(self) -> str:
        if self.stage != "ps":
            return "return;"
        out, r = [], 0
        for rt in range(8):
            mask = (self.sph.omap_targets >> (rt * 4)) & 0xF
            for comp in range(4):
                if mask >> comp & 1:
                    self.regs.add(r)
                    out.append(f"out_color{rt}[{comp}] = uintBitsToFloat(r{r});")
                    self.out_attrs.add(rt)
                    r += 1
        # Depth two registers after the last colour component (the one between is the sample mask): Ryujinx
        # TranslatorContext.GetDepthRegister (MIT). Without it a depth-only FS had no effect and the driver dropped it:
        # the race's half-resolution depth stayed at 1.0 and its lighting at 0 (run-race29, 31/10/2026).
        if self.sph.omap_depth:
            self.regs.add(r + 1)
            out.append(f"gl_FragDepth = uintBitsToFloat(r{r + 1});")
        return " ".join(out) + " return;"
