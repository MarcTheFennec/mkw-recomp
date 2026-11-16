"""SM86 -> GLSL, glsl.core.Builder methods for integer, float, predicate and packing instructions. A mixin: it uses the
Builder's operand helpers (src_u, dst...) and state."""

from typing import ClassVar

from program import Insn

from glsl.common import BOP, ICMP, LOP3_FORMS, PT, RZ, URZ, Unsupported, fcmp, pred, reg


class AluOps:
    def src_cmp(self, i: Insn, slot: str) -> str:
        """A float compare's operand; .FTZ compares a denormal as zero. Ignored, FSETP.NEU.FTZ sum, 0 let a denormal
        through and 1/sum gave inf in MKW's deferred lighting (nxnative docs/reference/shaders.md, «.FTZ in compares»,
        InstEmitAluHelper.cs FlushDenormal @ 2a6f9b3, MIT); 9 177 of MKW's 11 561 programs carry it."""
        e = self.src_f(i, slot)
        return f"(abs({e}) < 1.17549435e-38 ? 0.0 : {e})" if i.sym("ftz") == "FTZ" else e

    def setp(self, i: Insn) -> str:
        u = i.op == "UISETP"
        pre = "U" if u else ""
        if i.op == "FSETP":
            c = fcmp(i.sym("fcomp"), self.src_cmp(i, "a"), self.src_cmp(i, "b"))
        else:
            cmp = ICMP.get(i.sym("icmp"))
            a, b = self.src_u(i, "a"), self.src_u(i, "b")
            if i.sym("fmt") == "S32":
                a, b = f"int({a})", f"int({b})"
            c = f"(({a}) {cmp} ({b}))" if cmp else None
            # .EX: the high words of a 64-bit compare; equal ones leave the answer to the low words' compare in Pr
            # (Mesa NAK OpISetP fold, MIT). Was read from a field "e" that does not exist and dropped (12 in 6 programs)
            if c and i.sym("ex") == "EX":
                c = f"(({a}) == ({b}) ? {pred(i.v(f'{pre}Pr'), i.v(f'{pre}Pr@not'), u)} : {c})"
        if not c:
            raise Unsupported(f"{i.cls}: compare {i.sym('fcomp') or i.sym('icmp')}")
        if i.has("X"):
            raise Unsupported(f"{i.cls}.X")
        bop = BOP[i.sym("bop")]
        acc = pred(i.v(f"{pre}Pp"), i.v(f"{pre}Pp@not"), u)
        out, pv = [], "up" if u else "p"
        if i.v(f"{pre}Pu") != PT:
            out.append(f"{pv}{i.v(f'{pre}Pu')} = {c} {bop} {acc};")
        if i.v(f"{pre}Pv") != PT:
            out.append(f"{pv}{i.v(f'{pre}Pv')} = (!{c}) {bop} {acc};")
        return " ".join(out)

    # ---- packing, conversion, bit operations
    def f2fp(self, i: Insn) -> str:
        """F2FP.PACK_AB: two floats to a half pair, a in the high half, b in the low (F16; BF16 by truncation)."""
        if i.sym("merge") != "PACK_AB" or i.sym("relu") == "RELU":
            raise Unsupported(f"F2FP.{i.sym('merge')}")
        a, b = self.src_f(i, "a"), self.src_f(i, "b")
        if i.sym("dstfmt") == "F16":
            return self.dst(i, f"packHalf2x16(vec2({b}, {a}))")
        if i.sym("dstfmt") == "BF16":
            return self.dst(i, f"((floatBitsToUint({a}) & 0xffff0000u) | (floatBitsToUint({b}) >> 16))")
        raise Unsupported(f"F2FP.{i.sym('dstfmt')}")

    def r2ur(self, i: Insn) -> str:
        if i.v("Pu", PT) != PT:
            raise Unsupported("R2UR with predicate")
        return self.dst(i, self.src_u(i, "a"))

    def i2f(self, i: Insn) -> str:
        src, dst = i.sym("srcfmt"), i.sym("dstfmt")
        if dst != "F32":
            raise Unsupported(f"I2F.{dst}")
        v = self.src_u(i, "b")
        widths = {"U8": 8, "S8": 8, "U16": 16, "S16": 16, "U32": 32, "S32": 32}
        if src not in widths:
            raise Unsupported(f"I2F.{src}")
        w = widths[src]
        sel = int((i.sym("bsel") or i.sym("hsel") or "B0")[1:]) if w < 32 else 0
        if w < 32:
            v = f"bitfieldExtract({'int(' + v + ')' if src[0] == 'S' else v}, {sel * w}, {w})"
        elif src[0] == "S":
            v = f"int({v})"
        return self.dst_f(i, f"float({v})")

    def preds_to_bits(self, uniform: bool) -> str:
        return " | ".join(f"({'up' if uniform else 'p'}{n} ? {1 << n}u : 0u)" for n in range(7))

    def p2r(self, i: Insn, uniform: bool = False) -> str:
        """P2R/UP2UR Rd, PR, Ra, mask: the predicate bits under mask (shifted to byte insert Bn) merged into Ra."""
        if i.sym("insert") not in ("B0", ""):
            raise Unsupported(f"{i.op}.{i.sym('insert')}")
        m = i.v("Sb") & 0xFF
        return self.dst(i, f"((({self.preds_to_bits(uniform)}) & {m}u) | ({self.src_u(i, 'a')} & {~m & 0xFFFFFFFF}u))")

    def r2p(self, i: Insn) -> str:
        """R2P PR, Ra.Bn, mask: P0..P6 under mask take bits 8n.. of Ra; the rest keep their value."""
        if i.cls != "r2p__RIR":
            raise Unsupported(f"{i.cls}: mask not an immediate")
        m, sel = i.v("Sb") & 0x7F, int((i.sym("a_bsel") or "B0")[1:])
        if not m:
            return ""
        out = [f"{{ uint pr = {self.src_u(i, 'a')} >> {8 * sel}u;"]
        out += [f"p{n} = (pr & {1 << n}u) != 0u;" for n in range(7) if m >> n & 1]
        return " ".join(out) + " }"

    def up2ur(self, i: Insn) -> str:
        return self.p2r(i, True)

    def flo(self, i: Insn) -> str:
        """FLO: bit index of the highest set bit (sign bit flipped first for S32), 0xffffffff if none; .SH gives
        31 - index."""
        if i.v("Pu", PT) != PT:
            raise Unsupported("FLO with predicate")
        v = self.src_u(i, "b")
        name = self.operand(i, "b")
        if i.v(f"{name}@invert", 0):
            v = f"(~{v})"
        e = f"uint(findMSB(int({v})))" if i.sym("fmt") == "S32" else f"uint(findMSB({v}))"
        if i.sym("sh") == "SH":
            e = f"({e} == 0xffffffffu ? 0xffffffffu : 31u - {e})"
        return self.dst(i, e)

    def sgxt(self, i: Insn) -> str:
        bits = self.src_u(i, "b")
        if i.sym("fmt") == "U32":
            return self.dst(i, f"bitfieldExtract({self.src_u(i, 'a')}, 0, int(min({bits}, 32u)))")
        return self.dst(i, f"uint(bitfieldExtract(int({self.src_u(i, 'a')}), 0, int(min({bits}, 32u))))")

    def prmt(self, i: Insn) -> str:
        if i.sym("pmode") not in ("IDX", ""):
            raise Unsupported(f"PRMT.{i.sym('pmode')}")
        self.uses_prmt = True
        return self.dst(i, f"prmt({self.src_u(i, 'a')}, {self.src_u(i, 'b')}, {self.src_u(i, 'c')})")

    def uflo(self, i: Insn) -> str:
        if i.v("UPu", PT) != PT:
            raise Unsupported("UFLO with predicate")
        return self.flo(i)

    def ulea(self, i: Insn) -> str:
        if i.v("UPu", PT) != PT:
            raise Unsupported("ULEA with carry")
        return self.lea(i)

    def usel(self, i: Insn) -> str:
        return self.dst(i, f"({pred(i.v('UPp'), i.v('UPp@not'), True)} ? {self.src_u(i, 'a')} : {self.src_u(i, 'b')})")

    def uplop3(self, i: Insn) -> str:
        return self.plop3(i, "U")

    def iabs(self, i: Insn) -> str:
        # x times its sign (1 or -1), not abs(): NVIDIA's compiler (610.74 and 621.24, RTX 2070 SUPER) faults the GPU
        # ("instruction pointer invalid", device lost) on an IABS of a small select (0/-1, -1/0/1) followed by IADD and XOR,
        # and it also reads x < 0 ? -x : x, max(x, -x) and (x ^ m) - m as that IABS; the product it leaves alone.
        # Issue #22: the title-screen particles (vs c5d3832d / 1c7a765b) took the GPU down on their first draw.
        tmp = f"ab{i.pc}"
        d = self.dst(i, f"({tmp} * (uint(int({tmp}) >> 31) | 1u))")
        return f"{{ uint {tmp} = {self.src_u(i, 'b')}; {d} }}" if d else ""

    def ubrev(self, i: Insn) -> str:
        return self.dst(i, f"bitfieldReverse({self.src_u(i, 'b')})")

    def popc(self, i: Insn) -> str:
        return self.dst(i, f"uint(bitCount({self.neg_u(i, 'b')}))")

    # ---- integer and conversion
    def neg_u(self, i: Insn, slot: str) -> str:
        e = self.src_u(i, slot)
        name = self.operand(i, slot)
        if i.v(f"{name}@invert", 0):
            return f"(~{e})"
        return f"(0u - {e})" if i.v(f"{name}@negate", 0) else e

    def iadd3(self, i: Insn) -> str:
        """IADD3 a + b + c in 64 bits: Pu/Pv are the carries out of bit 31 and 32; .X adds the carries in Pp, Pq
        (the high half of a 64-bit add). Negated operands are two's complement words (their carry is the word's)."""
        u = "U" if i.has("URd") else ""
        terms = [f"uint64_t({self.neg_u(i, s)})" for s in "abc"]
        if i.has("X") and i.sym("X") == "X":
            for k in ("Pp", "Pq"):
                terms.append(f"uint64_t({pred(i.v(f'{u}{k}', PT), i.v(f'{u}{k}@not', 0), bool(u))} ? 1u : 0u)")
        pu, pv = i.v(f"{u}Pu", PT), i.v(f"{u}Pv", PT)
        if pu == PT and pv == PT:  # no carry out: the 32-bit sum is the same modulo 2^32, without 64-bit adds
            plain = [f"({self.neg_u(i, s)})" for s in "abc"] + [t.replace("uint64_t(", "uint(", 1) for t in terms[3:]]
            return self.dst(i, f"({' + '.join(plain)})")
        out = [f"uint64_t s3 = {' + '.join(terms)};", self.dst(i, "uint(s3)")]
        pre = "up" if u else "p"
        if pu != PT:
            out.append(f"{pre}{pu} = ((s3 >> 32) & 1ul) != 0ul;")
        if pv != PT:
            out.append(f"{pre}{pv} = ((s3 >> 33) & 1ul) != 0ul;")
        return "{ " + " ".join(out) + " }"

    def uiadd3(self, i: Insn) -> str:
        return self.iadd3(i)

    def uimad(self, i: Insn) -> str:
        return self.imad(i)

    def imnmx(self, i: Insn) -> str:
        a, b = self.src_u(i, "a"), self.src_u(i, "b")
        if i.sym("fmt") == "S32":
            a, b = f"int({a})", f"int({b})"
        p = pred(i.v("Pp"), i.v("Pp@not"))
        return self.dst(i, f"uint({p} ? min({a}, {b}) : max({a}, {b}))")

    def src_u_hi(self, i: Insn, slot: str) -> str:
        """The high word of a 64-bit operand pair: the next register or the next constant word."""
        if i.has(f"R{slot}"):
            n = i.v(f"R{slot}")
            if n != RZ:
                self.regs.add(n + 1)
            return reg(n if n == RZ else n + 1)
        if i.has(f"UR{slot}"):
            n = i.v(f"UR{slot}")
            if n == URZ:
                return "0u"
            self.uregs.add(n + 1)
            return f"ur{n + 1}"
        for k, iv in i.f.items():
            if k.startswith("ConstBankAddress") and f"S{slot}_bank" in k:
                bank, word = (v for _, v in iv.slot_values)
                return self.const(bank, word * 4 + 4)
        raise Unsupported(f"{i.cls}: 64-bit operand {slot}")

    def imad(self, i: Insn) -> str:
        """IMAD/UIMAD a * b + c. .WIDE: the 64-bit product (signed unless .U32) plus the 64-bit pair c, into Rd and
        Rd+1; .HI: the product's high word plus c; .X: plus the carry in Pp (PTX mad.wide, mad.hi, madc). The carry
        out Pu is not modelled."""
        u = "U" if i.has("URd") else ""
        if i.v(f"{u}Pu", PT) != PT:
            raise Unsupported(f"{i.cls}: carry out")
        # .HI and .WIDE are separate classes (imad_hi__, uimad_wide__) with the form as a fixed label, not a field:
        # reading a field "wide" turned 428 of them into a * b + c (NX2navi idea)
        form = i.cls.split("__")[0].split("_")
        a, b = self.src_u(i, "a"), self.src_u(i, "b")
        mode = "HI" if "hi" in form else "WIDE" if "wide" in form else ""
        if mode not in ("WIDE", "HI"):
            e = f"({a} * {b} + {self.neg_u(i, 'c')})"
            if i.sym("X") == "X":
                e = f"({e} + ({pred(i.v(f'{u}Pp', PT), i.v(f'{u}Pp@not', 0), bool(u))} ? 1u : 0u))"
            return self.dst(i, e)
        if i.sym("X") == "X":
            raise Unsupported(f"{i.cls}: .X")
        prod = (
            f"uint64_t(int64_t(int({a})) * int64_t(int({b})))"
            if i.sym("fmt") == "S32"
            else f"(uint64_t({a}) * uint64_t({b}))"
        )
        if mode == "HI":
            return self.dst(i, f"(uint({prod} >> 32) + {self.neg_u(i, 'c')})")
        c = f"((uint64_t({self.src_u_hi(i, 'c')}) << 32) | uint64_t({self.src_u(i, 'c')}))"
        name = next((n for n in ("Rc", "URc", "Sc") if i.has(n)), "Sc")
        if i.v(f"{name}@negate", 0):
            c = f"(0ul - {c})"
        n = i.v(f"{u}Rd")
        if n == (URZ if u else RZ):
            return ""
        (self.uregs if u else self.regs).update((n, n + 1))
        r = "ur" if u else "r"
        return f"{{ uint64_t w = {prod} + {c}; {r}{n} = uint(w); {r}{n + 1} = uint(w >> 32); }}"

    def shf(self, i: Insn, u: str = "") -> str:
        """Funnel shift of the 64-bit {c:a} by b; .LO/.HI keep the low/high word; 32-bit formats clamp or wrap the
        amount at 32 (.C/.W), 64-bit at 64. Signed formats shift arithmetically. u="U": the uniform form (USHF)."""
        fmt, wide = i.sym("fmt"), i.sym("fmt") in ("U64", "S64")
        lim = 64 if wide else 32
        b = self.src_u(i, "b")
        amt = f"min({b}, {lim}u)" if i.sym("cw") == "C" else f"({b} & {lim - 1}u)"
        v = f"((uint64_t({self.src_u(i, 'c')}) << 32) | uint64_t({self.src_u(i, 'a')}))"
        sh = "<<" if i.sym("dir") == "L" else ">>"
        v = f"uint64_t(int64_t({v}) {sh} {amt})" if fmt.startswith("S") else f"({v} {sh} {amt})"
        e = f"uint({v} >> 32)" if i.sym("hilo") == "HI" else f"uint({v})"
        if u:
            n = i.v("URd")
            if n == URZ:
                return ""
            self.uregs.add(n)
            return f"ur{n} = {e};"
        return self.dst(i, e)

    def half2(self, i: Insn, slot: str, swz: str) -> str:
        """A packed f16x2 operand as a vec2 (x = H0, y = H1) after its swizzle, |x| and -x; F32 takes the 32-bit
        float in both lanes. A pair of F16 immediates is Sc (H0) and Sc1 (H1)."""
        if i.has(f"S{slot}1"):
            v = f"vec2(unpackHalf2x16({i.v(f'S{slot}') & 0xFFFF:#x}u).x, unpackHalf2x16({i.v(f'S{slot}1') & 0xFFFF:#x}u).x)"
        else:
            u = self.src_u(i, slot)
            v = {"H1_H0": f"unpackHalf2x16({u})", "H0_H0": f"unpackHalf2x16({u}).xx", "H1_H1": f"unpackHalf2x16({u}).yy",
                 "F32": f"vec2(uintBitsToFloat({u}))"}.get(swz)  # fmt: skip
            if v is None:
                raise Unsupported(f"{i.op}: swizzle {swz}")
        name = self.operand(i, slot)
        if i.v(f"{name}@absolute", 0):
            v = f"abs({v})"
        return f"(-{v})" if i.v(f"{name}@negate", 0) else v

    def hadd2(self, i: Insn) -> str:
        """HADD2: two f16 adds (Mesa NAK OpHAdd2); computed in f32 and rounded on packing. .F32 writes the low lane
        as a 32-bit float."""
        if "32i" in i.cls:
            raise Unsupported(i.cls)
        a = self.half2(i, "a", i.sym("iswzA"))
        c = self.half2(i, "c", i.sym("iswzB_as_C") or i.sym("iswzB"))
        e = f"({a} + {c})"
        if i.sym("sat") == "SAT":  # a NaN saturates to 0, as FADD.SAT (dst_f): GLSL's clamp leaves it undefined
            self.uses_nminmax = True
            e = f"nclamp({e}, vec2(0.0), vec2(1.0))"
        return self.dst(i, f"floatBitsToUint(({e}).x)" if i.sym("ofmt") == "F32" else f"packHalf2x16({e})")

    def hmnmx2(self, i: Insn) -> str:
        """HMNMX2: per f16 lane the minimum when Pp holds, else the maximum, with FMNMX's NaN rule (nmin/nmax)."""
        if i.sym("ofmt") != "F16_V2" or i.sym("nan") == "NAN" or i.sym("xorsign") == "XORSIGN":
            raise Unsupported(f"HMNMX2.{i.sym('ofmt')}.{i.sym('nan')}.{i.sym('xorsign')}")
        self.uses_nminmax = True
        p = pred(i.v("Pp"), i.v("Pp@not"))
        a, b = self.half2(i, "a", i.sym("iswzA")), self.half2(i, "b", i.sym("iswzB"))
        e = f"{{ vec2 ma = {a}, mb = {b}; "
        e += "vec2 mr = " + f"({p}) ? vec2(nmin(ma.x, mb.x), nmin(ma.y, mb.y)) : vec2(nmax(ma.x, mb.x), nmax(ma.y, mb.y)); "
        return e + self.dst(i, "packHalf2x16(mr)") + " }"

    def hsetp2(self, i: Insn) -> str:
        """HSETP2: an f16 compare per lane (Mesa NAK OpHSetP2, Ryujinx HsetP2): Pu = lane 0 and Pv = lane 1, each
        combined with Pp; .H_AND puts both lanes together in Pu and its negation in Pv. In MKW's 17 programs (31/10/2026)
        it compares the two halves of one register (H0_H0 against H1_H1)."""
        if fcmp(i.sym("cmp"), "", "") is None:
            raise Unsupported(f"HSETP2.{i.sym('cmp')}")
        a = self.half2(i, "a", i.sym("iswzA"))
        c = self.half2(i, "c", i.sym("iswzB_as_C") or i.sym("iswzB"))
        bop, acc = BOP[i.sym("bop")], pred(i.v("Pp"), i.v("Pp@not"))
        out = [f"vec2 ha = {a}, hc = {c};"]
        if i.sym("ftz") == "FTZ":   # f16 denormals compare as zero (src_cmp)
            out.append("ha = mix(ha, vec2(0.0), lessThan(abs(ha), vec2(6.103515625e-05))); "
                       "hc = mix(hc, vec2(0.0), lessThan(abs(hc), vec2(6.103515625e-05)));")
        out.append(f"bool h0 = {fcmp(i.sym('cmp'), 'ha.x', 'hc.x')}, h1 = {fcmp(i.sym('cmp'), 'ha.y', 'hc.y')};")
        if i.sym("h_and") == "H_AND":
            out.append("h0 = h0 && h1; h1 = !h0;")
        if i.v("Pu") != PT:
            out.append(f"p{i.v('Pu')} = h0 {bop} {acc};")
        if i.v("Pv") != PT:
            out.append(f"p{i.v('Pv')} = h1 {bop} {acc};")
        return "{ " + " ".join(out) + " }"

    def plop3(self, i: Insn, u: str = "") -> str:
        """PLOP3: Pu and Pv are 8-bit truth tables (uimm8, vimm8) over (Pp, Pq, Pr), bit (p << 2 | q << 1 | r).
        u="U": UPLOP3 on uniform predicates; the uniform form of PLOP3 takes its third input from UPr."""
        ins = [pred(i.v(f"{u}{k}"), i.v(f"{u}{k}@not"), bool(u)) for k in ("Pp", "Pq")]
        r = "UPr" if i.has("UPr") else "Pr"
        ins.append(pred(i.v(r), i.v(f"{r}@not"), r == "UPr"))
        out = []
        for dst, lut in ((f"{u}Pu", "uimm8"), (f"{u}Pv", "vimm8")):
            d = i.v(dst)
            if d == PT:
                continue
            terms = []
            for bit in range(8):
                if i.v(lut) >> bit & 1:
                    lits = [ins[k] if bit >> (2 - k) & 1 else f"!({ins[k]})" for k in range(3)]
                    terms.append("(" + " && ".join(lits) + ")")
            out.append(f"{'up' if u else 'p'}{d} = {' || '.join(terms) or 'false'};")
        return " ".join(out)

    def lea(self, i: Insn) -> str:
        """LEA.LO: (a << s) + b. LEA.HI: high word of ({c:a} << s) + b; .SX32 takes c as a's sign."""
        s = i.v("scaleU5")
        if i.v("Pu", PT) != PT:
            if i.sym("hilo") == "HI":
                raise Unsupported(f"{i.cls} HI with carry out")
            a, b = self.neg_u(i, "a"), self.neg_u(i, "b")
            pu = i.v("Pu")
            return (f"{{ uint64_t s3 = uint64_t({a} << {s}u) + uint64_t({b}); {self.dst(i, 'uint(s3)')} "
                    f"p{pu} = (s3 >> 32) != 0ul; }}")  # fmt: skip
        a, b = self.neg_u(i, "a"), self.neg_u(i, "b")
        if i.sym("hilo") == "HI":
            c = f"uint(int({a}) >> 31)" if i.has("sx32") and i.sym("sx32") == "SX32" else self.src_u(i, "c")
            hi = f"uint(((uint64_t({c}) << 32) | uint64_t({a})) << {s}u >> 32)"
            return self.dst(i, f"({hi} + {b})")
        return self.dst(i, f"(({a} << {s}u) + {b})")

    def lop3(self, i: Insn, u: str = "") -> str:
        a, b, c = (self.src_u(i, s) for s in "abc")
        form = LOP3_FORMS.get(i.v("imm8"))
        if form is None:
            self.uses_lop3 = True
            e = f"lop3({a}, {b}, {c}, {i.v('imm8')}u)"
        else:  # the table as an expression: no helper call per instruction (9 of 10 are a & b, a ^ b, a | b...)
            e = form.format(a=f"({a})", b=f"({b})", c=f"({c})")
        pu = i.v(f"{u}Pu", PT)
        if pu != PT:
            # Pu = (result != 0) pop Pp: Ryujinx's NZ predicate (InstEmitIntegerLogical.cs, MIT) combined with SM70's
            # source predicate (PAND/POR/PXOR); the usual form is POR with !PT, i.e. result != 0 (hypothesis).
            op = {"PAND": "&&", "POR": "||", "PXOR": "!="}.get(i.sym("pop"))
            if op is None:
                raise Unsupported(f"{i.cls}: pop {i.sym('pop')}")
            sp = pred(i.v(f"{u}Pp"), i.v(f"{u}Pp@not"))
            tmp = f"lr{i.pc}"
            res = f"uint {tmp} = {e}; p{pu} = ({tmp} != 0u) {op} {sp};"
            if u:
                n = i.v("URd")
                if n != URZ:
                    self.uregs.add(n)
                    res += f" ur{n} = {tmp};"
                return f"{{ {res} }}"
            n = i.v("Rd")
            if n != RZ:
                self.regs.add(n)
                res += f" r{n} = {tmp};"
            return f"{{ {res} }}"
        if u:
            n = i.v("URd")
            if n == URZ:
                return ""
            self.uregs.add(n)
            return f"ur{n} = {e};"
        return self.dst(i, e)

    def ulop3(self, i: Insn) -> str:
        return self.lop3(i, "U")

    def sel(self, i: Insn) -> str:
        return self.dst(i, f"({pred(i.v('Pp'), i.v('Pp@not'))} ? {self.src_u(i, 'a')} : {self.src_u(i, 'b')})")

    def cs2r(self, i: Insn) -> str:
        if i.sym("SRa") not in ("SRZ", "SR255"):
            raise Unsupported(f"CS2R {i.sym('SRa')}")
        n, k = i.v("Rd"), 2 if i.sym("sz") in ("64", "_64") else 1
        if n == RZ:
            return ""
        self.regs.update(range(n, n + k))
        return " ".join(f"r{n + j} = 0u;" for j in range(k))

    ROUND: ClassVar[dict[str, str]] = {"ROUND": "roundEven", "FLOOR": "floor", "CEIL": "ceil", "TRUNC": "trunc", "RN": "roundEven",
             "RM": "floor", "RP": "ceil", "RZ": "trunc"}  # fmt: skip

    def f2i(self, i: Insn) -> str:
        x = f"{self.ROUND.get(i.sym('rnd'), 'trunc')}({self.src_f(i, 'b')})"
        dst = i.sym("dstfmt")
        lims = {"S32": ("int", -2147483648.0, 2147483647.0), "U32": ("uint", 0.0, 4294967295.0),
                "S16": ("int", -32768.0, 32767.0), "U16": ("uint", 0.0, 65535.0),
                "S8": ("int", -128.0, 127.0), "U8": ("uint", 0.0, 255.0)}  # fmt: skip
        if dst not in lims:
            raise Unsupported(f"F2I.{dst}")
        t, lo, hi = lims[dst]
        return self.dst(i, f"uint({t}(clamp({x}, {lo}, {hi})))")

    def i2fp(self, i: Insn) -> str:
        src = i.sym("srcfmt")
        if src not in ("S32", "U32") or i.sym("dstfmt") != "F32":
            raise Unsupported(f"I2FP.{i.sym('dstfmt')}.{src}")
        v = self.src_u(i, "b")
        return self.dst_f(i, f"float(int({v}))" if src == "S32" else f"float({v})")

    def frnd(self, i: Insn) -> str:
        if i.sym("fmt") != "F32":
            raise Unsupported(f"FRND.{i.sym('fmt')}")
        return self.dst_f(i, f"{self.ROUND.get(i.sym('rnd'), 'roundEven')}({self.src_f(i, 'b')})")

    def fset(self, i: Insn) -> str:
        cmp = fcmp(i.sym("fcomp"), self.src_cmp(i, "a"), self.src_cmp(i, "b"))
        if not cmp:
            raise Unsupported(f"FSET.{i.sym('fcomp')}")
        c = f"({cmp} {BOP[i.sym('bop')]} {pred(i.v('Pp'), i.v('Pp@not'))})"
        # SM86's FSET encoding is always .BF (nvdisasm prints FSET.BF for all 1 302 in the census): 1.0f, not the all-ones
        # mask of Maxwell's FSET without .BF, which read as a float is a NaN (the race's wave simulation, run-race35).
        return self.dst(i, f"({c} ? 0x3f800000u : 0u)")
