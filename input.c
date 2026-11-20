// The keyboard, answered by the Mac's own: the engine builds its table of keys at start by asking what each key code
// types (UCKeyTranslate), and with no answer the table is empty and two different keys look the same to it.
// The layout the game hands over is a pointer it got from a CFData of the host's, which is not memory of the game's,
// so the current layout is asked for here instead of read through that pointer.
#include "rt.h"
#include <Carbon/Carbon.h>
#include <stdlib.h>

#define RET(v) (c->eax = (uint32_t)(v))

void I__LMGetKbdType(CPU *c) { RET(LMGetKbdType()); }

// (layout, UInt16 key code, UInt16 action, UInt32 modifiers, UInt32 keyboard type, options, UInt32 *dead key state,
//  max length, length out, UniChar out[])
void I__UCKeyTranslate(CPU *c) {
    static CFDataRef data;
    static TISInputSourceRef src;
    if (!src) {
        src = TISCopyCurrentKeyboardLayoutInputSource();
        data = src ? TISGetInputSourceProperty(src, kTISPropertyUnicodeKeyLayoutData) : NULL;
    }
    if (!data) { RET(paramErr); return; }
    UInt32 dead = ARG(6) ? rd32(ARG(6)) : 0;
    UniCharCount n = 0, max = ARG(7) > 64 ? 64 : ARG(7);
    UniChar out[64];
    OSStatus e = UCKeyTranslate((const UCKeyboardLayout *)CFDataGetBytePtr(data), (UInt16)ARG(1), (UInt16)ARG(2), ARG(3), ARG(4),
                                ARG(5), &dead, max, &n, out);
    if (ARG(6)) wr32(ARG(6), dead);
    if (ARG(8)) wr32(ARG(8), (uint32_t)n);
    if (ARG(9)) for (UniCharCount i = 0; i < n; i++) wr16(ARG(9) + 2 * (uint32_t)i, out[i]);
    RET(e);
}

// ---- which process is in front: the engine sleeps instead of drawing while it is not the one ----
// A ProcessSerialNumber is two 32-bit numbers in both worlds, so the Mac's own answers go straight through.
// BMAA_ALWAYS_FRONT=1 says "this one" whatever is in front, for runs nobody is sitting at.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static int always_front(void) { static int v = -1; if (v < 0) v = getenv("BMAA_ALWAYS_FRONT") != NULL; return v; }
void I__GetCurrentProcess(CPU *c) { RET(ARG(0) ? GetCurrentProcess((ProcessSerialNumber *)G2H(ARG(0))) : paramErr); }
void I__GetFrontProcess(CPU *c) {
    if (!ARG(0)) { RET(paramErr); return; }
    RET(always_front() ? GetCurrentProcess((ProcessSerialNumber *)G2H(ARG(0))) : GetFrontProcess((ProcessSerialNumber *)G2H(ARG(0))));
}
void I__SameProcess(CPU *c) {
    Boolean same = 0;
    OSErr e = SameProcess((const ProcessSerialNumber *)G2H(ARG(0)), (const ProcessSerialNumber *)G2H(ARG(1)), &same);
    if (ARG(2)) wr8(ARG(2), same);
    RET(e);
}
// (coordinate space, object or NULL, HIPoint *out) -> out; the game's HIPoint is two floats, the Mac's two doubles
void I__HIGetMousePosition(CPU *c) {
    HIPoint p = {0, 0};
    HIGetMousePosition(ARG(0), NULL, &p);
    if (ARG(2)) { wrf(ARG(2), (float)p.x); wrf(ARG(2) + 4, (float)p.y); }
    RET(ARG(2));
}
