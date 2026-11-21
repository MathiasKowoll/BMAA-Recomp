#!/usr/bin/env python3
"""Makes the copy of DecompRecomp's interpreter that BMAA_INTERP uses (aot.c): x86.c with the instructions it lacks
and the game's code needs. DecompRecomp itself is not changed. usage: interp_patch.py x86.c out.c"""
import sys
src = open(sys.argv[1]).read()
anchor = "        case 0xC6: { /* shufps / shufpd */"
add = """        case 0x14: case 0x15: { /* unpcklps / unpckhps, unpcklpd / unpckhpd (BMAA-Recomp) */
            modrm m = decode_modrm(c);
            uint32_t b2[4];
            xmm_read(c, &m, b2, 16);
            uint32_t *d = (uint32_t *)cpu->xmm[m.reg].b;
            int hi = op2 == 0x15;
            uint32_t out[4];
            if (c->pfx66) { /* two doubles: one half of each */
                out[0] = d[hi ? 2 : 0]; out[1] = d[hi ? 3 : 1]; out[2] = b2[hi ? 2 : 0]; out[3] = b2[hi ? 3 : 1];
            } else {
                out[0] = d[hi ? 2 : 0]; out[1] = b2[hi ? 2 : 0]; out[2] = d[hi ? 3 : 1]; out[3] = b2[hi ? 3 : 1];
            }
            memcpy(d, out, 16);
            return true;
        }
        case 0x7C: case 0x7D: { /* haddpd / hsubpd (66), haddps / hsubps (F2) (BMAA-Recomp) */
            modrm m = decode_modrm(c);
            uint8_t b2[16]; xmm_read(c, &m, b2, 16);
            int sub = op2 == 0x7D;
            if (c->pfx66) {
                double a[2], b[2]; memcpy(a, cpu->xmm[m.reg].b, 16); memcpy(b, b2, 16);
                double o[2] = { sub ? a[0] - a[1] : a[0] + a[1], sub ? b[0] - b[1] : b[0] + b[1] };
                memcpy(cpu->xmm[m.reg].b, o, 16);
            } else {
                float a[4], b[4]; memcpy(a, cpu->xmm[m.reg].b, 16); memcpy(b, b2, 16);
                float o[4] = { sub ? a[0] - a[1] : a[0] + a[1], sub ? a[2] - a[3] : a[2] + a[3],
                               sub ? b[0] - b[1] : b[0] + b[1], sub ? b[2] - b[3] : b[2] + b[3] };
                memcpy(cpu->xmm[m.reg].b, o, 16);
            }
            return true;
        }
        case 0x50: { /* movmskps / movmskpd r32, xmm (BMAA-Recomp) */
            modrm m = decode_modrm(c);
            uint32_t b2[4]; xmm_read(c, &m, b2, 16);
            cpu->regs[m.reg] = c->pfx66 ? ((b2[1] >> 31) | (b2[3] >> 31) << 1)
                                        : ((b2[0] >> 31) | (b2[1] >> 31) << 1 | (b2[2] >> 31) << 2 | (b2[3] >> 31) << 3);
            return true;
        }
        case 0x38: { /* 0F 38 00: pshufb, 0F 38 02: phaddd (BMAA-Recomp); nothing else of 0F 38 */
            uint8_t op3 = dr_r8(cpu->g, c->pc);
            if (op3 != 0x00 && op3 != 0x02) return false;
            c->pc++;
            modrm m = decode_modrm(c);
            uint8_t b2[16], a[16], o[16]; xmm_read(c, &m, b2, 16); memcpy(a, cpu->xmm[m.reg].b, 16);
            if (op3 == 0x00) { for (int k = 0; k < 16; k++) o[k] = (b2[k] & 0x80) ? 0 : a[b2[k] & 15]; }
            else {
                int32_t x[4], y[4]; memcpy(x, a, 16); memcpy(y, b2, 16);
                uint32_t r[4] = { (uint32_t)x[0] + (uint32_t)x[1], (uint32_t)x[2] + (uint32_t)x[3], (uint32_t)y[0] + (uint32_t)y[1], (uint32_t)y[2] + (uint32_t)y[3] };
                memcpy(o, r, 16);
            }
            memcpy(cpu->xmm[m.reg].b, o, 16);
            return true;
        }
        case 0x3A: { /* 0F 3A 0F: palignr xmm, xmm/m128, imm8 (BMAA-Recomp); nothing else of 0F 3A */
            if (dr_r8(cpu->g, c->pc) != 0x0F) return false;
            c->pc++;
            modrm m = decode_modrm(c);
            uint8_t cat[32]; xmm_read(c, &m, cat, 16);             /* low half: the source */
            memcpy(cat + 16, cpu->xmm[m.reg].b, 16);               /* high half: the destination */
            uint8_t n = fetch8(c), o[16];
            for (int k = 0; k < 16; k++) o[k] = (n + k < 32) ? cat[n + k] : 0;
            memcpy(cpu->xmm[m.reg].b, o, 16);
            return true;
        }
        case 0x55: { /* andnps / andnpd: (not destination) and source (BMAA-Recomp) */
            modrm m = decode_modrm(c);
            uint64_t b2[2]; xmm_read(c, &m, b2, 16);
            for (int k = 0; k < 2; k++) cpu->xmm[m.reg].u64[k] = ~cpu->xmm[m.reg].u64[k] & b2[k];
            return true;
        }
        case 0xC2: { /* cmpps / cmpss / cmppd / cmpsd with its predicate (BMAA-Recomp) */
            modrm m = decode_modrm(c);
            int dbl = c->pfx66 || c->pfxf2, one = c->pfxf3 || c->pfxf2;
            uint8_t b2[16]; memset(b2, 0, 16);
            xmm_read(c, &m, b2, one ? (dbl ? 8 : 4) : 16);
            uint8_t imm = fetch8(c) & 7;
            int n = one ? 1 : (dbl ? 2 : 4);
            for (int k = 0; k < n; k++) {
                double a, b;
                if (dbl) { a = cpu->xmm[m.reg].f64[k]; memcpy(&b, b2 + 8 * k, 8); }
                else { float fa, fb; memcpy(&fa, cpu->xmm[m.reg].b + 4 * k, 4); memcpy(&fb, b2 + 4 * k, 4); a = fa; b = fb; }
                int un = a != a || b != b, r;
                switch (imm) {
                    case 0: r = a == b; break;        case 1: r = a < b; break;
                    case 2: r = a <= b; break;        case 3: r = un; break;
                    case 4: r = !(a == b); break;     case 5: r = !(a < b); break;
                    case 6: r = !(a <= b); break;     default: r = !un; break;
                }
                if (dbl) cpu->xmm[m.reg].u64[k] = r ? ~0ull : 0;
                else { uint32_t v = r ? ~0u : 0; memcpy(cpu->xmm[m.reg].b + 4 * k, &v, 4); }
            }
            return true;
        }
"""
# packed forms the interpreter's own cases read as scalar or as four floats: answered before its switch
anchor2 = "    const bool sd = c->pfxf2; /* escalar double */\n"
add2 = """    if (!ss && !sd && op2 == 0x5A) { /* cvtps2pd (two floats, eight bytes) / cvtpd2ps (66) (BMAA-Recomp) */
        modrm m = decode_modrm(c);
        if (c->pfx66) { double in[2]; xmm_read(c, &m, in, 16); float o[4] = { (float)in[0], (float)in[1], 0, 0 }; memcpy(cpu->xmm[m.reg].b, o, 16); }
        else { float in[2]; xmm_read(c, &m, in, 8); double o[2] = { in[0], in[1] }; memcpy(cpu->xmm[m.reg].b, o, 16); }
        return true;
    }
    if (c->pfx66 && !ss && !sd && (op2 == 0x51 || op2 == 0x58 || op2 == 0x59 || op2 == 0x5C || op2 == 0x5E || op2 == 0x5D || op2 == 0x5F)) {
        /* sqrtpd / addpd / mulpd / subpd / divpd / minpd / maxpd: two doubles (BMAA-Recomp) */
        modrm m = decode_modrm(c);
        double b[2], a[2]; xmm_read(c, &m, b, 16); memcpy(a, cpu->xmm[m.reg].b, 16);
        for (int k = 0; k < 2; k++) {
            switch (op2) {
                case 0x51: a[k] = __builtin_sqrt(b[k]); break;  case 0x58: a[k] = a[k] + b[k]; break;
                case 0x59: a[k] = a[k] * b[k]; break;           case 0x5C: a[k] = a[k] - b[k]; break;
                case 0x5E: a[k] = a[k] / b[k]; break;           case 0x5D: a[k] = a[k] < b[k] ? a[k] : b[k]; break;
                default:   a[k] = a[k] > b[k] ? a[k] : b[k]; break;
            }
        }
        memcpy(cpu->xmm[m.reg].b, a, 16);
        return true;
    }
"""
if anchor2 not in src: sys.exit("interp_patch.py: the start of sse_op was not found in x86.c")
src = src.replace(anchor2, anchor2 + add2, 1)
if anchor not in src: sys.exit("interp_patch.py: the place to add the unpack instructions was not found in x86.c")
open(sys.argv[2], "w").write(src.replace(anchor, add + anchor, 1))
