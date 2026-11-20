// The displays and their modes, answered by the Mac's own. A display id, a width or a flag is a 32-bit number in both
// worlds; what the bridge's generic route cannot carry is a CGRect returned through a hidden pointer, a refresh rate
// returned as a double, and the objects (modes, arrays of modes), which go to the game as handles.
// The gamma table is read but never written: setting it would change the whole screen of whoever is sitting there.
#include "dr_bridge.h"
#include "dr_objc.h"
#include "rt.h"
#include <Carbon/Carbon.h>
#include <CoreGraphics/CoreGraphics.h>
#include <OpenGL/OpenGL.h>
#include <stdlib.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

void aot_lock(void);
void aot_unlock(void);
dr_objc *dr_objc_actual(void);
#define RET(v) (c->eax = (uint32_t)(v))
static void *host(uint32_t v) {
    if (!v) return NULL;
    aot_lock(); void *h = dr_objc_object_for(dr_objc_actual(), v); aot_unlock();
    return h;
}
static uint32_t handle(const void *h) {
    if (!h) return 0;
    aot_lock(); uint32_t v = dr_objc_handle_for(dr_objc_actual(), (void *)h); aot_unlock();
    return v;
}
static void rect_out(uint32_t g, CGRect r) {
    wrf(g, (float)r.origin.x); wrf(g + 4, (float)r.origin.y); wrf(g + 8, (float)r.size.width); wrf(g + 12, (float)r.size.height);
}
static CGRect rect_in(CPU *c, int slot) { return CGRectMake(ARGF(slot), ARGF(slot + 1), ARGF(slot + 2), ARGF(slot + 3)); }

void I__CGMainDisplayID(CPU *c) { RET(CGMainDisplayID()); }
void I__CGGetActiveDisplayList(CPU *c) {
    CGDirectDisplayID ids[32]; uint32_t n = 0, max = ARG(0) > 32 ? 32 : ARG(0);
    CGError e = CGGetActiveDisplayList(max, ARG(1) ? ids : NULL, &n);
    if (ARG(1)) for (uint32_t i = 0; i < n && i < max; i++) wr32(ARG(1) + 4 * i, ids[i]);
    if (ARG(2)) wr32(ARG(2), n);
    RET(e);
}
// (CGDisplayBounds is in dates.c)
// CGSize f(display): two floats, eight bytes, come back in eax:edx
void I__CGDisplayScreenSize(CPU *c) {
    CGSize s = CGDisplayScreenSize(ARG(0)); float w = (float)s.width, h = (float)s.height;
    memcpy(&c->eax, &w, 4); memcpy(&c->edx, &h, 4);
}
void I__CGDisplayIDToOpenGLDisplayMask(CPU *c) { RET(CGDisplayIDToOpenGLDisplayMask(ARG(0))); }
void I__CGDisplayIOServicePort(CPU *c) { RET(0); }   // gone from the system; the game asks it for the monitor's name only
void I__CGDisplayCopyDisplayMode(CPU *c) { RET(handle(CGDisplayCopyDisplayMode(ARG(0)))); }
void I__CGDisplayCopyAllDisplayModes(CPU *c) { RET(handle(CGDisplayCopyAllDisplayModes(ARG(0), NULL))); }
void I__CGDisplayModeGetWidth(CPU *c)  { CGDisplayModeRef m = host(ARG(0)); RET(m ? CGDisplayModeGetWidth(m) : 0); }
void I__CGDisplayModeGetHeight(CPU *c) { CGDisplayModeRef m = host(ARG(0)); RET(m ? CGDisplayModeGetHeight(m) : 0); }
void I__CGDisplayModeGetIOFlags(CPU *c) { CGDisplayModeRef m = host(ARG(0)); RET(m ? CGDisplayModeGetIOFlags(m) : 0); }
void I__CGDisplayModeGetRefreshRate(CPU *c) {
    CGDisplayModeRef m = host(ARG(0));
    double hz = m ? CGDisplayModeGetRefreshRate(m) : 0;
    FPUSH(hz > 0 ? hz : 60.0);   // a built-in panel says 0 for "no fixed rate"; a 2011 engine divides by it
}
void I__CGDisplayModeCopyPixelEncoding(CPU *c) { CGDisplayModeRef m = host(ARG(0)); RET(m ? handle(CGDisplayModeCopyPixelEncoding(m)) : 0); }
void I__CGDisplayModeRelease(CPU *c) { }   // the handle keeps the object; a mode is a few bytes
void I__CGGetDisplayTransferByTable(CPU *c) {
    uint32_t cap = ARG(1) > 1024 ? 1024 : ARG(1), n = 0;
    static CGGammaValue r[1024], g[1024], b[1024];
    CGError e = CGGetDisplayTransferByTable(ARG(0), cap, r, g, b, &n);
    for (uint32_t i = 0; i < n && i < cap; i++) {
        if (ARG(2)) wrf(ARG(2) + 4 * i, r[i]);
        if (ARG(3)) wrf(ARG(3) + 4 * i, g[i]);
        if (ARG(4)) wrf(ARG(4) + 4 * i, b[i]);
    }
    if (ARG(5)) wr32(ARG(5), n);
    RET(e);
}
void I__CGSetDisplayTransferByTable(CPU *c) { RET(0); }
void I__CGDisplayRestoreColorSyncSettings(CPU *c) { }
void I__CGDisplayRegisterReconfigurationCallback(CPU *c) { RET(0); }
void I__CGDisplayRemoveReconfigurationCallback(CPU *c) { RET(0); }
void I__CGWindowLevelForKey(CPU *c) { RET(CGWindowLevelForKey((CGWindowLevelKey)ARG(0))); }
void I__CGEventSourceKeyState(CPU *c) { RET(CGEventSourceKeyState((CGEventSourceStateID)ARG(0), (CGKeyCode)ARG(1)) ? 1 : 0); }

