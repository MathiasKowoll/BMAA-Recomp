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
if anchor not in src: sys.exit("interp_patch.py: the place to add the unpack instructions was not found in x86.c")
open(sys.argv[2], "w").write(src.replace(anchor, add + anchor, 1))
