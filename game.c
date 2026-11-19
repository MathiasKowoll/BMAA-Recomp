// Batman: Arkham Asylum GOTY: what is this game's and not the runtime's.
//
// The two functions DecompRecomp's notes neutralise for this executable (its reference invocation passes them to
// the interpreter as --skip-func): with translated code they are listed in hle.txt and answered here. They return
// zero and do nothing, which is what the interpreter's skip did.
#include "rt.h"

void H_skip_1c05da0(CPU *c) { c->eax = 0; }
void H_skip_1b9b190(CPU *c) { c->eax = 0; }
