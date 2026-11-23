// The displays and their modes, answered by the Mac's own. A display id, a width or a flag is a 32-bit number in both
// worlds; what the bridge's generic route cannot carry is a CGRect returned through a hidden pointer, a refresh rate
// returned as a double, and the objects (modes, arrays of modes), which go to the game as handles.
// The gamma table is read but never written: setting it would change the whole screen of whoever is sitting there.
#include <stdatomic.h>
#include "dr_bridge.h"
#include "dr_objc.h"
#include "rt.h"
#include <Carbon/Carbon.h>
#include <CoreGraphics/CoreGraphics.h>
#include <OpenGL/OpenGL.h>
#include <stdlib.h>
#include <time.h>
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

// The pointer is only moved or held when this process really is the one in front: a run that says it is in front
// for a test (BMAA_ALWAYS_FRONT) must not take the pointer from whoever is using the Mac.
static int really_front(void) {
    ProcessSerialNumber me, front; Boolean same = 0;
    return GetCurrentProcess(&me) == noErr && GetFrontProcess(&front) == noErr && SameProcess(&me, &front, &same) == noErr && same;
}
// The keyboard's state is the whole Mac's: a test run kept behind (BMAA_BACKGROUND) must not read what is being typed
// into another application as its own keys.
void I__CGEventSourceKeyState(CPU *c) {
    static int bg = -1; if (bg < 0) bg = getenv("BMAA_BACKGROUND") != NULL;
    if (bg && !really_front()) { RET(0); return; }
    RET(CGEventSourceKeyState((CGEventSourceStateID)ARG(0), (CGKeyCode)ARG(1)) ? 1 : 0);
}
// (double seconds): after a warp the system holds the mouse's movement back for a quarter of a second unless told
// otherwise. A game that warps the pointer to the middle on every frame would then never see the mouse move, so
// what it asks for is passed on.
void I__CGSetLocalEventsSuppressionInterval(CPU *c) {
    double s = ARGD(0);
    if (getenv("BMAA_TRACE_MOUSE")) fprintf(stderr, "  [mouse] the game sets the pause after a warp to %g s\n", s);
    CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStateCombinedSessionState);
    if (src) { CGEventSourceSetLocalEventsSuppressionInterval(src, s); CFRelease(src); }
    RET(0);
}
void bmaa_mouse_warped(void);
void I__CGWarpMouseCursorPosition(CPU *c) {   // (CGPoint: two floats)
    static unsigned n;
    if (getenv("BMAA_TRACE_MOUSE") && (n++ < 5 || n % 600 == 0)) fprintf(stderr, "  [mouse] warp %u to %g,%g (in front: %d)\n", n, ARGF(0), ARGF(1), really_front());
    if (really_front()) bmaa_mouse_warped();
    RET(really_front() ? CGWarpMouseCursorPosition(CGPointMake(ARGF(0), ARGF(1))) : 0);
}
// The game holds the pointer while it is played (the mouse then only turns the camera) and lets it go for its menus:
// one call each way. What it asked for is remembered and applied whenever this process is the one in front, so that a
// request made a moment before the window came forward is not lost, and the pointer is given back (and shown) for as
// long as another application is in front. While held, the pointer is hidden too: a held pointer drawn in the middle
// of the picture is no use to anyone. BMAA_TRACE_MOUSE=1 prints each request.
// A controller in use holds it as well: with a pad the game does not ask (it leaves the pointer free and, in the
// original, drew its own empty cursor image, which the bridge does not carry), so the system's arrow sat over the
// picture and a nudge of the mouse could click outside the game. Moving the mouse gives the pointer back, for the
// menus; the next press on the pad takes it again.
// Played with the mouse, the game does neither: it puts the pointer back in the middle on every frame and, in the
// original, hid it behind the same empty image. So a pointer that is being put back is hidden too, and shown again
// half a second after the game stops (its menus, where the pointer is the game's to use).
static int mouse_wants_held, mouse_is_held, mouse_is_hidden; static _Atomic int pad_in_use; static _Atomic uint64_t warped_at;
void bmaa_mouse_apply(void);
void bmaa_pad_used(void) { if (!pad_in_use) { pad_in_use = 1; bmaa_mouse_apply(); } }
void bmaa_mouse_moved(void) { if (pad_in_use) { pad_in_use = 0; bmaa_mouse_apply(); } }
void bmaa_mouse_warped(void) { warped_at = clock_gettime_nsec_np(CLOCK_MONOTONIC); if (!mouse_is_hidden) bmaa_mouse_apply(); }
void bmaa_mouse_apply(void) {
    int front = really_front();
    int hold = (mouse_wants_held || pad_in_use) && front;
    int hide = hold || (front && warped_at && clock_gettime_nsec_np(CLOCK_MONOTONIC) - warped_at < 500000000ull);
    if (hold != mouse_is_held) { mouse_is_held = hold; CGAssociateMouseAndMouseCursorPosition(!hold); }
    if (hide == mouse_is_hidden) return;
    mouse_is_hidden = hide;
    if (hide) CGDisplayHideCursor(CGMainDisplayID()); else CGDisplayShowCursor(CGMainDisplayID());
    if (getenv("BMAA_TRACE_MOUSE")) fprintf(stderr, "  [mouse] the pointer is %s and %s\n", hold ? "held" : "free", hide ? "hidden" : "shown");
}
void I__CGAssociateMouseAndMouseCursorPosition(CPU *c) {
    mouse_wants_held = ARG(0) == 0;
    if (getenv("BMAA_TRACE_MOUSE")) fprintf(stderr, "  [mouse] the game asks for the pointer %s (in front: %d)\n", mouse_wants_held ? "held" : "free", really_front());
    bmaa_mouse_apply();
    RET(0);
}

// CGFloat f(CGRect by value): the answer on the x87 stack
void I__CGRectGetMinX(CPU *c) { FPUSH(CGRectGetMinX(rect_in(c, 0))); }
void I__CGRectGetMinY(CPU *c) { FPUSH(CGRectGetMinY(rect_in(c, 0))); }
void I__CGRectGetMidX(CPU *c) { FPUSH(CGRectGetMidX(rect_in(c, 0))); }
void I__CGRectGetMidY(CPU *c) { FPUSH(CGRectGetMidY(rect_in(c, 0))); }
void I__CGRectGetMaxX(CPU *c) { FPUSH(CGRectGetMaxX(rect_in(c, 0))); }
void I__CGRectGetMaxY(CPU *c) { FPUSH(CGRectGetMaxY(rect_in(c, 0))); }
