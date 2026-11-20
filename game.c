// Batman: Arkham Asylum GOTY: what is this game's and not the runtime's.
//
// The two functions DecompRecomp's notes neutralise for this executable (its reference invocation passes them to
// the interpreter as --skip-func): with translated code they are listed in hle.txt and answered here. They return
// zero and do nothing, which is what the interpreter's skip did.
#include "rt.h"
#include <stdio.h>

void H_skip_1c05da0(CPU *c) { c->eax = 0; }
void H_skip_1b9b190(CPU *c) { c->eax = 0; }

// The start-up stage at 0x45320 (the application object's virtual method at +0x174) loads five PNGs for Feral's
// achievement frame through ImageIO and fails the whole start-up if one does not load. The runtime's bridge does
// not decode images yet (CGImageSourceCreateWithData is turned away), so the stage is answered "done" and the
// frame is not there. To take out when images load: an achievement shown without its frame is the cost.
void aot_override(uint32_t addr, void (*fn)(CPU *));
static void stage_achievement_frame(CPU *c) {
    static int said;
    if (!said++) fprintf(stderr, "  [bmaa] stage 0x45320 (the achievement frame's images) skipped: the bridge does not decode images yet\n");
    c->eax = 0;
}
void game_init(void) { aot_override(0x45320, stage_achievement_frame); }

// sub_1c97620(CFSTR("X.framework"), CFBundleRef *out) -> 0 when it has the bundle: Feral's way to a system library's
// functions by name. It walks there with FSFindFolder and FSRef URLs, which the bridge does not carry far enough,
// and the engine uses it for `free` itself: every release of memory came here, failed, and released nothing.
// Answered with the bundle of /System/Library/Frameworks/X.framework, as a handle; the functions are then found by
// I__CFBundleGetFunctionPointerForName (aot.c).
#include "dr_bridge.h"
#include "dr_objc.h"
#include <CoreFoundation/CoreFoundation.h>
#include <string.h>
void aot_lock(void);
void aot_unlock(void);
dr_objc *dr_objc_actual(void);
void H_open_system_bundle(CPU *c) {
    uint32_t s = ARG(0), out = ARG(1); char name[64] = "";
    if (s && s < DR_OBJC_HANDLE_BASE) {   // a constant string of the game's: { isa, flags, bytes, length }
        uint32_t bytes = rd32(s + 8), len = rd32(s + 12);
        if (bytes && len < sizeof name) { memcpy(name, G2H(bytes), len); name[len] = 0; }
    }
    if (!name[0] || !out) { c->eax = (uint32_t)-1; return; }
    // a new reference each time: the game releases the bundle once it has the function it wanted
    char path[160]; snprintf(path, sizeof path, "/System/Library/Frameworks/%s", name);
    CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), true);
    CFBundleRef bundle = u ? CFBundleCreate(NULL, u) : NULL;
    if (u) CFRelease(u);
    aot_lock();
    uint32_t h = bundle ? dr_objc_handle_for(dr_objc_actual(), (void *)bundle) : 0;
    aot_unlock();
    if (h) { wr32(out, h); c->eax = 0; } else c->eax = (uint32_t)-1;
}
