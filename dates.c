// Dates: five CoreFoundation and Carbon functions that the runtime's bridge leaves unanswered, and that the game
// calls early (a date it reads back from a structure nobody filled sends it through a wild pointer). They are
// arithmetic on values, so they are answered here with the host's own functions.
//
// i386: a double argument is two slots. CFGregorianDate is {int32 year; int8 month, day, hour, minute; double
// second}, 16 bytes with the double at offset 8; returned through a hidden pointer that is the first argument and
// that the callee takes off the stack.
#include "rt.h"
#include <CoreFoundation/CoreFoundation.h>

#define RET(v) (c->eax = (uint32_t)(v))
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static double argd(CPU *c, int slot) { uint64_t b = (uint64_t)ARG(slot) | (uint64_t)ARG(slot + 1) << 32; double d; memcpy(&d, &b, 8); return d; }
static CFTimeZoneRef zone(uint32_t guest_tz) {   // NULL is GMT; anything else, the system's zone
    static CFTimeZoneRef sys;
    if (!guest_tz) return NULL;
    if (!sys) sys = CFTimeZoneCopySystem();
    return sys;
}

void I__UCConvertCFAbsoluteTimeToLongDateTime(CPU *c) {   // (CFAbsoluteTime, LongDateTime *): seconds since 1904
    int64_t t = (int64_t)(argd(c, 0) + kCFAbsoluteTimeIntervalSince1904);
    if (ARG(2)) wr64(ARG(2), (uint64_t)t);
    RET(0);
}
void I__UCConvertLongDateTimeToCFAbsoluteTime(CPU *c) {   // (LongDateTime: two slots, CFAbsoluteTime *)
    int64_t t = (int64_t)((uint64_t)ARG(0) | (uint64_t)ARG(1) << 32);
    double d = (double)t - kCFAbsoluteTimeIntervalSince1904;
    if (ARG(2)) wrd(ARG(2), d);
    RET(0);
}
void I__CFAbsoluteTimeGetGregorianDate(CPU *c) {   // sret, (CFAbsoluteTime, CFTimeZoneRef)
    uint32_t out = ARG(0);
    CFGregorianDate g = CFAbsoluteTimeGetGregorianDate(argd(c, 1), zone(ARG(3)));
    wr32(out, (uint32_t)g.year); wr8(out + 4, (uint8_t)g.month); wr8(out + 5, (uint8_t)g.day);
    wr8(out + 6, (uint8_t)g.hour); wr8(out + 7, (uint8_t)g.minute); wrd(out + 8, g.second);
    RET(out);
    c->esp += 4;   // the hidden pointer is the callee's to pop
}
void I__CFGregorianDateGetAbsoluteTime(CPU *c) {   // (CFGregorianDate by value: four slots, CFTimeZoneRef) -> double
    CFGregorianDate g;
    g.year = (SInt32)ARG(0); g.month = (SInt8)(ARG(1) & 0xFF); g.day = (SInt8)(ARG(1) >> 8 & 0xFF);
    g.hour = (SInt8)(ARG(1) >> 16 & 0xFF); g.minute = (SInt8)(ARG(1) >> 24); g.second = argd(c, 2);
    FPUSH(CFGregorianDateGetAbsoluteTime(g, zone(ARG(4))));
}
void I__CFAbsoluteTimeGetDayOfWeek(CPU *c) { RET(CFAbsoluteTimeGetDayOfWeek(argd(c, 0), zone(ARG(2)))); }

// ---------- CoreGraphics functions that return a structure ----------
// A CGRect is four CGFloat: `float` in the guest. It comes back through a hidden pointer, the first argument,
// which the callee takes off the stack. The bridge answered zero and left the pointer there, and everything the
// caller pushed afterwards was one slot off.
#include <CoreGraphics/CoreGraphics.h>
static void rect_out(CPU *c, CGRect r) {
    uint32_t out = ARG(0);
    wrf(out, (float)r.origin.x); wrf(out + 4, (float)r.origin.y); wrf(out + 8, (float)r.size.width); wrf(out + 12, (float)r.size.height);
    RET(out);
    c->esp += 4;
}
void I__CGDisplayBounds(CPU *c) { rect_out(c, CGDisplayBounds((CGDirectDisplayID)ARG(1))); }