// The pointer is only moved or held when this process really is the one in front: a run that says it is in front
// for a test (BMAA_ALWAYS_FRONT) must not take the pointer from whoever is using the Mac.
static int really_front(void) {
    ProcessSerialNumber me, front; Boolean same = 0;
    return GetCurrentProcess(&me) == noErr && GetFrontProcess(&front) == noErr && SameProcess(&me, &front, &same) == noErr && same;
}
void I__CGSetLocalEventsSuppressionInterval(CPU *c) { RET(0); }   // (double seconds): nothing is suppressed after a warp nowadays
void I__CGWarpMouseCursorPosition(CPU *c) {   // (CGPoint: two floats)
    RET(really_front() ? CGWarpMouseCursorPosition(CGPointMake(ARGF(0), ARGF(1))) : 0);
}
void I__CGAssociateMouseAndMouseCursorPosition(CPU *c) {
    RET(really_front() || ARG(0) ? CGAssociateMouseAndMouseCursorPosition(ARG(0) != 0) : 0);
}

// CGFloat f(CGRect by value): the answer on the x87 stack
void I__CGRectGetMinX(CPU *c) { FPUSH(CGRectGetMinX(rect_in(c, 0))); }
void I__CGRectGetMinY(CPU *c) { FPUSH(CGRectGetMinY(rect_in(c, 0))); }
void I__CGRectGetMidX(CPU *c) { FPUSH(CGRectGetMidX(rect_in(c, 0))); }
void I__CGRectGetMidY(CPU *c) { FPUSH(CGRectGetMidY(rect_in(c, 0))); }
void I__CGRectGetMaxX(CPU *c) { FPUSH(CGRectGetMaxX(rect_in(c, 0))); }
void I__CGRectGetMaxY(CPU *c) { FPUSH(CGRectGetMaxY(rect_in(c, 0))); }
