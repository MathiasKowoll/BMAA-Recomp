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
